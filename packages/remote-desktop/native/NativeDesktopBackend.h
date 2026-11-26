#pragma once

#include "CaptureProfile.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dockpipe::desktop {

enum class Outcome { Success, Cancelled, Failure };
struct CleanupComponent {
    bool known = false;
    bool verified = false;
};
struct CleanupSnapshot {
    CleanupComponent capture;
    CleanupComponent encoder;
    CleanupComponent input;
};
struct Result {
    Outcome outcome = Outcome::Failure;
    std::string error;
    bool cleanupVerified = false;
    CleanupSnapshot cleanup{};
    explicit operator bool() const { return outcome == Outcome::Success; }
};
using Completion = std::function<void(Result)>;
using Clock = std::chrono::steady_clock;

struct NativePermissionStatus {
    bool screenCapture = false;
    bool accessibility = false;
    bool eventPosting = false;
};
enum class NativePermission { ScreenCapture, InputControl };

// Construct only from the worker's authenticated, target-bound grant. Renewal
// can extend this session's lease, never broaden its capture/input authority.
struct SessionGrant {
    bool captureAllowed = false;
    bool inputAllowed = false;
    Clock::time_point expiresAt;
    CaptureProfile capture;
};

struct EncodedFrame {
    std::vector<std::uint8_t> annexB;
    std::int64_t presentationMicroseconds = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool keyframe = false;
};
struct CaptureCallbacks {
    // Called on the bounded serial media queue. The sink must return promptly;
    // it owns transport backpressure and must never journal video or raw input.
    std::function<void(EncodedFrame)> frame;
    std::function<void(Result)> stopped;
};

enum class InputKind { Key, PointerMove, Button, Scroll };
struct InputEvent {
    std::uint64_t sequence = 0;
    InputKind kind = InputKind::PointerMove;
    std::uint16_t hidUsage = 0; // USB HID keyboard usage page 0x07, physical identity.
    bool down = false;
    std::uint8_t button = 0; // Left=0, right=1, middle=2.
    double x = 0;
    double y = 0; // Finite normalized coordinates in the captured display.
    std::int16_t scrollX = 0;
    std::int16_t scrollY = 0; // Pixel deltas; no text, clipboard or shell commands.
};

class NativeVirtualDisplay;

class NativeDesktopBackend final {
public:
    using VirtualDisplayFactory = std::function<std::shared_ptr<NativeVirtualDisplay>()>;

    explicit NativeDesktopBackend(VirtualDisplayFactory virtualDisplayFactory = {});
    ~NativeDesktopBackend();
    NativeDesktopBackend(const NativeDesktopBackend &) = delete;
    NativeDesktopBackend &operator=(const NativeDesktopBackend &) = delete;

    static NativePermissionStatus preflight();
    // Explicit native operator action only. Never called from start, renew,
    // input, discovery or a remotely supplied "allow" field.
    static void requestPermission(NativePermission permission);

    void start(SessionGrant grant, CaptureCallbacks callbacks, Completion completion);
    void renewLease(Clock::time_point expiresAt);
    void input(InputEvent event, Completion completion);
    void requestKeyframe();
    void stop(Completion completion = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dockpipe::desktop
