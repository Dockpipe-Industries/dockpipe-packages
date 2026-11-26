#include "NativeVirtualDisplayObserver.h"
#include "NativeVirtualDisplayOwnerProtocol.h"
#include "NativeVirtualDisplaySnapshot.h"
#include "worker/ChildProcess.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace dockpipe::desktop {
namespace {
using Clock = NativeVirtualDisplay::Clock;
using Json = nlohmann::json;
using namespace std::chrono_literals;
void requireObserver(bool value)
{
    if (!value)
        throw std::runtime_error("Fresh virtual display observation failed.");
}
}

struct VirtualDisplayModeObserver::State {
    explicit State(VirtualDisplayOwnerContext value) : context(std::move(value)) {}
    VirtualDisplayOwnerContext context;
    std::unique_ptr<ChildProcess> child;
    RecordReader reader;
    NativeVirtualDisplaySnapshot snapshot;
    std::optional<NativeVirtualMode> verifiedMode;
    std::string directory;
    std::string temporary;
    bool used = false;
    bool spawnFailed = false;
    bool snapshotAdmitted = false;
    bool reaped = false;
    bool filesClean = false;
    bool cleanupReported = false;

    Json waitMessage(Clock::time_point deadline, const NativeVirtualDisplay::SelectionAdmission &admission)
    {
        while (Clock::now() < deadline && admission && admission()) {
            const auto outcome = reader.poll(child->output(), VirtualDisplayOwnerRecordLimit, true);
            // A read-only observer may finish before its final pipe record is
            // consumed. Read buffered data before inspecting its exit state.
            if (outcome == RecordReader::Outcome::Complete) {
                const auto bytes = reader.take();
                return parseVirtualDisplayOwnerJson(std::string(bytes.begin(), bytes.end()));
            }
            // Exit can race a pending read. The next bounded pipe poll
            // distinguishes a buffered final record from actual EOF.
            requireObserver(outcome == RecordReader::Outcome::Pending);
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("Fresh virtual display observation expired.");
    }

    void removeFiles(Clock::time_point deadline)
    {
        if (filesClean)
            return;
        if (directory.empty()) {
            filesClean = true;
            return;
        }
        filesClean = snapshotAdmitted ? snapshot.cleanupAfterReap()
            : (rmdir(temporary.c_str()) == 0 || errno == ENOENT);
        if (filesClean) {
            filesClean = (unlink((directory + "/child-host.json").c_str()) == 0 || errno == ENOENT)
                && rmdir(directory.c_str()) == 0;
        }
        if (!filesClean && !cleanupReported) {
            cleanupReported = true;
            const char message[] = "Native virtual display observer snapshot cleanup remains unverified.\n";
            writeBytes(STDERR_FILENO, reinterpret_cast<const std::uint8_t *>(message), sizeof(message) - 1,
                std::min(deadline, Clock::now() + 10ms));
            const int output = open((directory + "/snapshot-cleanup-unknown.json").c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (output >= 0) {
                const char record[] = "{\"schema\":\"dockpipe.desktop.snapshot-cleanup/v1\",\"verified\":false}\n";
                const auto ignored = write(output, record, sizeof(record) - 1);
                (void)ignored;
                ::close(output);
            }
        }
    }
};

VirtualDisplayModeObserver::VirtualDisplayModeObserver(VirtualDisplayOwnerContext context)
    : m_state(std::make_unique<State>(std::move(context)))
{
}
VirtualDisplayModeObserver::~VirtualDisplayModeObserver()
{
    try {
        close(Clock::now());
    } catch (...) {
        // Never introduce a destructor wait budget after an incomplete close.
    }
}

const std::optional<NativeVirtualMode> &VirtualDisplayModeObserver::mode() const
{
    return m_state->verifiedMode;
}

bool VirtualDisplayModeObserver::observe(const VirtualDisplayObserverRequest &request, Clock::time_point deadline,
    const NativeVirtualDisplay::SelectionAdmission &admission)
{
    auto &state = *m_state;
    if (state.used)
        return false;
    state.used = true;
    try {
        parseVirtualDisplayObserverRequest(virtualDisplayObserverProfile(request));
        requireObserver(request.parentProcess == getpid() && admission && admission() && Clock::now() < deadline
            && state.context.materializeDescriptor && state.context.verifySnapshot
            && !state.context.hostExecutable.empty() && state.context.hostExecutable.front() == '/');
#ifdef __APPLE__
        char pattern[] = "/private/tmp/dockpipe-virtual-owner-observer-XXXXXX";
#else
        char pattern[] = "/tmp/dockpipe-virtual-owner-observer-XXXXXX";
#endif
        requireObserver(mkdtemp(pattern));
        state.directory = pattern;
        char canonical[PATH_MAX];
        requireObserver(realpath(pattern, canonical));
        state.directory = canonical;
        const auto descriptor = state.context.materializeDescriptor(state.directory, virtualDisplayObserverProfile(request));
        requireObserver(descriptor == state.directory + "/child-host.json");
        struct stat metadata{};
        requireObserver(lstat(descriptor.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode)
            && metadata.st_uid == getuid() && (metadata.st_mode & 0777) == 0600 && metadata.st_size <= 65536);
        state.temporary = state.directory + "/host-tmp";
        requireObserver(mkdir(state.temporary.c_str(), 0700) == 0 && admission() && Clock::now() < deadline);
        try {
            state.child = ChildProcess::start({state.context.hostExecutable, "--native-worker-host", "--request", descriptor},
                false, state.temporary);
        } catch (...) {
            state.spawnFailed = true;
            throw;
        }
        const auto admitted = state.waitMessage(deadline, admission);
        validateVirtualDisplayObserverMessage(admitted, request, "admitted", 8);
        requireObserver(virtualDisplayOwnerNumber(admitted.at("pid"), INT_MAX) == static_cast<unsigned>(state.child->pid())
            && admitted.at("snapshot_path").is_string());
        state.snapshotAdmitted = state.snapshot.admit(state.temporary, admitted.at("snapshot_path").get<std::string>(),
            state.context.verifySnapshot);
        requireObserver(state.snapshotAdmitted && admission() && Clock::now() < deadline);
        const auto command = virtualDisplayObserverMessage(request, "observe").dump() + "\n";
        requireObserver(writeBytes(state.child->input(), reinterpret_cast<const std::uint8_t *>(command.data()),
            command.size(), deadline));
        const auto observed = state.waitMessage(deadline, admission);
        const auto mode = parseVirtualDisplayObservation(observed, request, state.child->pid());
        bool successfulExit = false;
        while (Clock::now() < deadline && admission()) {
            siginfo_t status{};
            requireObserver(waitid(P_PID, static_cast<id_t>(state.child->pid()), &status,
                WEXITED | WNOHANG | WNOWAIT) == 0);
            if (status.si_pid == state.child->pid()) {
                successfulExit = status.si_code == CLD_EXITED && status.si_status == 0;
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        requireObserver(successfulExit && admission() && Clock::now() < deadline);
        requireObserver(close(deadline) && state.filesClean && admission() && Clock::now() < deadline);
        state.verifiedMode = mode;
        return true;
    } catch (...) {
        // An unsuccessful observer remains owned for the caller's shared native
        // cleanup deadline. Do not start a new wait budget here or in a retry.
        if (!state.child && !state.spawnFailed)
            state.removeFiles(deadline);
        return false;
    }
}

bool VirtualDisplayModeObserver::close(Clock::time_point deadline)
{
    auto &state = *m_state;
    if (!state.reaped) {
        state.reaped = state.child ? state.child->stop(deadline) : !state.spawnFailed;
        if (state.reaped)
            state.removeFiles(deadline);
    }
    // Inert file retention is reported separately from process ownership. It
    // blocks positive observe(), but is not a false display-cleanup claim.
    return state.reaped;
}
} // namespace dockpipe::desktop
