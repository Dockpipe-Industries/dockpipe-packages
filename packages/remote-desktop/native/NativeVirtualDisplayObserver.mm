#include "NativeVirtualDisplayObserver.h"
#include "NativeVirtualDisplayOwnerProtocol.h"
#include "worker/ChildProcess.h"
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>
#include <sys/stat.h>

namespace dockpipe::desktop {
namespace {
using Json = nlohmann::json;
using Clock = NativeVirtualDisplay::Clock;
using namespace std::chrono_literals;
void requireObserver(bool value)
{
    if (!value)
        throw std::runtime_error("Fresh virtual display observer failed.");
}
std::int64_t wallMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
bool send(const Json &value)
{
    const auto line = value.dump() + "\n";
    return line.size() <= VirtualDisplayOwnerRecordLimit
        && writeBytes(STDOUT_FILENO, reinterpret_cast<const std::uint8_t *>(line.data()), line.size(), Clock::now() + 50ms);
}
Json readback(const VirtualDisplayObserverRequest &request)
{
    std::uint32_t before = 0;
    std::uint32_t count = 0;
    std::uint32_t after = 0;
    CGDirectDisplayID ids[64];
    const bool complete = CGGetOnlineDisplayList(0, nullptr, &before) == kCGErrorSuccess && before <= 64
        && CGGetOnlineDisplayList(64, ids, &count) == kCGErrorSuccess && count <= 64
        && CGGetOnlineDisplayList(0, nullptr, &after) == kCGErrorSuccess && before == count && after == count;
    const bool present = complete && std::find(ids, ids + count, request.display) != ids + count;
    const auto display = request.display;
    const auto bounds = present ? CGDisplayBounds(display) : CGRectZero;
    struct ModeReference {
        CGDisplayModeRef value;
        ~ModeReference()
        {
            if (value)
                CFRelease(value);
        }
    } retained{present ? CGDisplayCopyDisplayMode(display) : nullptr};
    const auto mode = retained.value;
    Json result{{"online_complete", complete}, {"online", present && CGDisplayIsOnline(display)},
        {"active", present && CGDisplayIsActive(display)}, {"mirrored", present && CGDisplayIsInMirrorSet(display)},
        {"mirror_target", present ? CGDisplayMirrorsDisplay(display) : 0},
        {"vendor", present ? CGDisplayVendorNumber(display) : 0}, {"product", present ? CGDisplayModelNumber(display) : 0},
        {"serial", present ? CGDisplaySerialNumber(display) : 0},
        {"pixels_width", present ? CGDisplayPixelsWide(display) : 0}, {"pixels_height", present ? CGDisplayPixelsHigh(display) : 0},
        {"bounds_width", bounds.size.width}, {"bounds_height", bounds.size.height}, {"mode_present", mode != nullptr},
        {"mode_width", mode ? CGDisplayModeGetWidth(mode) : 0}, {"mode_height", mode ? CGDisplayModeGetHeight(mode) : 0},
        {"mode_pixel_width", mode ? CGDisplayModeGetPixelWidth(mode) : 0},
        {"mode_pixel_height", mode ? CGDisplayModeGetPixelHeight(mode) : 0},
        {"refresh_rate", mode ? CGDisplayModeGetRefreshRate(mode) : 0}};
    return result;
}
}

NativeVirtualDisplayResult runVirtualDisplayObserverChild(const VirtualDisplayObserverRequest &request,
    const std::function<bool()> &stopRequested, const std::string &loadedModulePath)
{
    try {
        requireObserver([NSThread isMainThread] && getppid() == request.parentProcess
            && !loadedModulePath.empty() && loadedModulePath.size() < 4096);
        for (int descriptor : {STDIN_FILENO, STDOUT_FILENO}) {
            struct stat metadata{};
            const int flags = fcntl(descriptor, F_GETFL);
            requireObserver(fstat(descriptor, &metadata) == 0 && S_ISFIFO(metadata.st_mode) && flags >= 0
                && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0);
        }
        auto admitted = virtualDisplayObserverMessage(request, "admitted");
        admitted["pid"] = getpid();
        admitted["snapshot_path"] = loadedModulePath;
        requireObserver(send(admitted));
        RecordReader reader;
        while (!(stopRequested && stopRequested()) && getppid() == request.parentProcess
            && wallMilliseconds() < request.deadlineUnixMilliseconds) {
            const auto incoming = reader.poll(STDIN_FILENO, VirtualDisplayOwnerRecordLimit, true);
            if (incoming == RecordReader::Outcome::Closed)
                break;
            requireObserver(incoming != RecordReader::Outcome::Invalid);
            if (incoming == RecordReader::Outcome::Complete) {
                const auto bytes = reader.take();
                const auto command = parseVirtualDisplayOwnerJson(std::string(bytes.begin(), bytes.end()));
                validateVirtualDisplayObserverMessage(command, request, "observe", 6);
                const auto remaining = request.deadlineUnixMilliseconds - wallMilliseconds();
                requireObserver(remaining > 0 && remaining <= 2000 && getppid() == request.parentProcess
                    && !(stopRequested && stopRequested()));
                auto observed = virtualDisplayObserverMessage(request, "observed");
                observed["pid"] = getpid();
                // First mode query in this fresh process, after the display is
                // already created. No NSApplication or display mutation occurs.
                observed["observation"] = readback(request);
                requireObserver(wallMilliseconds() < request.deadlineUnixMilliseconds
                    && getppid() == request.parentProcess && !(stopRequested && stopRequested()));
                requireObserver(send(observed));
                parseVirtualDisplayObservation(observed, request, getpid());
                return {true, true, request.display, {}};
            }
            pollfd command{STDIN_FILENO, POLLIN | POLLHUP, 0};
            poll(&command, 1, 5);
        }
    } catch (...) {
        // Public queries and their released CF mode reference own no display,
        // capture or input resources. Failure is not a display-cleanup claim.
    }
    return {false, true, request.display, "Fresh virtual display mode was not verified."};
}
} // namespace dockpipe::desktop
