#include "NativeVirtualDisplayOwnerClient.h"
#include "NativeVirtualDisplayOwnerProtocol.h"
#include "worker/ChildProcess.h"
#include "worker/WorkerApplication.h"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <fcntl.h>
#include <future>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

namespace dockpipe::desktop {
namespace {
using Clock = NativeVirtualDisplay::Clock;
using Json = nlohmann::json;
using namespace std::chrono_literals;

VirtualDisplayOwnerInventory inventory(bool includeModes)
{
    VirtualDisplayOwnerInventory result;
    std::uint32_t before = 0;
    std::uint32_t count = 0;
    std::uint32_t after = 0;
    CGDirectDisplayID ids[64];
    if (CGGetOnlineDisplayList(0, nullptr, &before) != kCGErrorSuccess || before > 64
        || CGGetOnlineDisplayList(64, ids, &count) != kCGErrorSuccess || count > 64
        || CGGetOnlineDisplayList(0, nullptr, &after) != kCGErrorSuccess || before != count || after != count)
        return result;
    const auto main = CGMainDisplayID();
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto id = ids[index];
        VirtualDisplayOwnerDisplay display;
        display.id = id;
        display.vendor = CGDisplayVendorNumber(id);
        display.product = CGDisplayModelNumber(id);
        display.serial = CGDisplaySerialNumber(id);
        display.width = CGDisplayPixelsWide(id);
        display.height = CGDisplayPixelsHigh(id);
        const auto bounds = CGDisplayBounds(id);
        display.boundsWidth = bounds.size.width;
        display.boundsHeight = bounds.size.height;
        display.mirror = CGDisplayMirrorsDisplay(id);
        display.mirrored = CGDisplayIsInMirrorSet(id);
        display.main = id == main;
        display.active = CGDisplayIsActive(id);
        // Baseline/absence inventory must not prime CoreGraphics mode caches
        // before the child has created and verified its exact mode.
        CGDisplayModeRef mode = includeModes ? CGDisplayCopyDisplayMode(id) : nullptr;
        if (mode) {
            display.modeKnown = true;
            display.mode = {CGDisplayModeGetWidth(mode), CGDisplayModeGetHeight(mode), CGDisplayModeGetPixelWidth(mode),
                CGDisplayModeGetPixelHeight(mode), CGDisplayModeGetRefreshRate(mode)};
            CFRelease(mode);
        }
        result.displays.push_back(display);
    }
    result.complete = true;
    return result;
}

bool identity(std::uint32_t id, std::uint32_t serial)
{
    return id && CGDisplayIsOnline(id) && CGDisplayIsActive(id) && CGDisplayVendorNumber(id) == 0x4450
        && CGDisplayModelNumber(id) == 1 && CGDisplaySerialNumber(id) == serial;
}

void pump()
{
    @autoreleasepool {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    }
    std::this_thread::sleep_for(1ms);
}

bool send(const Json &message)
{
    const auto line = message.dump() + "\n";
    return line.size() <= VirtualDisplayOwnerRecordLimit
        && writeBytes(STDOUT_FILENO, reinterpret_cast<const std::uint8_t *>(line.data()), line.size(), Clock::now() + 100ms);
}

void requireOwner(bool value)
{
    if (!value)
        throw std::runtime_error("Private virtual display owner child failed.");
}
}

std::shared_ptr<NativeVirtualDisplay::Platform> makeVirtualDisplayOwnerPlatform(VirtualDisplayOwnerContext context)
{
    // Only function objects are installed here. No public/private display call,
    // child process or descriptor materialization occurs before create().
    return makeVirtualDisplayOwnerPlatformForSystem(std::move(context), inventory, identity);
}

NativeVirtualDisplayResult runVirtualDisplayOwnerChild(const VirtualDisplayOwnerRequest &request,
    const std::function<bool()> &stopRequested, const std::string &loadedModulePath)
{
    std::shared_ptr<NativeVirtualDisplay> display;
    NativeVirtualDisplayResult started;
    bool createRequested = false;
    bool readySent = false;
    std::chrono::milliseconds stopBudget{100};
    try {
        requireOwner([NSThread isMainThread] && getppid() == request.parentProcess);
        // These are private pipes inherited by the exact parent-spawned child.
        // They must never block the main native run loop or alter global signals.
        for (int descriptor : {STDIN_FILENO, STDOUT_FILENO}) {
            struct stat metadata{};
            const int flags = fcntl(descriptor, F_GETFL);
            requireOwner(fstat(descriptor, &metadata) == 0 && S_ISFIFO(metadata.st_mode) && flags >= 0
                && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0);
        }
        auto admitted = virtualDisplayOwnerMessage(request, "admitted");
        admitted["pid"] = getpid();
        requireOwner(!loadedModulePath.empty() && loadedModulePath.size() < 4096);
        admitted["snapshot_path"] = loadedModulePath;
        requireOwner(send(admitted)); // No display has been created at this point.
        RecordReader reader;
        std::future<NativeVirtualDisplayResult> pending;
        while (!(stopRequested && stopRequested())) {
            requireOwner(getppid() == request.parentProcess);
            if (!createRequested) {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (now >= request.createDeadlineUnixMilliseconds)
                    break;
            }
            const auto incoming = reader.poll(STDIN_FILENO, VirtualDisplayOwnerRecordLimit, true);
            if (incoming == RecordReader::Outcome::Closed)
                break; // Parent death/EOF cannot leave an orphan owner alive.
            requireOwner(incoming != RecordReader::Outcome::Invalid);
            if (incoming == RecordReader::Outcome::Complete) {
                const auto bytes = reader.take();
                const auto command = parseVirtualDisplayOwnerJson(std::string(bytes.begin(), bytes.end()));
                if (command.value("type", "") == "stop") {
                    validateVirtualDisplayOwnerMessage(command, request, "stop", 6);
                    const auto remaining = virtualDisplayOwnerNumber(command.at("remaining_ms"), 5000);
                    stopBudget = std::min(100ms, std::chrono::milliseconds(remaining / 2));
                    break;
                }
                validateVirtualDisplayOwnerMessage(command, request, "create", 5);
                requireOwner(!createRequested && getppid() == request.parentProcess && !(stopRequested && stopRequested()));
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                const auto remaining = request.createDeadlineUnixMilliseconds - now;
                requireOwner(remaining > 0 && remaining <= 2000);
                createRequested = true;
                display = std::make_shared<NativeVirtualDisplay>();
                auto completion = std::make_shared<std::promise<NativeVirtualDisplayResult>>();
                pending = completion->get_future();
                display->create(request.spec, Clock::now() + std::chrono::milliseconds(remaining),
                    [completion](NativeVirtualDisplayResult result) { completion->set_value(std::move(result)); });
            }
            if (createRequested && !readySent && pending.valid() && pending.wait_for(0ms) == std::future_status::ready) {
                started = pending.get();
                requireOwner(started.success && started.display);
                // Constructing NSApplication before virtual-display creation can
                // cache the preexisting mode. Keep the creator CoreGraphics-only
                // until its exact requested mode has been independently verified.
                prepareBackgroundWorkerApplication();
                auto ready = virtualDisplayOwnerMessage(request, "ready");
                ready["pid"] = getpid();
                ready["display"] = started.display;
                ready["width"] = request.spec.width;
                ready["height"] = request.spec.height;
                ready["refresh_rate"] = 60;
                requireOwner(send(ready));
                readySent = true;
            }
            pump();
            if (readySent) {
                // A display-only owner has no media work. Wait for private
                // close/EOF rather than spinning a source-less NSRunLoop.
                pollfd command{STDIN_FILENO, POLLIN | POLLHUP, 0};
                poll(&command, 1, 20);
            }
        }
    } catch (...) {
        // No raw profile/input content is logged. Cleanup below still runs for
        // malformed commands, partial pipes and a missing Ready acknowledgment.
    }
    if (!display)
        return {false, true, 0, "Virtual display owner stopped before creation."};
    auto completion = std::make_shared<std::promise<NativeVirtualDisplayResult>>();
    auto stopped = completion->get_future();
    const auto deadline = Clock::now() + stopBudget;
    display->close(deadline, [completion](NativeVirtualDisplayResult result) { completion->set_value(std::move(result)); });
    while (Clock::now() < deadline && stopped.wait_for(0ms) != std::future_status::ready)
        pump();
    NativeVirtualDisplayResult result{false, false, started.display, "Virtual display owner requires verified process exit."};
    if (stopped.wait_for(0ms) == std::future_status::ready)
        result = stopped.get();
    auto released = virtualDisplayOwnerMessage(request, "released");
    released["pid"] = getpid();
    released["display"] = result.display;
    released["cleanup_verified"] = result.cleanupVerified;
    send(released);
    // In particular, do not change cleanupVerified=false while an ID remains
    // online. The generic host will exit this child; the parent proves absence.
    return result;
}
} // namespace dockpipe::desktop
