#include "ChildProcess.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <limits.h>
#include <sys/stat.h>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

extern char **environ;
namespace dockpipe::desktop {
namespace {
class PipeSignalMask final {
public:
    PipeSignalMask()
    {
        sigemptyset(&m_pipe);
        sigaddset(&m_pipe, SIGPIPE);
        m_valid = pthread_sigmask(SIG_BLOCK, &m_pipe, &m_previous) == 0;
        sigset_t pending;
        m_previouslyPending = !m_valid || sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) == 1;
    }

    ~PipeSignalMask()
    {
        if (m_valid)
            pthread_sigmask(SIG_SETMASK, &m_previous, nullptr);
    }

    bool valid() const { return m_valid; }

    void consumeNewBrokenPipe()
    {
        if (m_previouslyPending)
            return;
        sigset_t pending;
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
            int received = 0;
            sigwait(&m_pipe, &received);
        }
    }

private:
    sigset_t m_pipe{};
    sigset_t m_previous{};
    bool m_valid = false;
    bool m_previouslyPending = true;
};

void validateTemporaryDirectory(const std::string &path)
{
    char canonical[PATH_MAX];
    if (path.empty() || path.front() != '/' || path.size() >= sizeof(canonical)
        || path.find('\0') != std::string::npos || !realpath(path.c_str(), canonical) || path != canonical)
        throw std::runtime_error("Child temporary directory must be canonical.");
    const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat opened{};
    struct stat named{};
    const bool valid = descriptor >= 0 && fstat(descriptor, &opened) == 0
        && lstat(path.c_str(), &named) == 0 && S_ISDIR(opened.st_mode) && S_ISDIR(named.st_mode)
        && opened.st_uid == getuid() && (opened.st_mode & 0777) == 0700
        && named.st_dev == opened.st_dev && named.st_ino == opened.st_ino;
    if (descriptor >= 0)
        close(descriptor);
    if (!valid)
        throw std::runtime_error("Child temporary directory must be private and owned.");
}

std::vector<std::string> temporaryDirectoryEnvironment(const std::string &path)
{
    validateTemporaryDirectory(path);
    constexpr std::size_t MaximumEntries = 4096;
    constexpr std::size_t MaximumEntryBytes = 65536;
    constexpr std::size_t MaximumEnvironmentBytes = 256 * 1024;
    std::vector<std::string> values;
    std::size_t total = path.size() + 8;
    std::size_t entries = 0;
    for (char **entry = environ; entry && *entry; ++entry) {
        if (++entries > MaximumEntries)
            throw std::runtime_error("Child environment exceeds its entry bound.");
        const auto length = strnlen(*entry, MaximumEntryBytes + 1);
        if (length > MaximumEntryBytes || total + length + 1 > MaximumEnvironmentBytes)
            throw std::runtime_error("Child environment exceeds its byte bound.");
        total += length + 1;
        if (std::strncmp(*entry, "TMPDIR=", 7) != 0)
            values.emplace_back(*entry, length);
    }
    values.push_back("TMPDIR=" + path);
    return values;
}

void closeDescriptor(int &descriptor)
{
    if (descriptor >= 0)
        close(descriptor);
    descriptor = -1;
}
void newPipe(int (&descriptors)[2])
{
    int original[2];
    if (pipe(original) != 0)
        throw std::runtime_error("Cannot allocate private process pipe.");
    descriptors[0] = fcntl(original[0], F_DUPFD_CLOEXEC, 10);
    descriptors[1] = fcntl(original[1], F_DUPFD_CLOEXEC, 10);
    close(original[0]);
    close(original[1]);
    if (descriptors[0] < 0 || descriptors[1] < 0) {
        closeDescriptor(descriptors[0]);
        closeDescriptor(descriptors[1]);
        throw std::runtime_error("Cannot protect private process pipe.");
    }
}
}
std::unique_ptr<ChildProcess> ChildProcess::start(const std::vector<std::string> &arguments,
    bool channelDescriptors, const std::string &temporaryDirectory)
{
    if (arguments.empty() || arguments.front().empty() || arguments.front().front() != '/')
        throw std::runtime_error("A trusted absolute executable is required.");
    std::vector<std::string> environment;
    std::vector<char *> environmentPointers;
    char **childEnvironment = environ;
    if (!temporaryDirectory.empty()) {
        environment = temporaryDirectoryEnvironment(temporaryDirectory);
        environmentPointers.reserve(environment.size() + 1);
        for (auto &value : environment)
            environmentPointers.push_back(value.data());
        environmentPointers.push_back(nullptr);
        childEnvironment = environmentPointers.data();
    }
    int input[2]{-1, -1};
    int output[2]{-1, -1};
    int control[2]{-1, -1};
    int status[2]{-1, -1};
    int nullDescriptor = -1;
    auto result = std::unique_ptr<ChildProcess>(new ChildProcess);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    try {
        newPipe(input);
        newPipe(output);
        if (channelDescriptors) {
            newPipe(control);
            newPipe(status);
        }
        const int originalNull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        nullDescriptor = originalNull >= 0 ? fcntl(originalNull, F_DUPFD_CLOEXEC, 10) : -1;
        if (originalNull >= 0)
            close(originalNull);
        if (nullDescriptor < 0)
            throw std::runtime_error("Cannot isolate child diagnostics.");
        posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, nullDescriptor, STDERR_FILENO);
        if (channelDescriptors) {
            posix_spawn_file_actions_adddup2(&actions, control[0], 3);
            posix_spawn_file_actions_adddup2(&actions, status[1], 4);
        }
        for (int descriptor : {input[0], input[1], output[0], output[1], control[0], control[1], status[0], status[1], nullDescriptor}) {
            if (descriptor >= 0)
                posix_spawn_file_actions_addclose(&actions, descriptor);
        }
        std::vector<char *> argv;
        for (const auto &argument : arguments)
            argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        short spawnFlags = POSIX_SPAWN_SETPGROUP;
#ifdef __APPLE__
        spawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
        posix_spawnattr_setflags(&attributes, spawnFlags);
        posix_spawnattr_setpgroup(&attributes, 0);
        const int error = posix_spawn(&result->m_pid, argv.front(), &actions, &attributes, argv.data(), childEnvironment);
        posix_spawnattr_destroy(&attributes);
        if (error != 0)
            throw std::runtime_error("Cannot start trusted native child.");
        result->m_input = input[1]; input[1] = -1;
        result->m_output = output[0]; output[0] = -1;
        result->m_control = control[1]; control[1] = -1;
        result->m_status = status[0]; status[0] = -1;
        for (int descriptor : {result->m_input, result->m_output, result->m_control, result->m_status}) {
            if (descriptor >= 0) {
                const int flags = fcntl(descriptor, F_GETFL);
                if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
                    throw std::runtime_error("Cannot make private process pipe cancellable.");
            }
        }
    } catch (...) {
        posix_spawn_file_actions_destroy(&actions);
        for (int *descriptor : {&input[0], &input[1], &output[0], &output[1], &control[0], &control[1], &status[0], &status[1], &nullDescriptor})
            closeDescriptor(*descriptor);
        throw;
    }
    posix_spawn_file_actions_destroy(&actions);
    for (int *descriptor : {&input[0], &input[1], &output[0], &output[1], &control[0], &control[1], &status[0], &status[1], &nullDescriptor})
        closeDescriptor(*descriptor);
    return result;
}
ChildProcess::~ChildProcess() { stop(); }
bool ChildProcess::exited()
{
    if (m_reaped || m_pid <= 0)
        return true;
    siginfo_t information{};
    // Observe without reaping. Holding the PID prevents group-ID reuse until
    // stop() has killed any surviving relay descendant and reaped the owner.
    const int result = waitid(P_PID, static_cast<id_t>(m_pid), &information, WEXITED | WNOHANG | WNOWAIT);
    return result == 0 && information.si_pid == m_pid;
}
bool ChildProcess::childIdentityPinned()
{
    if (m_reaped || m_pid <= 0)
        return false;
    siginfo_t information{};
    if (waitid(P_PID, static_cast<id_t>(m_pid), &information, WEXITED | WNOHANG | WNOWAIT) == 0)
        return true; // Running or unreaped exited child; the PID remains ours.
    if (errno == ECHILD)
        m_reaped = true; // Lost ownership is not permission to signal a reused ID.
    return false;
}

bool ChildProcess::stop()
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    if (m_explicitStopDeadline)
        deadline = std::min(deadline, *m_explicitStopDeadline);
    return stopWithin(deadline);
}

bool ChildProcess::stop(std::chrono::steady_clock::time_point deadline)
{
    if (!m_explicitStopDeadline || deadline < *m_explicitStopDeadline)
        m_explicitStopDeadline = deadline;
    return stopWithin(*m_explicitStopDeadline);
}

bool ChildProcess::stopWithin(std::chrono::steady_clock::time_point deadline)
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    closeDescriptor(m_input);
    closeDescriptor(m_output);
    closeDescriptor(m_control);
    closeDescriptor(m_status);
    if (m_reaped || m_pid <= 0)
        return m_groupVerified || m_pid <= 0;
    if (!childIdentityPinned())
        return false;

    const auto initial = Clock::now();
    // Preserve the default 2.5s TERM/0.5s reap limits, but a short explicit
    // deadline reserves its remaining time for forced stop and proof.
    auto gracefulDeadline = initial;
    if (deadline > initial) {
        const auto available = deadline - initial;
        if (available > 500ms)
            gracefulDeadline += std::min(available - 500ms, std::chrono::duration_cast<Clock::duration>(2500ms));
    }
    if (!exited() && initial < deadline) {
        kill(-m_pid, SIGTERM);
        while (Clock::now() < gracefulDeadline && !exited()) {
            const auto remaining = gracefulDeadline - Clock::now();
            if (remaining > Clock::duration::zero())
                std::this_thread::sleep_for(std::min(remaining, std::chrono::duration_cast<Clock::duration>(5ms)));
        }
    }
    // Recheck the unreaped-child pin before the last group signal. No signal
    // may follow reaping, even if another waiter unexpectedly consumed it.
    if (!childIdentityPinned())
        return false;
    kill(-m_pid, SIGKILL);
    const auto reapDeadline = std::min(deadline, Clock::now() + 500ms);
    for (;;) {
        if (!m_reaped) {
            int status = 0;
            const auto result = waitpid(m_pid, &status, WNOHANG);
            if (result == m_pid || (result < 0 && errno == ECHILD))
                m_reaped = true;
        }
        // A zero-time/expired call can initiate bounded cleanup, but cannot
        // invent a fresh wait budget or report newly verified late cleanup.
        if (m_reaped && kill(-m_pid, 0) < 0 && errno == ESRCH && Clock::now() < deadline) {
            m_groupVerified = true;
            return true;
        }
        const auto now = Clock::now();
        if (now >= reapDeadline)
            return false;
        const auto remaining = reapDeadline - now;
        std::this_thread::sleep_for(std::min(remaining, std::chrono::duration_cast<Clock::duration>(5ms)));
    }
}
bool writeBytes(int descriptor, const std::uint8_t *bytes, std::size_t length, std::chrono::steady_clock::time_point deadline)
{
    // This can run inside the host App. Never change its global SIGPIPE handler
    // or leave a new thread-directed broken-pipe signal pending on return.
    PipeSignalMask signals;
    if (!signals.valid())
        return false;
    while (length > 0 && std::chrono::steady_clock::now() < deadline) {
        pollfd item{descriptor, POLLOUT, 0};
        const auto ready = ::poll(&item, 1, 50);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready > 0 && !(item.revents & POLLOUT)))
            return false;
        if (ready == 0)
            continue;
        const auto count = write(descriptor, bytes, length);
        if (count < 0 && errno == EPIPE)
            signals.consumeNewBrokenPipe();
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            return false;
        bytes += count;
        length -= static_cast<std::size_t>(count);
    }
    return length == 0;
}
bool writeRecord(int descriptor, const std::vector<std::uint8_t> &value, std::size_t maximum, std::chrono::steady_clock::time_point deadline)
{
    if (value.empty() || value.size() > maximum)
        return false;
    const auto size = static_cast<std::uint32_t>(value.size());
    const std::uint8_t header[]{static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    return writeBytes(descriptor, header, sizeof(header), deadline) && writeBytes(descriptor, value.data(), value.size(), deadline);
}
RecordReader::Outcome RecordReader::poll(int descriptor, std::size_t maximum, bool lines)
{
    if (descriptor < 0)
        return Outcome::Closed;
    std::uint8_t bytes[65536];
    for (unsigned iteration = 0; iteration < 65536; ++iteration) {
        if (m_buffer.size() > maximum + 4)
            return Outcome::Invalid;
        if (lines && !m_buffer.empty() && m_buffer.back() == '\n')
            return Outcome::Complete;
        if (!lines && m_buffer.size() >= 4) {
            if (m_expected == 0) {
                m_expected = (std::size_t(m_buffer[0]) << 24) | (std::size_t(m_buffer[1]) << 16) | (std::size_t(m_buffer[2]) << 8) | m_buffer[3];
                if (m_expected == 0 || m_expected > maximum)
                    return Outcome::Invalid;
            }
            if (m_buffer.size() == m_expected + 4)
                return Outcome::Complete;
        }
        std::size_t wanted = 1;
        if (!lines)
            wanted = m_buffer.size() < 4 ? 4 - m_buffer.size() : std::min(sizeof(bytes), m_expected + 4 - m_buffer.size());
        const auto count = read(descriptor, bytes, wanted);
        if (count == 0)
            return m_buffer.empty() ? Outcome::Closed : Outcome::Invalid;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno == EAGAIN ? Outcome::Pending : Outcome::Invalid;
        }
        m_buffer.insert(m_buffer.end(), bytes, bytes + count);
    }
    return Outcome::Pending;
}
std::vector<std::uint8_t> RecordReader::take()
{
    const auto start = m_expected > 0 ? m_buffer.begin() + 4 : m_buffer.begin();
    std::vector<std::uint8_t> value(start, m_buffer.end());
    m_buffer.clear();
    m_expected = 0;
    return value;
}
} // namespace dockpipe::desktop
