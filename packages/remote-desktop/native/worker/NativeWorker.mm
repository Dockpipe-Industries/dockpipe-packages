#include "WorkerProtocol.h"
#include "WorkerArguments.h"
#include "ChildProcess.h"
#include "MediaWriter.h"
#include "NativeClipboard.h"
#include "NativeAssets.h"
#include "WorkerCleanup.h"
#include "WorkerRunLoop.h"
#include "WorkerRuntime.h"
#include "WorkerReadiness.h"
#include "WorkerChannel.h"
#include "WorkerReports.h"
#include "WorkerApplication.h"
#include "PowerPolicy.h"
#include "PowerPolicyFile.h"
#include "../NativePowerAssertion.h"
#include "../NativeVirtualDisplayOwner.h"

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <spawn.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <atomic>
#include <fstream>
#include <map>
#include <iomanip>
#include <sstream>
#include <thread>

extern char **environ;

namespace dockpipe::desktop {
namespace {
#ifndef DOCKPIPE_NATIVE_WORKER_MODULE
std::atomic_bool *standaloneStop = nullptr;
void onSignal(int)
{
    if (standaloneStop)
        *standaloneStop = true;
}
#endif

class SessionNotifications final {
public:
    explicit SessionNotifications(const std::shared_ptr<std::atomic_bool> &stop)
    {
        const auto ownedStop = stop;
        m_center = NSWorkspace.sharedWorkspace.notificationCenter;
        m_session = [m_center addObserverForName:NSWorkspaceSessionDidResignActiveNotification
            object:nil queue:nil usingBlock:^(NSNotification *) { *ownedStop = true; }];
        m_sleep = [m_center addObserverForName:NSWorkspaceScreensDidSleepNotification
            object:nil queue:nil usingBlock:^(NSNotification *) { *ownedStop = true; }];
    }

    ~SessionNotifications()
    {
        [m_center removeObserver:m_session];
        [m_center removeObserver:m_sleep];
    }

private:
    NSNotificationCenter *__strong m_center;
    id __strong m_session;
    id __strong m_sleep;
};

Json readPrivate(const std::string &path, std::size_t maximum)
{
    struct stat metadata{};
    require(lstat(path.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode) && metadata.st_uid == getuid()
        && (metadata.st_mode & 0777) == 0600 && metadata.st_size > 0 && std::uint64_t(metadata.st_size) <= maximum, "Invalid private provider configuration.");
    const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(descriptor >= 0, "Cannot open private provider configuration.");
    struct stat opened{};
    if (fstat(descriptor, &opened) != 0 || opened.st_dev != metadata.st_dev || opened.st_ino != metadata.st_ino) {
        close(descriptor);
        throw std::runtime_error("Private provider configuration changed.");
    }
    std::string content(static_cast<std::size_t>(metadata.st_size), '\0');
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto count = read(descriptor, content.data() + offset, content.size() - offset);
        if (count <= 0) {
            close(descriptor);
            throw std::runtime_error("Cannot read private provider configuration.");
        }
        offset += static_cast<std::size_t>(count);
    }
    close(descriptor);
    return Json::parse(content);
}
void writePrivate(const std::string &path, const Json &value)
{
    const auto content = value.dump();
    require(content.size() <= 16384, "Private channel configuration exceeds its contract.");
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(descriptor >= 0, "Cannot create private channel configuration.");
    const bool complete = writeBytes(descriptor, reinterpret_cast<const std::uint8_t *>(content.data()), content.size(), Clock::now() + std::chrono::seconds(1));
    close(descriptor);
    require(complete, "Cannot write private channel configuration.");
}
std::string executablePath()
{
    char path[4096];
    std::uint32_t length = sizeof(path);
    require(_NSGetExecutablePath(path, &length) == 0, "Native executable path exceeds contract.");
    char resolved[4096];
    require(realpath(path, resolved) != nullptr, "Cannot resolve native executable.");
    return resolved;
}
std::string imageHash(const std::string &path)
{
    require(!path.empty() && path.front() == '/', "An absolute trusted executable is required.");
    const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(descriptor >= 0, "Cannot open trusted executable.");
    struct stat info{};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_uid != getuid() && info.st_uid != 0)
        || (info.st_mode & 0022) != 0 || !(info.st_mode & 0111) || info.st_size <= 0 || info.st_size > 128 * 1024 * 1024) {
        close(descriptor);
        throw std::runtime_error("Trusted executable has unsafe ownership, mode or size.");
    }
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    std::uint8_t buffer[65536];
    std::uint64_t total = 0;
    for (;;) {
        const auto count = read(descriptor, buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0 || total + static_cast<std::uint64_t>(count) > 128 * 1024 * 1024) {
            close(descriptor);
            throw std::runtime_error("Cannot hash bounded executable.");
        }
        total += static_cast<std::uint64_t>(count);
        CC_SHA256_Update(&hash, buffer, static_cast<CC_LONG>(count));
    }
    close(descriptor);
    std::uint8_t digestBytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digestBytes, &hash);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digestBytes)
        output << std::setw(2) << unsigned(byte);
    return output.str();
}
std::chrono::system_clock::time_point parseDate(const std::string &value)
{
    @autoreleasepool {
        NSString *text = [NSString stringWithUTF8String:value.c_str()];
        auto *formatter = [[NSISO8601DateFormatter alloc] init];
        formatter.formatOptions = NSISO8601DateFormatWithInternetDateTime | NSISO8601DateFormatWithFractionalSeconds;
        NSDate *date = [formatter dateFromString:text];
        if (!date) {
            formatter.formatOptions = NSISO8601DateFormatWithInternetDateTime;
            date = [formatter dateFromString:text];
        }
        require(date != nil, "Invalid original desktop deadline.");
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(static_cast<std::int64_t>(date.timeIntervalSince1970 * 1000)));
    }
}
std::string formatDate(Clock::time_point expiry)
{
    @autoreleasepool {
        const auto remaining = expiry - Clock::now();
        const auto wall = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(remaining);
        const auto seconds = std::chrono::duration<double>(wall.time_since_epoch()).count();
        auto *formatter = [[NSISO8601DateFormatter alloc] init];
        formatter.formatOptions = NSISO8601DateFormatWithInternetDateTime | NSISO8601DateFormatWithFractionalSeconds;
        return [[formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:seconds]] UTF8String];
    }
}
void sendLine(ChildProcess &child, const Json &value)
{
    const auto line = value.dump() + "\n";
    require(line.size() <= WorkerReports::MaximumBytes && writeBytes(child.input(), reinterpret_cast<const std::uint8_t *>(line.data()), line.size(), Clock::now() + std::chrono::seconds(1)), "Native control bridge write failed.");
}
void sendReport(ChildProcess &child, const WorkerProtocol &protocol, WorkerReports &reports, WorkerRuntime &runtime)
{
    runtime.diagnostics->stage(WorkerStage::Report);
    reports.publish(protocol.report(), [&](std::string_view line) {
        return writeBytes(child.input(), reinterpret_cast<const std::uint8_t *>(line.data()),
            line.size(), Clock::now() + std::chrono::seconds(1));
    });
}

Json waitLine(ChildProcess &child, Clock::time_point deadline, WorkerRuntime &runtime)
{
    RecordReader reader;
    while (!runtime.cancelled() && Clock::now() < deadline) {
        const auto outcome = reader.poll(child.output(), 1 << 20, true);
        if (outcome == RecordReader::Outcome::Complete)
            return Json::parse(reader.take());
        require(outcome == RecordReader::Outcome::Pending && !child.exited(), "Native control bridge closed.");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Native control bridge readiness expired.");
}
struct BackendSignals {
    std::atomic_bool started{false};
    std::atomic_bool startSucceeded{false};
    std::atomic_bool stopped{false};
    std::atomic_bool unverifiedStopObserved{false};
    std::atomic_bool inputFailed{false};
};
struct SessionResources {
    std::unique_ptr<ChildProcess> channel;
    std::shared_ptr<NativeDesktopBackend> backend;
    std::shared_ptr<MediaWriter> writer;
    std::unique_ptr<NativeAssets> assets;
    std::unique_ptr<ClipboardAdapter> clipboardAdapter;
    std::unique_ptr<WorkerClipboard> clipboard;
    std::shared_ptr<BackendSignals> signals = std::make_shared<BackendSignals>();
    std::string directory;
    NativePowerAssertion powerAssertion;
    ~SessionResources()
    {
        if (clipboard)
            clipboard->stop();
        if (writer)
            writer->stop();
        if (channel)
            channel->stop();
        // Paths below are fixed package-owned temporary files; no recursive
        // deletion or caller/wire-selected child paths.
        if (!directory.empty()) {
            for (const auto &name : {"channel.json", "identity/key.pem", "identity/cert.pem", "identity/consumed", "identity/relay-request.json"})
                unlink((directory + "/" + name).c_str());
            rmdir((directory + "/identity").c_str());
            rmdir(directory.c_str());
        }
    }
};

bool cleanup(SessionResources &resources, WorkerRuntime &runtime)
{
    runtime.diagnostics->stage(WorkerStage::WriterStop);
    const bool assetsStopped = !resources.assets || resources.assets->stop();
    if (resources.clipboard)
        resources.clipboard->stop();
    if (resources.writer)
        resources.writer->stop();
    runtime.diagnostics->writerJoined();
    runtime.diagnostics->stage(WorkerStage::NativeStop);
    const auto deadline = Clock::now() + std::chrono::milliseconds(5500);
    const auto completion = std::make_shared<StopCompletion>(deadline);
    if (resources.backend) {
        const auto signals = resources.signals;
        const auto diagnostics = runtime.diagnostics;
        resources.backend->stop([completion, signals, diagnostics](Result result) {
            diagnostics->nativeResult(result);
            // A watchdog's earlier unknown result is not upgraded by a later
            // cached completion, even if the OS eventually finishes stopping.
            completion->complete(result.cleanupVerified && !signals->unverifiedStopObserved.load());
        });
    } else {
        completion->complete(true);
    }
    // The relay's existing bounded TERM/KILL/reap runs while the main thread
    // services native capture-stop callbacks, rather than consuming another3s
    // after the native5.5s wait and exceeding the core's10s cleanup drain.
    bool nativeVerified = false;
    try {
        nativeVerified = finishCleanupConcurrently([&] {
            return awaitStopCompletion(*completion, deadline, [] { return Clock::now(); }, [&](Clock::time_point until) {
                runtime.cancelled();
                pumpMainRunLoop(until);
            });
        }, [&] {
            const bool verified = !resources.channel || resources.channel->stop();
            runtime.diagnostics->childStopped(verified);
            return verified;
        });
    } catch (...) {
        resources.powerAssertion.release();
        throw;
    }
    // Keep the Mac awake through held-input/capture teardown. Release is still
    // mandatory on an unverified native result, before any local/Core success.
    const auto powerReleased = resources.powerAssertion.release();
    return assetsStopped && nativeVerified && powerReleased.success && powerReleased.cleanupVerified;
}

bool stopControlBridge(ChildProcess &bridge, WorkerRuntime &runtime)
{
    runtime.diagnostics->stage(WorkerStage::BridgeStop);
    const bool verified = bridge.stop();
    runtime.diagnostics->bridgeStopped(verified);
    return verified;
}
} // namespace

std::string diagnosticMessageHash(const char *message)
{
    const auto length = message ? strnlen(message, 4096) : 0;
    std::uint8_t bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(message, static_cast<CC_LONG>(length), bytes);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes)
        output << std::setw(2) << unsigned(byte);
    return output.str();
}

int runPermissionMode(PermissionMode mode)
{
    require(mode != PermissionMode::None, "An explicit local permission operation is required.");
    if (mode == PermissionMode::Screen || mode == PermissionMode::Input) {
        // This is the same signed executable and bundle identity as the worker.
        // A local product setup action selects this mode; normal worker startup,
        // discovery and remote directives never call permission-request APIs.
        auto *application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        [application activateIgnoringOtherApps:YES];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        NativeDesktopBackend::requestPermission(mode == PermissionMode::Screen ? NativePermission::ScreenCapture : NativePermission::InputControl);
    }
    const auto status = NativeDesktopBackend::preflight();
    const Json result{{"screen_capture", status.screenCapture}, {"accessibility", status.accessibility}, {"event_posting", status.eventPosting}};
    const auto encoded = result.dump() + "\n";
    require(fwrite(encoded.data(), 1, encoded.size(), stdout) == encoded.size() && fflush(stdout) == 0, "Cannot report native permission status.");
    return 0;
}

namespace {
bool desktopServiceRegistered(const std::string &label)
{
    const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    require(null >= 0, "Cannot prepare desktop service observation.");
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(null);
        throw std::runtime_error("Cannot prepare desktop service observation.");
    }
    bool prepared = true;
    for (int descriptor = 0; descriptor <= 2; ++descriptor)
        prepared = prepared && posix_spawn_file_actions_adddup2(&actions, null, descriptor) == 0;
    const std::string target = "gui/" + std::to_string(getuid()) + "/" + label;
    char *arguments[] = {const_cast<char *>("/bin/launchctl"), const_cast<char *>("print"),
        const_cast<char *>(target.c_str()), nullptr};
    pid_t child = -1;
    const int status = prepared ? posix_spawn(&child, arguments[0], &actions, nullptr, arguments, environ) : EINVAL;
    posix_spawn_file_actions_destroy(&actions);
    close(null);
    require(status == 0, "Cannot inspect desktop service registration.");
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    while (Clock::now() < deadline) {
        int exitStatus = 0;
        const auto waited = waitpid(child, &exitStatus, WNOHANG);
        if (waited == child)
            return WIFEXITED(exitStatus) && WEXITSTATUS(exitStatus) == 0;
        if (waited < 0 && errno != EINTR)
            throw std::runtime_error("Cannot reap desktop service observation.");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    kill(child, SIGKILL); // Only this owned, bounded metadata subprocess.
    int exitStatus = 0;
    while (waitpid(child, &exitStatus, 0) < 0 && errno == EINTR) {}
    throw std::runtime_error("Desktop service observation exceeded its deadline.");
}

struct PowerAgentResources {
    NativePowerAssertion assertion;
    int lockDescriptor = -1;
    ~PowerAgentResources()
    {
        if (lockDescriptor >= 0)
            close(lockDescriptor);
    }
};
}

int runPowerAgent(const std::string &cli, const std::string &state, const std::string &helper,
    const std::string &manifestPath, const std::string &providerImage, const Json &desktopProvider,
    const std::string &desktopServiceLabel, WorkerRuntime &runtime)
{
    prepareBackgroundWorkerApplication();
    // No ScreenCaptureKit, input, permission, session-lock or unlock operations.
    // This companion has its own module pin and an independent exact worker pin.
    const auto manifest = readPrivate(manifestPath, 4096);
    require(manifest.is_object() && manifest.size() == 3
        && manifest.value("schema", "") == "dockpipe.desktop.native.provider/v1"
        && manifest.at("provider").at("id") == "dockpipe.desktop.native"
        && manifest.at("provider").at("version") == "0.1.0"
        && manifest.at("provider").at("id") == desktopProvider.at("id")
        && manifest.at("provider").at("version") == desktopProvider.at("version")
        && digest(manifest.at("provider").at("artifact")) && digest(manifest.at("channel_helper_sha256")),
        "Invalid independent desktop power manifest.");
    require(imageHash(providerImage) == manifest["provider"]["artifact"].get<std::string>()
        && imageHash(helper) == manifest["channel_helper_sha256"].get<std::string>(),
        "Desktop power module differs from its frozen manifest.");
    imageHash(cli);
    const PowerPolicyDirectory directory(state);
    const auto resources = std::make_shared<PowerAgentResources>();
    resources->lockDescriptor = directory.lock();
    require(flock(resources->lockDescriptor, LOCK_EX | LOCK_NB) == 0,
        "Another desktop power companion owns this state.");
    runtime.retainedResources = resources;
    runtime.cleanupVerified = false;
    try {
        auto nextServiceCheck = Clock::time_point{};
        bool serviceRegistered = false;
        while (!runtime.cancelled()) {
            const auto owner = readPrivate(state + "/desktop.json", 4096);
            require(owner.value("schema", "") == "dockpipe.remote.desktop/v1"
                && owner.at("provider") == desktopProvider && owner.at("view").is_boolean(),
                "Independent desktop power owner binding changed.");
            const auto policy = readPowerPolicy(state);
            const auto now = Clock::now();
            if (policy != PowerPolicy::Enabled || !owner.at("view").get<bool>()) {
                serviceRegistered = false;
                nextServiceCheck = Clock::time_point{};
            } else if (now >= nextServiceCheck) {
                serviceRegistered = desktopServiceRegistered(desktopServiceLabel);
                nextServiceCheck = Clock::now() + std::chrono::seconds(2);
            }
            const bool desired = enabledPowerAssertion(policy, owner.at("view").get<bool>(), serviceRegistered);
            const auto assertion = resources->assertion.setActive(desired, [&runtime] { return runtime.cancelled(); });
            require(assertion.success && assertion.active == desired, "Desktop power assertion update failed.");
            pumpMainRunLoop(Clock::now() + std::chrono::milliseconds(250));
        }
    } catch (...) {
        const auto released = resources->assertion.release();
        runtime.cleanupVerified = released.success && released.cleanupVerified;
        throw;
    }
    const auto released = resources->assertion.release();
    runtime.cleanupVerified = released.success && released.cleanupVerified;
    require(runtime.cleanupVerified, "Desktop power companion release remains unconfirmed.");
    return 0;
}

int runWorker(const std::string &cli, const std::string &state, const std::string &helper,
    const std::string &manifestPath, const std::string &providerImage, WorkerRuntime &runtime,
    std::optional<VirtualDisplayHostTemplateReference> virtualDisplayHostTemplate)
{
    prepareBackgroundWorkerApplication();
    runtime.diagnostics->stage(WorkerStage::Manifest);
    const auto manifest = readPrivate(manifestPath, 4096);
    require(manifest.at("schema") == "dockpipe.desktop.native.provider/v1" && manifest.size() == 3
        && manifest.at("provider").at("id") == "dockpipe.desktop.native" && manifest.at("provider").at("version") == "0.1.0"
        && digest(manifest.at("provider").at("artifact")) && digest(manifest.at("channel_helper_sha256")), "Invalid frozen native provider manifest.");
    runtime.diagnostics->stage(WorkerStage::ImageBinding);
    require(imageHash(providerImage) == manifest["provider"]["artifact"].get<std::string>() && imageHash(helper) == manifest["channel_helper_sha256"].get<std::string>(), "Native provider image differs from frozen manifest.");
    imageHash(cli); // Verify trusted install ownership/mode before execution.
    NativeDesktopBackend::VirtualDisplayFactory virtualDisplayFactory;
    if (virtualDisplayHostTemplate) {
        const auto context = loadVirtualDisplayOwnerContext(*virtualDisplayHostTemplate, cli, state, providerImage);
        virtualDisplayFactory = [context] {
            return std::make_shared<NativeVirtualDisplay>(makeVirtualDisplayOwnerPlatform(context));
        };
    }
    runtime.diagnostics->stage(WorkerStage::Owner);
    const auto owner = readPrivate(state + "/desktop.json", 4096);
    require(owner.at("schema") == "dockpipe.remote.desktop/v1" && owner.at("provider") == manifest["provider"], "Native provider differs from independent owner policy.");
    require(!runtime.cancelled(), "Native worker was cancelled before startup.");
    readPowerPolicy(state); // Invalid durable settings fail before bridge admission.
    publishConnectedPowerCapability(state, manifest["provider"]);
    runtime.diagnostics->stage(WorkerStage::Notifications);
    SessionNotifications notifications(runtime.stopRequested);
    runtime.diagnostics->stage(WorkerStage::Permissions);
    const auto permissions = NativeDesktopBackend::preflight();
    const bool ownerView = owner.at("view").get<bool>();
    const bool ownerInput = owner.at("input").get<bool>();
    const auto advertisedReadiness = workerReadiness(ownerView, ownerInput, permissions);
    const bool view = advertisedReadiness.view;
    const bool input = advertisedReadiness.input;
    runtime.cleanupVerified = false;
    runtime.diagnostics->stage(WorkerStage::BridgeStart);
    auto bridge = ChildProcess::start({cli, "remote", "desktop-worker", "--stdio", "--state", state, "--auth", "never"});
    sendLine(*bridge, Json{{"type", "ready"}, {"provider", manifest["provider"]}, {"view", view}, {"input", input}});
    runtime.diagnostics->stage(WorkerStage::BridgeReady);
    const auto event = waitLine(*bridge, Clock::now() + std::chrono::seconds(10), runtime);
    require(event.at("type") == "catalog" && event.at("catalog").at("view") == view && event.at("catalog").at("input") == input, "Native worker catalog changed readiness.");
    WorkerProtocol protocol(event.at("catalog"), manifest["provider"]);
    WorkerReports reports;
    auto resourceOwner = std::make_shared<SessionResources>();
    runtime.retainedResources = resourceOwner;
    auto &resources = *resourceOwner;
    RecordReader directives;
    RecordReader channelStatus;
    RecordReader channelInput;
    Clock::time_point originalDeadline{};
    Clock::time_point lastRenewal{};
    Clock::time_point lastPolicyCheck{};
    bool closedReported = false;
    bool cleanupVerified = false;
    bool forgetAcknowledged = false;
    Clock::time_point reportedAt{};
    auto stopSession = [&] {
        if (closedReported)
            return cleanupVerified;
        protocol.stop();
        const bool verified = cleanup(resources, runtime);
        if (!verified)
            runtime.diagnostics->failure();
        protocol.cleanup(verified, verified);
        cleanupVerified = verified;
        runtime.diagnostics->stage(WorkerStage::Report);
        sendReport(*bridge, protocol, reports, runtime);
        runtime.diagnostics->reportSent();
        closedReported = true;
        reportedAt = Clock::now();
        return verified;
    };
    // Keep authenticated control alive briefly for exact cleanup proof; this
    // applies equally to policy errors, user signals and normal closure.
    auto drainProof = [&] {
        runtime.diagnostics->stage(WorkerStage::Forget);
        if (forgetAcknowledged)
            return true;
        if (!closedReported || !cleanupVerified)
            return false;
        const auto drainDeadline = reportedAt + std::chrono::milliseconds(2500);
        while (Clock::now() < drainDeadline) {
            const auto outcome = directives.poll(bridge->output(), 1 << 20, true);
            if (outcome == RecordReader::Outcome::Complete) {
                try {
                    const auto message = Json::parse(directives.take());
                    if (exactCleanupForget(message, protocol.reference(), cleanupVerified)) {
                        forgetAcknowledged = true;
                        runtime.diagnostics->forgetVerified();
                        return true;
                    }
                    if (!queuedOriginalDirective(message, protocol.open()))
                        return false;
                } catch (...) {
                    return false;
                }
            } else if (outcome == RecordReader::Outcome::Closed || outcome == RecordReader::Outcome::Invalid) {
                return false;
            }
            pumpMainRunLoop(std::min(drainDeadline, Clock::now() + std::chrono::milliseconds(5)));
        }
        return false;
    };
    try {
        while (!runtime.cancelled()) {
            @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate date]];
            const auto now = Clock::now();
            if (closedReported) {
                require(drainProof(), "Exact desktop cleanup acknowledgment was not received.");
                require(stopControlBridge(*bridge, runtime), "Native control bridge cleanup is unknown.");
                runtime.cleanupVerified = true;
                return 0;
            }
            if (now - lastPolicyCheck >= std::chrono::milliseconds(500)) {
                runtime.diagnostics->stage(WorkerStage::Policy);
                require(readPrivate(state + "/desktop.json", 4096) == owner && readPrivate(manifestPath, 4096) == manifest, "Frozen native owner policy changed.");
                const auto current = NativeDesktopBackend::preflight();
                const auto action = readinessAction(advertisedReadiness,
                    workerReadiness(ownerView, ownerInput, current), protocol.phase() != WorkerProtocol::Phase::Idle);
                lastPolicyCheck = now;
                const auto savedPowerPolicy = readPowerPolicy(state);
                const bool desiredPower = connectedPowerAssertion(savedPowerPolicy,
                    protocol.phase() == WorkerProtocol::Phase::Running);
                const auto power = resources.powerAssertion.setActive(desiredPower, [&runtime] { return runtime.cancelled(); });
                require(power.success && power.active == desiredPower, "Connected desktop power assertion failed.");
                if (action == ReadinessAction::RefreshIdle) {
                    // No session was opened. Retire this frozen catalog only
                    // after its native CLI scope has been verified stopped.
                    require(stopControlBridge(*bridge, runtime), "Native control bridge cleanup is unknown.");
                    runtime.cleanupVerified = true;
                    return 0;
                }
                if (action == ReadinessAction::CloseOwnedSession) {
                    // Gains never widen this open's authority. Losses and gains
                    // both use native cleanup plus exact original-ref Forget.
                    require(stopSession(), "Native desktop cleanup is unknown.");
                    continue;
                }
            }
            if (protocol.phase() != WorkerProtocol::Phase::Idle && !closedReported) {
                if (protocol.expired(now) || now >= originalDeadline || resources.signals->inputFailed || (resources.writer && resources.writer->failed())
                    || (resources.signals->started && !resources.signals->startSucceeded) || resources.signals->stopped) {
                    const bool verified = stopSession();
                    require(verified, "Native desktop cleanup is unknown; replacement requires provider termination.");
                    continue;
                }
            }
            runtime.diagnostics->stage(WorkerStage::Directive);
            const auto line = directives.poll(bridge->output(), 1 << 20, true);
            require(line != RecordReader::Outcome::Closed && line != RecordReader::Outcome::Invalid, "Native worker bridge transport failed.");
            if (line == RecordReader::Outcome::Complete) {
                const auto message = Json::parse(directives.take());
                if (message.at("type") == "forget") {
                    require(closedReported && protocol.phase() == WorkerProtocol::Phase::Closed
                        && exactCleanupForget(message, protocol.reference(), cleanupVerified), "Forget cannot release unknown native ownership.");
                    forgetAcknowledged = true;
                    runtime.diagnostics->forgetVerified();
                    require(stopControlBridge(*bridge, runtime), "Native control bridge cleanup is unknown.");
                    runtime.cleanupVerified = true;
                    return 0; // A supervisor may start a fresh bridge; no automatic open.
                }
                require(message.at("type") == "directive", "Unsupported native worker control event.");
                const auto &directive = message.at("directive");
                const bool created = protocol.directive(directive, now);
                if (created) {
                    const auto deadlineWall = parseDate(protocol.open().at("deadline").get<std::string>());
                    const auto remaining = deadlineWall - std::chrono::system_clock::now();
                    require(remaining > std::chrono::seconds(0) && remaining <= std::chrono::hours(1), "Original desktop deadline expired.");
                    originalDeadline = now + std::chrono::duration_cast<Clock::duration>(remaining);
                    if (protocol.phase() != WorkerProtocol::Phase::Stopping) {
                        char temporary[] = "/tmp/dockpipe-native-desktop-session-XXXXXX";
                        require(mkdtemp(temporary) != nullptr, "Cannot create owned private session directory.");
                        resources.directory = temporary;
                        runtime.diagnostics->stage(WorkerStage::Identity);
                        auto identity = ChildProcess::start({helper, "identity", "--directory", resources.directory + "/identity"});
                        const auto generated = waitLine(*identity, now + std::chrono::seconds(3), runtime);
                        require(identity->stop() && generated.size() == 1 && digest(generated.at("fingerprint")), "Native TLS identity generation failed.");
                        protocol.localPin(generated["fingerprint"].get<std::string>());
                    }
                }
                if (protocol.phase() == WorkerProtocol::Phase::Stopping && !closedReported) {
                    require(stopSession(), "Native desktop cleanup is unknown.");
                } else if (!closedReported) {
                    if (protocol.canConnect()) {
                        const auto lease = std::min(protocol.lease(), originalDeadline);
                        Json config{{"schema", "dockpipe.desktop.channel/v2"}, {"reference", protocol.reference()}, {"role", "worker"}, {"mode", protocol.open()["mode"]},
                            {"local_fingerprint", protocol.localPin()}, {"peer_fingerprint", protocol.peerPin()}, {"identity_directory", resources.directory + "/identity"},
                            {"cli", cli}, {"state_directory", state}, {"deadline", protocol.open()["deadline"]}, {"lease_expires_at", formatDate(lease)}};
                        writePrivate(resources.directory + "/channel.json", config);
                        runtime.diagnostics->stage(WorkerStage::ChannelStart);
                        resources.channel = ChildProcess::start({helper, "connect", "--config", resources.directory + "/channel.json", "--control-fd", "3", "--status-fd", "4"}, true);
                        protocol.connecting();
                        lastRenewal = lease;
                    }
                    const auto lease = std::min(protocol.lease(), originalDeadline);
                    if (resources.channel && lease > lastRenewal + std::chrono::milliseconds(50)) {
                        runtime.diagnostics->stage(WorkerStage::Renewal);
                        const auto renewal = Json{{"type", "lease"}, {"expires_at", formatDate(lease)}}.dump();
                        require(writeRecord(resources.channel->control(), std::vector<std::uint8_t>(renewal.begin(), renewal.end()), 4096, now + std::chrono::milliseconds(250)), "Native authority renewal failed.");
                        lastRenewal = lease;
                    }
                    if (resources.backend)
                        resources.backend->renewLease(lease);
                    sendReport(*bridge, protocol, reports, runtime);
                }
            }
            if (resources.channel && !closedReported) {
                const bool childExited = resources.channel->exited();
                if (!resources.backend) {
                    runtime.diagnostics->stage(WorkerStage::ChannelReady);
                    require(!childExited, "Pinned native channel exited before readiness.");
                    const auto ready = channelStatus.poll(resources.channel->status(), 4096);
                    require(ready != RecordReader::Outcome::Closed && ready != RecordReader::Outcome::Invalid, "Encrypted channel never became ready.");
                    if (ready == RecordReader::Outcome::Complete) {
                        protocol.ready(Json::parse(channelStatus.take()));
                        runtime.diagnostics->stage(WorkerStage::NativeStart);
                        const auto capture = protocol.capture();
                        const auto bounds = captureCodecBounds(capture);
                        require(bounds.has_value(), "The negotiated codec bounds are unavailable.");
                        require(capture.displaySource != DisplaySource::Virtual || bool(virtualDisplayFactory),
                            "Virtual display capture requires a pinned dedicated-owner template.");
                        resources.backend = std::make_shared<NativeDesktopBackend>(virtualDisplayFactory);
                        const std::weak_ptr<NativeDesktopBackend> weakBackend = resources.backend;
                        resources.writer = std::make_shared<MediaWriter>(resources.channel->input(), [weakBackend] {
                            if (const auto backend = weakBackend.lock())
                                backend->requestKeyframe();
                        }, *bounds);
                        const auto signals = resources.signals;
                        const auto writer = resources.writer;
                        auto admitted = [&] {
                            if (runtime.cancelled() || protocol.phase() != WorkerProtocol::Phase::Running
                                || protocol.open()["mode"] != "control"
                                || !resources.signals->startSucceeded || resources.signals->stopped
                                || resources.signals->inputFailed || resources.writer->failed()
                                || protocol.expired(Clock::now()) || Clock::now() >= originalDeadline)
                                return false;
                            require(readPrivate(state + "/desktop.json", 4096) == owner
                                && readPrivate(manifestPath, 4096) == manifest, "Frozen clipboard policy changed.");
                            const auto status = NativeDesktopBackend::preflight();
                            return ownerInput && status.screenCapture && status.accessibility && status.eventPosting
                                && !runtime.cancelled() && !resources.signals->stopped
                                && !resources.signals->inputFailed && !resources.writer->failed()
                                && !protocol.expired(Clock::now()) && Clock::now() < originalDeadline;
                        };
                        if (capture.clipboardText) {
                            resources.clipboardAdapter = makeNativeClipboard();
                            resources.clipboard = std::make_unique<WorkerClipboard>(*resources.clipboardAdapter,
                                admitted, [writer](ClipboardRecord record) {
                                    require(writer->submitClipboard(std::move(record)), "Native clipboard output is saturated.");
                                }, [writer](std::uint64_t epoch) { writer->clipboardEpoch(epoch); },
                                [writer](std::uint64_t epoch) { writer->discardClipboardObservations(epoch); },
                                [&resources] {
                                    if (resources.assets)
                                        resources.assets->externalClipboardWrite();
                                });
                        }
                        if (capture.clipboardImages || capture.clipboardFiles) {
                            resources.assets = std::make_unique<NativeAssets>(state + "/clipboard-assets",
                                capture.clipboardImages, capture.clipboardFiles,
                                [writer](AssetClipboardRecord record) {
                                    require(writer->submitAsset(std::move(record)), "Native clipboard asset output is saturated.");
                                }, admitted, [&resources] {
                                    if (resources.clipboard)
                                        resources.clipboard->retireObservation();
                                });
                        }
                        CaptureCallbacks callbacks;
                        callbacks.frame = [writer](EncodedFrame frame) { writer->submit(std::move(frame)); };
                        const auto diagnostics = runtime.diagnostics;
                        callbacks.stopped = [signals, diagnostics](Result result) {
                            diagnostics->nativeResult(result);
                            if (!result.cleanupVerified)
                                signals->unverifiedStopObserved = true;
                            signals->stopped = true;
                        };
                        SessionGrant grant{true, protocol.open()["mode"] == "control", std::min(protocol.lease(), originalDeadline), capture};
                        resources.backend->start(grant, callbacks, [signals](Result result) { signals->startSucceeded = bool(result); signals->started = true; });
                    }
                } else {
                    if (resources.signals->started && resources.signals->startSucceeded && protocol.phase() == WorkerProtocol::Phase::Connecting) {
                        protocol.running();
                        sendReport(*bridge, protocol, reports, runtime);
                    }
                    runtime.diagnostics->stage(WorkerStage::ChannelRead);
                    const auto message = pollWorkerChannel(channelInput, resources.channel->output(),
                        protocol.phase(), childExited, protocol.open()["mode"] == "control", protocol.capture().clipboardText,
                        protocol.capture().clipboardImages, protocol.capture().clipboardFiles);
                    if (message.kind == WorkerChannelMessage::Kind::Stop) {
                        // Established authenticated EOF is a close event, not
                        // an exception. All existing cleanup/Forget proofs
                        // remain mandatory before the clean return below.
                        require(stopSession(), "Native desktop cleanup is unknown.");
                        continue;
                    }
                    if (message.kind == WorkerChannelMessage::Kind::Keyframe) {
                        resources.backend->requestKeyframe();
                    } else if (message.kind == WorkerChannelMessage::Kind::Clipboard) {
                        require(bool(resources.clipboard), "Native clipboard was not negotiated.");
                        resources.clipboard->receive(message.clipboard, Clock::now());
                    } else if (message.kind == WorkerChannelMessage::Kind::Asset) {
                        require(bool(resources.assets), "Native clipboard assets were not negotiated.");
                        resources.assets->receive(message.asset);
                    } else if (message.kind == WorkerChannelMessage::Kind::Input) {
                        const auto signals = resources.signals;
                        resources.backend->input(message.input, [signals](Result result) {
                            if (!result)
                                signals->inputFailed = true;
                        });
                    }
                    if (resources.assets)
                        resources.assets->poll();
                    if (resources.clipboard)
                        resources.clipboard->poll(Clock::now());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    } catch (...) {
        runtime.diagnostics->failure();
        if (protocol.phase() != WorkerProtocol::Phase::Idle && !closedReported) {
            try { stopSession(); } catch (...) { cleanup(resources, runtime); }
        }
        const bool proofVerified = protocol.phase() == WorkerProtocol::Phase::Idle || drainProof();
        const bool bridgeVerified = stopControlBridge(*bridge, runtime);
        runtime.cleanupVerified = proofVerified && bridgeVerified
            && (protocol.phase() == WorkerProtocol::Phase::Idle || cleanupVerified);
        throw;
    }
    if (protocol.phase() != WorkerProtocol::Phase::Idle) {
        require(stopSession(), "Native desktop cleanup remains unknown.");
        require(drainProof(), "Exact desktop cleanup acknowledgment was not received.");
    }
    require(stopControlBridge(*bridge, runtime), "Native control bridge cleanup is unknown.");
    runtime.cleanupVerified = true;
    return 0;
}
} // namespace dockpipe::desktop

#ifndef DOCKPIPE_NATIVE_WORKER_MODULE
int main(int argc, char **argv)
{
    dockpipe::desktop::WorkerRuntime runtime;
    dockpipe::desktop::standaloneStop = runtime.stopRequested.get();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, dockpipe::desktop::onSignal);
    signal(SIGTERM, dockpipe::desktop::onSignal);
    @autoreleasepool {
        try {
            std::vector<std::string> values;
            for (int index = 1; index < argc; ++index)
                values.emplace_back(argv[index]);
            const auto arguments = dockpipe::desktop::parseWorkerArguments(values);
            if (arguments.permission != dockpipe::desktop::PermissionMode::None)
                return dockpipe::desktop::runPermissionMode(arguments.permission);
            const auto &paths = arguments.paths;
            return dockpipe::desktop::runWorker(paths.at("--cli"), paths.at("--state"), paths.at("--channel-helper"), paths.at("--manifest"), dockpipe::desktop::executablePath(), runtime);
        } catch (...) {
            // Fixed diagnostics only: no input, media, keys, selected private
            // paths or operator metadata are printed.
            fputs("Native desktop worker stopped or failed.\n", stderr);
            return 1;
        }
    }
}

#endif
