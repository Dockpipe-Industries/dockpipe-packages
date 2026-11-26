#include "InputState.h"
#include "H264Contract.h"
#include "TestSps.h"
#include "CaptureLifecycle.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace dockpipe::desktop;
int main()
{
    const auto sps = test::SpsFixture{}.nal();
    assert(validatedSps(sps.data(), sps.size()));
    assert(!validatedSps(sps.data(), 3));
    assert(!validatedSps(nullptr, 4));
    const auto now = Clock::now();
    unsigned calls = 0;
    NativeInput last{InputKind::Key};
    auto sink = [&](const NativeInput &event) { ++calls; last = event; return true; };
    assert(quartzKeyForHid(4) == 0);
    assert(quartzKeyForHid(225) == 56);
    assert(!quartzKeyForHid(57));
    assert(!quartzKeyForHid(65535));
    InputEvent key;
    key.kind = InputKind::Key;
    key.hidUsage = 4;
    key.sequence = 1;
    key.down = true;
    InputState viewOnly(false, now + std::chrono::seconds(10));
    assert(!viewOnly.apply(key, now, sink));
    assert(calls == 0);
    InputState control(true, now + std::chrono::seconds(10));
    assert(!control.apply(key, now, [](const NativeInput &) { return false; }));
    assert(control.pressedCount() == 0);
    assert(control.apply(key, now, sink));
    assert(control.pressedCount() == 1 && !last.repeated);
    assert(!control.apply(key, now, sink));
    key.sequence = 3;
    assert(!control.apply(key, now, sink));
    key.sequence = 2;
    assert(control.apply(key, now, sink) && last.repeated);
    key.sequence = 3;
    key.hidUsage = 225;
    assert(control.apply(key, now, sink));
    assert(last.modifierFlags & (1ULL << 17));
    key.sequence = 4;
    key.hidUsage = 229;
    assert(control.apply(key, now, sink));
    key.sequence = 5;
    key.hidUsage = 225;
    key.down = false;
    assert(control.apply(key, now, sink));
    assert(last.modifierFlags & (1ULL << 17));
    InputEvent pointer;
    pointer.sequence = 6;
    pointer.x = std::numeric_limits<double>::quiet_NaN();
    assert(!control.apply(pointer, now, sink));
    pointer.x = 0.5;
    pointer.y = 0.5;
    pointer.kind = InputKind::Button;
    pointer.down = true;
    assert(control.apply(pointer, now, sink));
    assert(control.pressedCount() == 3);
    assert(!control.disconnect([](const NativeInput &) { return false; }));
    assert(control.pressedCount() == 3);
    assert(control.disconnect(sink));
    assert(control.pressedCount() == 0);
    const auto afterRelease = calls;
    assert(control.disconnect(sink));
    assert(calls == afterRelease);
    assert(!control.renew(now + std::chrono::seconds(20), now));
    InputState expired(true, now + std::chrono::seconds(1));
    key = {};
    key.sequence = 1;
    key.kind = InputKind::Key;
    key.hidUsage = 4;
    key.down = true;
    assert(expired.apply(key, now, sink));
    key.sequence = 2;
    const auto result = expired.apply(key, now + std::chrono::seconds(2), sink);
    assert(result.outcome == Outcome::Cancelled && expired.pressedCount() == 0);
    assert(!expired.renew(now + std::chrono::seconds(30), now + std::chrono::seconds(2)));
    InputState renewable(true, now + std::chrono::seconds(10));
    assert(renewable.renew(now + std::chrono::seconds(20), now));
    assert(!renewable.renew(now + std::chrono::seconds(15), now));

    CaptureLifecycle cancelled;
    assert(!cancelled.begin(false));
    assert(cancelled.begin(true));
    assert(cancelled.stop() == CaptureLifecycle::StopAction::AwaitStart);
    assert(!cancelled.acceptsFrames());
    // Fake native start completes after cancellation: exactly one cleanup,
    // with no frame publication or successful start transition.
    unsigned nativeStops = 0;
    if (cancelled.started(true))
        ++nativeStops;
    assert(nativeStops == 1 && !cancelled.acceptsFrames());
    cancelled.stopped(false);
    assert(cancelled.phase() == CaptureLifecycle::Phase::Unknown);
    assert(!cancelled.begin(true));
    assert(cancelled.started(true));
    cancelled.stopped(true);
    assert(cancelled.stop() == CaptureLifecycle::StopAction::Complete);
    CaptureLifecycle running;
    assert(running.begin(true));
    assert(!running.started(true) && running.acceptsFrames());
    assert(running.stop() == CaptureLifecycle::StopAction::StopNative);
    assert(running.stop() == CaptureLifecycle::StopAction::AwaitStop);
    assert(!running.acceptsFrames());
    std::cout << "PASS mapping, view-only, sequence, bounded owned state, modifiers, failed posting, expiry, release, late-start cancellation, unknown cleanup\n";
}
