#include "NativeVirtualDisplayOwnerClient.h"
#include "NativeVirtualDisplayOwnerProtocol.h"
#include "NativeVirtualDisplaySnapshot.h"
#include "NativeVirtualDisplayObserver.h"
#include "worker/ChildProcess.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

namespace dockpipe::desktop {
namespace {
using Clock = NativeVirtualDisplay::Clock;
using Json = nlohmann::json;
using namespace std::chrono_literals;

bool dockpipeDisplay(const VirtualDisplayOwnerDisplay &display)
{
    return display.vendor == 0x4450 && display.product == 1;
}
bool fallbackDisplay(const VirtualDisplayOwnerDisplay &display)
{
    return display.vendor == 0x756e6b6e && display.product == 0x76697274;
}
bool complete(const VirtualDisplayOwnerInventory &inventory)
{
    if (!inventory.complete || inventory.displays.size() > 64)
        return false;
    std::set<std::uint32_t> ids;
    for (const auto &display : inventory.displays) {
        if (!display.id || !ids.insert(display.id).second)
            return false;
    }
    return true;
}
bool preserved(const VirtualDisplayOwnerInventory &before, const VirtualDisplayOwnerInventory &after)
{
    for (const auto &original : before.displays) {
        if (fallbackDisplay(original))
            continue;
        const auto current = std::find_if(after.displays.begin(), after.displays.end(), [&](const auto &display) {
            return display.id == original.id;
        });
        if (current == after.displays.end() || current->vendor != original.vendor || current->product != original.product
            || current->serial != original.serial || current->width != original.width || current->height != original.height
            || current->mirror != original.mirror || current->mirrored != original.mirrored || current->main != original.main)
            return false;
    }
    return true;
}
void requireOwner(bool value)
{
    if (!value)
        throw std::runtime_error("Private virtual display owner admission failed.");
}
std::array<unsigned char, 32> randomBytes()
{
    std::array<unsigned char, 32> bytes{};
    const int input = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    requireOwner(input >= 0);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = read(input, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            close(input);
            requireOwner(false);
        }
        offset += static_cast<std::size_t>(count);
    }
    close(input);
    return bytes;
}

class OwnerPlatform final : public NativeVirtualDisplay::Platform {
public:
    OwnerPlatform(VirtualDisplayOwnerContext context, VirtualDisplayOwnerInventoryProvider inventory,
        VirtualDisplayOwnerIdentityProbe identity)
        : m_context(std::move(context)), m_inventory(std::move(inventory)), m_identity(std::move(identity))
    {
    }
    bool available() override
    {
        return !m_context.hostExecutable.empty() && m_context.hostExecutable.front() == '/'
            && m_context.hostExecutable.size() <= 4096 && m_context.hostExecutable.find('\0') == std::string::npos
            && m_context.materializeDescriptor && m_context.verifySnapshot && m_inventory && m_identity;
    }
    bool ownerAlive() override
    {
        std::unique_lock lock(m_mutex, std::try_to_lock);
        return lock.owns_lock() && m_ready && !m_released && m_child && !m_child->exited()
            && m_identity(m_display, m_request.spec.serial);
    }
    NativeVirtualDisplay::Creation create(NativeVirtualDisplaySpec) override
    {
        return {};
    }
    NativeVirtualDisplay::Creation createWithAdmission(NativeVirtualDisplaySpec spec, Clock::time_point deadline,
        const NativeVirtualDisplay::SelectionAdmission &admission) override;
    NativeVirtualDisplay::Observation observe(std::uint32_t display, NativeVirtualDisplaySpec spec) override;
    void release() override
    {
        releaseBefore(Clock::now());
    }
    void releaseBefore(Clock::time_point deadline) override;
private:
    Json waitMessage(Clock::time_point deadline, const NativeVirtualDisplay::SelectionAdmission &admission);
    bool publicMatches(const VirtualDisplayOwnerInventory &snapshot, std::uint32_t display, NativeVirtualDisplaySpec spec) const;
    bool matches(const VirtualDisplayOwnerInventory &snapshot, std::uint32_t display, NativeVirtualDisplaySpec spec) const;
    void removeFiles();
    VirtualDisplayOwnerContext m_context;
    VirtualDisplayOwnerInventoryProvider m_inventory;
    VirtualDisplayOwnerIdentityProbe m_identity;
    std::mutex m_mutex;
    VirtualDisplayOwnerInventory m_before;
    VirtualDisplayOwnerRequest m_request;
    std::unique_ptr<ChildProcess> m_child;
    std::unique_ptr<VirtualDisplayModeObserver> m_observer;
    bool m_observerReaped = true;
    RecordReader m_reader;
    std::string m_directory;
    std::string m_hostTemporary;
    NativeVirtualDisplaySnapshot m_snapshot;
    bool m_snapshotAdmitted = false;
    bool m_fileCleanupReported = false;
    std::uint32_t m_display = 0;
    bool m_used = false;
    bool m_createSent = false;
    bool m_spawnFailed = false;
    bool m_ready = false;
    bool m_released = false;
    bool m_reaped = false;
};

Json OwnerPlatform::waitMessage(Clock::time_point deadline, const NativeVirtualDisplay::SelectionAdmission &admission)
{
    while (Clock::now() < deadline && admission && admission()) {
        requireOwner(m_child && !m_child->exited());
        const auto outcome = m_reader.poll(m_child->output(), VirtualDisplayOwnerRecordLimit, true);
        if (outcome == RecordReader::Outcome::Complete) {
            const auto record = m_reader.take();
            return parseVirtualDisplayOwnerJson(std::string(record.begin(), record.end()));
        }
        requireOwner(outcome == RecordReader::Outcome::Pending);
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Private virtual display owner admission expired.");
}

bool OwnerPlatform::publicMatches(const VirtualDisplayOwnerInventory &snapshot, std::uint32_t display,
    NativeVirtualDisplaySpec spec) const
{
    if (!complete(snapshot) || !preserved(m_before, snapshot) || !m_child || m_child->exited()
        || std::any_of(m_before.displays.begin(), m_before.displays.end(), [&](const auto &before) { return before.id == display; }))
        return false;
    const auto found = std::find_if(snapshot.displays.begin(), snapshot.displays.end(), [&](const auto &value) {
        return value.id == display;
    });
    return found != snapshot.displays.end() && dockpipeDisplay(*found) && found->serial == m_request.spec.serial
        && found->active && !found->mirrored && !found->mirror && found->width == spec.width && found->height == spec.height
        && found->boundsWidth == spec.width && found->boundsHeight == spec.height;
}


bool OwnerPlatform::matches(const VirtualDisplayOwnerInventory &snapshot, std::uint32_t display,
    NativeVirtualDisplaySpec spec) const
{
    return publicMatches(snapshot, display, spec) && m_observer && m_observer->mode()
        && exactVirtualMode(*m_observer->mode(), spec);
}

NativeVirtualDisplay::Creation OwnerPlatform::createWithAdmission(NativeVirtualDisplaySpec spec,
    Clock::time_point deadline, const NativeVirtualDisplay::SelectionAdmission &admission)
{
    std::lock_guard lock(m_mutex);
    if (m_used)
        return {bool(m_child), m_display};
    m_used = true;
    try {
        requireOwner(available() && admission && admission());
        m_before = m_inventory(false);
        requireOwner(complete(m_before));
        const auto random = randomBytes();
        static const char digits[] = "0123456789abcdef";
        for (unsigned char byte : random) {
            m_request.nonce.push_back(digits[byte >> 4]);
            m_request.nonce.push_back(digits[byte & 15]);
        }
        m_request.spec = spec;
        m_request.spec.serial = (std::uint32_t(random[0]) << 24) | (std::uint32_t(random[1]) << 16)
            | (std::uint32_t(random[2]) << 8) | random[3];
        requireOwner(m_request.spec.serial != 0);
        for (const auto &display : m_before.displays)
            requireOwner(!dockpipeDisplay(display) || display.serial != m_request.spec.serial);
        m_request.parentProcess = getpid();
        const auto remaining = deadline - Clock::now();
        requireOwner(remaining > Clock::duration::zero());
        m_request.createDeadlineUnixMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::system_clock::now() + remaining).time_since_epoch()).count();
#ifdef __APPLE__
        char directory[] = "/private/tmp/dockpipe-virtual-owner-XXXXXX";
#else
        char directory[] = "/tmp/dockpipe-virtual-owner-XXXXXX";
#endif
        requireOwner(mkdtemp(directory) != nullptr);
        m_directory = directory;
        char canonical[PATH_MAX];
        requireOwner(realpath(directory, canonical) != nullptr);
        m_directory = canonical;
        const auto descriptor = m_context.materializeDescriptor(m_directory, virtualDisplayOwnerProfile(m_request));
        requireOwner(descriptor == m_directory + "/child-host.json");
        struct stat metadata{};
        requireOwner(lstat(descriptor.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode)
            && metadata.st_uid == getuid() && (metadata.st_mode & 0777) == 0600 && metadata.st_size <= 65536);
        m_hostTemporary = m_directory + "/host-tmp";
        requireOwner(mkdir(m_hostTemporary.c_str(), 0700) == 0);
        requireOwner(admission());
        try {
            m_child = ChildProcess::start({m_context.hostExecutable, "--native-worker-host", "--request", descriptor}, false, m_hostTemporary);
        } catch (...) {
            m_spawnFailed = true; // A post-spawn transport failure cannot invent a reap proof.
            throw;
        }
        auto admitted = waitMessage(deadline, admission);
        validateVirtualDisplayOwnerMessage(admitted, m_request, "admitted", 7);
        requireOwner(virtualDisplayOwnerNumber(admitted.at("pid"), INT_MAX) == static_cast<unsigned>(m_child->pid()));
        requireOwner(admitted.at("snapshot_path").is_string());
        m_snapshotAdmitted = m_snapshot.admit(m_hostTemporary, admitted.at("snapshot_path").get<std::string>(),
            m_context.verifySnapshot);
        requireOwner(m_snapshotAdmitted);
        requireOwner(admission()); // Last authority check before the sole create command.
        auto command = virtualDisplayOwnerMessage(m_request, "create").dump() + "\n";
        m_createSent = true; // A lost/partial write is never treated as no effects.
        requireOwner(writeBytes(m_child->input(), reinterpret_cast<const std::uint8_t *>(command.data()), command.size(), deadline));
        auto ready = waitMessage(deadline, admission);
        validateVirtualDisplayOwnerMessage(ready, m_request, "ready", 10);
        requireOwner(virtualDisplayOwnerNumber(ready.at("pid"), INT_MAX) == static_cast<unsigned>(m_child->pid()));
        requireOwner(virtualDisplayOwnerNumber(ready.at("width"), 3840) == spec.width
            && virtualDisplayOwnerNumber(ready.at("height"), 2160) == spec.height
            && virtualDisplayOwnerNumber(ready.at("refresh_rate"), 60) == 60);
        m_display = virtualDisplayOwnerNumber(ready.at("display"), std::numeric_limits<std::uint32_t>::max());
        requireOwner(m_display != 0);
        bool observed = false;
        while (Clock::now() < deadline && admission()) {
            if (publicMatches(m_inventory(false), m_display, spec)) {
                observed = true;
                break;
            }
            requireOwner(!m_child->exited());
            std::this_thread::sleep_for(5ms);
        }
        requireOwner(observed && Clock::now() < deadline && admission());
        VirtualDisplayObserverRequest observation{m_request.spec, m_display, m_request.nonce,
            m_request.parentProcess, m_request.createDeadlineUnixMilliseconds};
        m_observer = std::make_unique<VirtualDisplayModeObserver>(m_context);
        m_observerReaped = false;
        requireOwner(m_observer->observe(observation, deadline, admission));
        m_observerReaped = true; // Positive observation includes exit0, reap and snapshot cleanup.
        requireOwner(Clock::now() < deadline && admission() && matches(m_inventory(false), m_display, spec));
        m_ready = true;
        return {true, m_display};
    } catch (...) {
        // Keep every launched child and planned identity for close(), including
        // failed descriptor admission, partial writes and missing Ready.
        if (!m_child && !m_createSent)
            removeFiles();
        return {bool(m_child) || m_createSent || m_spawnFailed, 0};
    }
}

NativeVirtualDisplay::Observation OwnerPlatform::observe(std::uint32_t display, NativeVirtualDisplaySpec spec)
{
    std::lock_guard lock(m_mutex);
    if (!m_released) {
        const auto snapshot = m_inventory(false);
        return {complete(snapshot), true, m_ready && display == m_display && matches(snapshot, display, spec)};
    }
    if (!m_reaped || !m_observerReaped)
        return {};
    if (!m_createSent)
        return {true, false, false};
    const auto snapshot = m_inventory(false);
    if (!complete(snapshot))
        return {};
    for (const auto &value : snapshot.displays) {
        const bool known = std::any_of(m_before.displays.begin(), m_before.displays.end(), [&](const auto &before) {
            return before.id == value.id;
        });
        if (((display || m_display) && value.id == (display ? display : m_display)) || (dockpipeDisplay(value)
                && (value.serial == m_request.spec.serial || !known)))
            return {true, true, false};
    }
    return {true, false, false};
}

void OwnerPlatform::removeFiles()
{
    if (!m_directory.empty()) {
        rmdir((m_directory + "/host-tmp").c_str());
        unlink((m_directory + "/child-host.json").c_str());
        rmdir(m_directory.c_str());
    }
}

void OwnerPlatform::releaseBefore(Clock::time_point deadline)
{
    std::lock_guard lock(m_mutex);
    m_released = true;
    m_ready = false;
    if (m_observer && !m_observerReaped) {
        const auto now = Clock::now();
        const auto remaining = std::max(Clock::duration::zero(), deadline - now);
        const auto observerBudget = std::min(remaining / 2, std::chrono::duration_cast<Clock::duration>(500ms));
        m_observerReaped = m_observer->close(std::min(deadline, now + observerBudget));
    }
    if (m_child && !m_reaped) {
        auto stop = virtualDisplayOwnerMessage(m_request, "stop");
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        stop["remaining_ms"] = std::max<std::int64_t>(0, std::min<std::int64_t>(remaining, 5000));
        const auto line = stop.dump() + "\n";
        writeBytes(m_child->input(), reinterpret_cast<const std::uint8_t *>(line.data()), line.size(),
            std::min(deadline, Clock::now() + 20ms));
        // Leave time for independent public absence observations in the same
        // caller-owned deadline. stop() cannot renew this absolute budget.
        m_reaped = m_child->stop(deadline - std::min(100ms, std::chrono::duration_cast<std::chrono::milliseconds>(
            std::max(Clock::duration::zero(), deadline - Clock::now()))));
    } else if (!m_child) {
        m_reaped = !m_spawnFailed;
    }
    if (m_reaped) {
        const bool snapshotClean = m_snapshotAdmitted ? m_snapshot.cleanupAfterReap()
            : (rmdir(m_hostTemporary.c_str()) == 0 || errno == ENOENT);
        if (snapshotClean) {
            removeFiles();
        } else {
            // Display closure is independent of private snapshot-file cleanup.
            // Retain this namespace for exact operator reconciliation.
            if (!m_fileCleanupReported) {
                m_fileCleanupReported = true;
                const char diagnostic[] = "Native virtual display private snapshot cleanup remains unverified.\n";
                writeBytes(STDERR_FILENO, reinterpret_cast<const std::uint8_t *>(diagnostic),
                    sizeof(diagnostic) - 1, std::min(deadline, Clock::now() + 10ms));
            }
            const auto report = m_directory + "/snapshot-cleanup-unknown.json";
            const int output = open(report.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (output >= 0) {
                const char status[] = "{\"schema\":\"dockpipe.desktop.snapshot-cleanup/v1\",\"verified\":false}\n";
                const auto ignored = write(output, status, sizeof(status) - 1);
                (void)ignored;
                close(output);
            }
        }
    }
}
} // namespace

std::shared_ptr<NativeVirtualDisplay::Platform> makeVirtualDisplayOwnerPlatformForSystem(
    VirtualDisplayOwnerContext context, VirtualDisplayOwnerInventoryProvider inventory, VirtualDisplayOwnerIdentityProbe identity)
{
    return std::make_shared<OwnerPlatform>(std::move(context), std::move(inventory), std::move(identity));
}
} // namespace dockpipe::desktop
