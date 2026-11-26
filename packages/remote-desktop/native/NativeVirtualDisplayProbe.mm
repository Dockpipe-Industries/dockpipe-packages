#include "NativeVirtualDisplay.h"
#include "CaptureProfile.h"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <string_view>
#include <vector>

using namespace dockpipe::desktop;
using namespace std::chrono_literals;

namespace {
struct Connection {
    std::size_t width;
    std::size_t height;
    CGDirectDisplayID mirror;
    bool mirrored;
};

bool online(std::vector<CGDirectDisplayID> &ids)
{
    uint32_t before = 0;
    uint32_t actual = 0;
    uint32_t after = 0;
    CGDirectDisplayID values[64];
    if (CGGetOnlineDisplayList(0, nullptr, &before) != kCGErrorSuccess || before > 64
        || CGGetOnlineDisplayList(64, values, &actual) != kCGErrorSuccess || actual > 64
        || CGGetOnlineDisplayList(0, nullptr, &after) != kCGErrorSuccess || after > 64
        || before != actual || after != actual)
        return false;
    ids.assign(values, values + actual);
    return true;
}

bool fallback(CGDirectDisplayID display)
{
    return CGDisplayVendorNumber(display) == 0x756e6b6e && CGDisplayModelNumber(display) == 0x76697274;
}

struct Baseline {
    std::map<CGDirectDisplayID, Connection> connections;
    CGDirectDisplayID protectedMain = 0;
    bool capture()
    {
        std::vector<CGDirectDisplayID> ids;
        if (!online(ids))
            return false;
        for (const auto id : ids) {
            if (fallback(id))
                continue;
            connections.emplace(id, Connection{CGDisplayPixelsWide(id), CGDisplayPixelsHigh(id),
                CGDisplayMirrorsDisplay(id), bool(CGDisplayIsInMirrorSet(id))});
            if (id == CGMainDisplayID())
                protectedMain = id;
        }
        return true;
    }
    bool preserved(const std::vector<CGDirectDisplayID> &ids) const
    {
        if (protectedMain && CGMainDisplayID() != protectedMain)
            return false;
        for (const auto &[id, before] : connections) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()
                || CGDisplayPixelsWide(id) != before.width || CGDisplayPixelsHigh(id) != before.height
                || CGDisplayMirrorsDisplay(id) != before.mirror || bool(CGDisplayIsInMirrorSet(id)) != before.mirrored)
                return false;
        }
        return true;
    }
};

NativeVirtualDisplayResult await(std::future<NativeVirtualDisplayResult> &future)
{
    const auto deadline = NativeVirtualDisplay::Clock::now() + 4s;
    while (future.wait_for(0ms) != std::future_status::ready) {
        if (NativeVirtualDisplay::Clock::now() >= deadline)
            return {false, false, 0, "Virtual display callback exceeded the probe bound."};
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }
    }
    return future.get();
}

void emit(NSDictionary *value)
{
    NSData *json = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    if (json) {
        fwrite(json.bytes, 1, json.length, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}
} // namespace

int main(int argc, char **argv)
{
    // This executable is built, but never registered as a CTest. Display effects
    // require this explicit flag and a separately reviewed native invocation.
    if (argc != 2 || std::string_view(argv[1]) != "--native-lifecycle") {
        fputs("Explicit --native-lifecycle authorization is required.\n", stderr);
        return 2;
    }
    @autoreleasepool {
        if (!NativeVirtualDisplay::available()) {
            emit(@{@"schema": @"dockpipe.desktop.virtual-display-probe/v1", @"outcome": @"unsupported"});
            return 1;
        }
        Baseline baseline;
        if (!baseline.capture())
            return 1;
        unsigned completed = 0;
        for (unsigned round = 0; round < 2; ++round) {
            for (const auto quality : {CaptureQuality::Low, CaptureQuality::Balanced, CaptureQuality::High, CaptureQuality::Ultra}) {
                const auto preset = capturePreset({quality, 30, DisplaySource::Virtual});
                if (!preset)
                    return 1;
                NativeVirtualDisplay owner;
                auto creating = std::make_shared<std::promise<NativeVirtualDisplayResult>>();
                auto createdFuture = creating->get_future();
                owner.create({preset->maximumWidth, preset->maximumHeight}, NativeVirtualDisplay::Clock::now() + 2s,
                    [creating](auto result) { creating->set_value(std::move(result)); });
                const auto created = await(createdFuture);
                std::vector<CGDirectDisplayID> during;
                const bool preservedDuring = online(during) && baseline.preserved(during);
                auto closing = std::make_shared<std::promise<NativeVirtualDisplayResult>>();
                auto closedFuture = closing->get_future();
                owner.close(NativeVirtualDisplay::Clock::now() + 3s,
                    [closing](auto result) { closing->set_value(std::move(result)); });
                const auto closed = await(closedFuture);
                std::vector<CGDirectDisplayID> after;
                const bool observed = online(after);
                const bool absent = observed && created.display
                    && std::find(after.begin(), after.end(), created.display) == after.end();
                const bool preservedAfter = observed && baseline.preserved(after);
                const bool passed = created.success && preservedDuring && closed.cleanupVerified && absent && preservedAfter;
                emit(@{@"schema": @"dockpipe.desktop.virtual-display-probe/v1", @"round": @(round),
                    @"width": @(preset->maximumWidth), @"height": @(preset->maximumHeight), @"refresh_rate": @60,
                    @"display": @(created.display), @"created": @(created.success),
                    @"cleanup_verified": @(closed.cleanupVerified), @"online_absent": @(absent),
                    @"connections_preserved": @(preservedDuring && preservedAfter), @"passed": @(passed)});
                if (!passed)
                    return 1;
                ++completed;
            }
        }
        emit(@{@"schema": @"dockpipe.desktop.virtual-display-probe/v1", @"outcome": @"passed", @"completed": @(completed)});
        return completed == 8 ? 0 : 1;
    }
}
