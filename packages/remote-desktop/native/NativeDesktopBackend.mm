#include "NativeDesktopBackend.h"
#include "CaptureLifecycle.h"
#include "NativeVirtualDisplay.h"
#include "InputState.h"
#include "H264Contract.h"

#import <Foundation/Foundation.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <exception>
#include <utility>

namespace dockpipe::desktop {
namespace {
constexpr std::size_t MaximumEncodedBytes = 2 * 1024 * 1024;
constexpr unsigned MaximumEncodingFrames = 2;

struct MacState {
    dispatch_queue_t queue = dispatch_queue_create("dockpipe.desktop.native", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t leaseTimer = nullptr;
    dispatch_source_t displayTimer = nullptr;
    NativeDesktopBackend::VirtualDisplayFactory virtualDisplayFactory;
    std::shared_ptr<NativeVirtualDisplay> virtualDisplay;
    bool virtualCreationPending = false;
    bool virtualCleanupStarted = false;
    Clock::time_point stopDeadline;
    SCStream *__strong stream = nil;
    VTCompressionSessionRef encoder = nullptr;
    CGDirectDisplayID display = 0;
    CGRect displayBounds{}; // Logical points remain the input coordinate space.
    std::size_t displayPixelWidth = 0;
    std::size_t displayPixelHeight = 0;
    CaptureProfile profile;
    unsigned width = 0;
    unsigned height = 0;
    unsigned encodingFrames = 0;
    std::atomic_bool forceKeyframe{true};
    std::atomic_uint pendingInput{0};
    bool encoderCleanupDone = true;
    bool captureCleanupDone = false;
    bool captureCleanupVerified = false;
    bool nativeStartPending = false;
    bool stopping = false;
    bool stopCompleted = false;
    bool stoppedNotified = false;
    bool inputCleanupVerified = true;
    CleanupSnapshot cleanup;
    CaptureLifecycle lifecycle;
    std::unique_ptr<InputState> input;
    CaptureCallbacks callbacks;
    Completion startCompletion;
    Completion stopCompletion;
    Result stopReason{Outcome::Success, {}};
    CMTime lastTimestamp = kCMTimeInvalid;
};

void stopOnQueue(std::shared_ptr<MacState> state, Completion completion = {}, Result reason = {Outcome::Success, {}});

void complete(Completion &completion, Result result)
{
    auto callback = std::move(completion);
    completion = {};
    if (callback)
        callback(std::move(result));
}

bool approvedDisplayUnchanged(const std::shared_ptr<MacState> &state)
{
    const bool expectedSource = state->profile.displaySource == DisplaySource::Virtual
        ? state->virtualDisplay && state->virtualDisplay->ownerAlive()
            && CGDisplayIsOnline(state->display) && CGDisplayIsActive(state->display)
        : CGMainDisplayID() == state->display;
    return expectedSource && state->display
        && CGRectEqualToRect(CGDisplayBounds(state->display), state->displayBounds)
        && CGDisplayPixelsWide(state->display) == state->displayPixelWidth
        && CGDisplayPixelsHigh(state->display) == state->displayPixelHeight;
}

bool postNative(const std::shared_ptr<MacState> &state, const NativeInput &input)
{
    // TCC is checked at the posting boundary, including cleanup releases. A
    // revoked permission must not become a false successful cleanup receipt.
    if ((!state->stopping && !approvedDisplayUnchanged(state))
        || !CGPreflightPostEventAccess() || !AXIsProcessTrusted())
        return false;
    CGEventRef event = nullptr;
    const auto point = CGPointMake(state->displayBounds.origin.x + input.x * std::max(0.0, state->displayBounds.size.width - 1),
        state->displayBounds.origin.y + input.y * std::max(0.0, state->displayBounds.size.height - 1));
    switch (input.kind) {
    case InputKind::Key:
        event = CGEventCreateKeyboardEvent(nullptr, input.quartzKey, input.down);
        if (event)
            CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, input.repeated ? 1 : 0);
        break;
    case InputKind::Button: {
        CGEventType type;
        if (input.button == 0)
            type = input.down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        else if (input.button == 1)
            type = input.down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        else
            type = input.down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        event = CGEventCreateMouseEvent(nullptr, type, point, static_cast<CGMouseButton>(input.button));
        break;
    }
    case InputKind::PointerMove: {
        CGEventType type = kCGEventMouseMoved;
        CGMouseButton button = kCGMouseButtonLeft;
        if (input.pressedButtons[0])
            type = kCGEventLeftMouseDragged;
        else if (input.pressedButtons[1]) {
            type = kCGEventRightMouseDragged;
            button = kCGMouseButtonRight;
        } else if (input.pressedButtons[2]) {
            type = kCGEventOtherMouseDragged;
            button = kCGMouseButtonCenter;
        }
        event = CGEventCreateMouseEvent(nullptr, type, point, button);
        break;
    }
    case InputKind::Scroll:
        event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2,
            static_cast<int32_t>(input.scrollY), static_cast<int32_t>(input.scrollX));
        break;
    }
    if (!event)
        return false;
    CGEventSetFlags(event, static_cast<CGEventFlags>(input.modifierFlags));
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return true;
}

void finishStop(const std::shared_ptr<MacState> &state, bool nativeVerified)
{
    if (state->stopCompleted)
        return;
    state->stopCompleted = true;
    if (!state->virtualDisplay || nativeVerified)
        state->stream = nil;
    const bool verified = nativeVerified && state->inputCleanupVerified;
    state->lifecycle.stopped(verified);
    if (!verified)
        state->stopReason = {Outcome::Failure, "Native desktop cleanup remains unconfirmed."};
    state->stopReason.cleanupVerified = verified;
    state->stopReason.cleanup = state->cleanup;
    complete(state->startCompletion, {Outcome::Cancelled, "Desktop start was cancelled."});
    complete(state->stopCompletion, state->stopReason);
    if (!state->stoppedNotified && state->callbacks.stopped) {
        state->stoppedNotified = true;
        state->callbacks.stopped(state->stopReason);
    }
}

void completeNativeCleanup(const std::shared_ptr<MacState> &state)
{
    if (state->stopCompleted || !state->captureCleanupDone || !state->encoderCleanupDone
        || state->virtualCreationPending)
        return;
    if (!state->virtualDisplay) {
        finishStop(state, state->captureCleanupVerified);
        return;
    }
    if (state->virtualCleanupStarted)
        return;
    state->virtualCleanupStarted = true;
    // Keep the display registered if capture or held-input cleanup is unknown.
    // Releasing the last display owner is not a substitute for those proofs.
    if (!state->captureCleanupVerified || !state->inputCleanupVerified
        || !state->cleanup.encoder.known || !state->cleanup.encoder.verified) {
        state->cleanup.capture = {true, false};
        finishStop(state, false);
        return;
    }
    @autoreleasepool {
        state->stream = nil; // Drain SCK display/filter ownership before removal.
    }
    state->cleanup.capture = {};
    state->virtualDisplay->close(state->stopDeadline, [state](NativeVirtualDisplayResult result) {
        dispatch_async(state->queue, ^{
            if (state->stopCompleted)
                return;
            state->cleanup.capture = {true, result.cleanupVerified};
            finishStop(state, result.success && result.cleanupVerified);
        });
    });
}

void captureStopped(const std::shared_ptr<MacState> &state, bool verified)
{
    state->captureCleanupDone = true;
    state->captureCleanupVerified = verified;
    state->cleanup.capture = state->virtualDisplay ? CleanupComponent{} : CleanupComponent{true, verified};
    completeNativeCleanup(state);
}

void stopStream(std::shared_ptr<MacState> state)
{
    if (!state->stream) {
        captureStopped(state, true);
        return;
    }
    [state->stream stopCaptureWithCompletionHandler:^(NSError *error) {
        dispatch_async(state->queue, ^{ captureStopped(state, error == nil); });
    }];
}

void stopOnQueue(std::shared_ptr<MacState> state, Completion completion, Result reason)
{
    if (completion) {
        if (state->stopCompletion) {
            completion({Outcome::Failure, "A desktop stop is already pending."});
            return;
        }
        state->stopCompletion = std::move(completion);
    }
    if (state->stopCompleted) {
        complete(state->stopCompletion, state->stopReason);
        return;
    }
    if (state->stopping)
        return;
    state->stopping = true;
    state->stopDeadline = Clock::now() + std::chrono::seconds(5);
    state->stopReason = std::move(reason);
    if (state->leaseTimer) {
        dispatch_source_cancel(state->leaseTimer);
        state->leaseTimer = nullptr;
    }
    if (state->displayTimer) {
        dispatch_source_cancel(state->displayTimer);
        state->displayTimer = nullptr;
    }
    if (state->input) {
        state->inputCleanupVerified = bool(state->input->disconnect([state](const NativeInput &event) {
            return postNative(state, event);
        }));
    }
    state->cleanup.input = {true, state->inputCleanupVerified};
    state->cleanup.encoder = {state->encoderCleanupDone, state->encoderCleanupDone};
    if (state->encoder) {
        state->cleanup.encoder = {};
        // Hardware finalization may wait inside an OS driver. Do not block the
        // lease/input queue or its watchdog on that wait. One finalizer exists
        // per backend; a timed-out cleanup remains unverified and cannot be
        // used to authorize a replacement provider process.
        VTCompressionSessionRef encoder = state->encoder;
        state->encoder = nullptr;
        state->encoderCleanupDone = false;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            const OSStatus status = VTCompressionSessionCompleteFrames(encoder, kCMTimeInvalid);
            VTCompressionSessionInvalidate(encoder);
            CFRelease(encoder);
            dispatch_async(state->queue, ^{
                state->encoderCleanupDone = true;
                state->cleanup.encoder = {true, status == noErr};
                if (status != noErr)
                    state->inputCleanupVerified = false;
                completeNativeCleanup(state);
            });
        });
    }
    const auto action = state->lifecycle.stop();
    if (action == CaptureLifecycle::StopAction::Complete || !state->nativeStartPending)
        stopStream(state);
    // A pending start must finish first; its late completion immediately stops.
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(state->stopDeadline - Clock::now()).count();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, std::max<int64_t>(0, remaining)), state->queue, ^{
        if (!state->stopCompleted) {
            state->stopCompleted = true; // Late native acknowledgments cannot upgrade Unknown.
            state->lifecycle.stopped(false);
            state->stopReason = {Outcome::Failure, "Native desktop stop exceeded five seconds; provider cleanup is unconfirmed."};
            state->stopReason.cleanup = state->cleanup;
            complete(state->startCompletion, {Outcome::Cancelled, "Desktop start was cancelled."});
            complete(state->stopCompletion, state->stopReason);
            if (!state->stoppedNotified && state->callbacks.stopped) {
                state->stoppedNotified = true;
                state->callbacks.stopped(state->stopReason);
            }
        }
    });
}

bool appendNAL(std::vector<std::uint8_t> &output, const std::uint8_t *data, std::size_t size)
{
    if (!data || size == 0 || size > MaximumEncodedBytes - 4 || output.size() > MaximumEncodedBytes - size - 4)
        return false;
    output.insert(output.end(), {0, 0, 0, 1});
    output.insert(output.end(), data, data + size);
    return true;
}

Result encodedFrame(CMSampleBufferRef sample, EncodedFrame &frame, CaptureProfile profile)
{
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block)
        return {Outcome::Failure, "The native H264 encoder returned no data buffer."};
    const auto size = CMBlockBufferGetDataLength(block);
    if (size == 0 || size > MaximumEncodedBytes)
        return {Outcome::Failure, "Native H264 frame exceeds the two MiB output limit."};
    const auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    frame.keyframe = true;
    if (attachments && CFArrayGetCount(attachments) > 0) {
        const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        const auto notSync = CFDictionaryGetValue(dictionary, kCMSampleAttachmentKey_NotSync);
        frame.keyframe = notSync != kCFBooleanTrue;
    }
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    const std::uint8_t *parameter = nullptr;
    std::size_t parameterSize = 0;
    std::size_t parameterCount = 0;
    int lengthBytes = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &parameter, &parameterSize, &parameterCount, &lengthBytes) != noErr
        || parameterCount != 2 || lengthBytes != 4)
        return {Outcome::Failure, "The encoder returned an unsupported H264 baseline layout."};
    const auto bounds = captureCodecBounds(profile);
    const auto parameters = bounds ? validatedSps(parameter, parameterSize, *bounds) : std::nullopt;
    if (!parameters || parameters->width != frame.width || parameters->height != frame.height)
        return {Outcome::Failure, "Native H264 SPS exceeds the frozen capture profile or geometry."};
    frame.annexB.reserve(std::min(MaximumEncodedBytes, size + std::size_t(4096)));
    if (frame.keyframe) {
        for (std::size_t index = 0; index < parameterCount; ++index) {
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, index, &parameter, &parameterSize, nullptr, nullptr) != noErr
                || !appendNAL(frame.annexB, parameter, parameterSize))
                return {Outcome::Failure, "Native H264 parameter sets exceed their output contract."};
        }
    }
    std::vector<std::uint8_t> avcc(size);
    if (CMBlockBufferCopyDataBytes(block, 0, size, avcc.data()) != noErr)
        return {Outcome::Failure, "Native H264 output could not be copied."};
    std::size_t offset = 0;
    while (offset < avcc.size()) {
        if (avcc.size() - offset < 4)
            return {Outcome::Failure, "Truncated native H264 NAL header."};
        const std::size_t length = (std::size_t(avcc[offset]) << 24) | (std::size_t(avcc[offset + 1]) << 16)
            | (std::size_t(avcc[offset + 2]) << 8) | std::size_t(avcc[offset + 3]);
        offset += 4;
        if (length > avcc.size() - offset || !appendNAL(frame.annexB, avcc.data() + offset, length))
            return {Outcome::Failure, "Invalid or oversized native H264 NAL unit."};
        offset += length;
    }
    const auto timestamp = CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample), 1000000, kCMTimeRoundingMethod_Default);
    if (!CMTIME_IS_NUMERIC(timestamp) || timestamp.value < 0)
        return {Outcome::Failure, "Invalid native H264 timestamp."};
    frame.presentationMicroseconds = timestamp.value;
    return {Outcome::Success, {}};
}

struct EncodingTicket { std::shared_ptr<MacState> state; };

void encoderOutput(void *, void *sourceFrame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
    std::unique_ptr<EncodingTicket> ticket(static_cast<EncodingTicket *>(sourceFrame));
    const auto state = ticket->state;
    if (sample)
        CFRetain(sample);
    dispatch_async(state->queue, ^{
        if (state->encodingFrames > 0)
            --state->encodingFrames;
        if (state->lifecycle.acceptsFrames() && !state->stopping) {
            if (status != noErr || !sample || (flags & kVTEncodeInfo_FrameDropped)) {
                state->forceKeyframe = true;
            } else {
                EncodedFrame frame;
                frame.width = state->width;
                frame.height = state->height;
                const auto result = encodedFrame(sample, frame, state->profile);
                if (!result) {
                    stopOnQueue(state, {}, result);
                } else if (state->callbacks.frame) {
                    try {
                        state->callbacks.frame(std::move(frame));
                    } catch (...) {
                        stopOnQueue(state, {}, {Outcome::Failure, "The desktop media sink failed."});
                    }
                }
            }
        }
        if (sample)
            CFRelease(sample);
    });
}

NSDictionary *encoderProperties(CaptureProfile profile)
{
    const auto preset = capturePreset(profile);
    if (!preset)
        return nil;
    return @{
        (__bridge NSString *)kVTCompressionPropertyKey_RealTime: @YES,
        (__bridge NSString *)kVTCompressionPropertyKey_AllowFrameReordering: @NO,
        (__bridge NSString *)kVTCompressionPropertyKey_ProfileLevel: (__bridge NSString *)kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel,
        (__bridge NSString *)kVTCompressionPropertyKey_ExpectedFrameRate: @(profile.frameRate),
        (__bridge NSString *)kVTCompressionPropertyKey_AverageBitRate: @(preset->bitsPerSecond),
        (__bridge NSString *)kVTCompressionPropertyKey_MaxKeyFrameInterval: @(profile.frameRate * 2),
        (__bridge NSString *)kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration: @2,
        (__bridge NSString *)kVTCompressionPropertyKey_MaxFrameDelayCount: @1,
    };
}

SCStreamConfiguration *captureConfiguration(const std::shared_ptr<MacState> &state)
{
    const auto preset = capturePreset(state->profile);
    if (!preset || state->width < 2 || state->height < 2 || state->width > preset->maximumWidth
        || state->height > preset->maximumHeight || state->width % 2 != 0 || state->height % 2 != 0)
        return nil;
    auto *configuration = [[SCStreamConfiguration alloc] init];
    configuration.width = state->width;
    configuration.height = state->height;
    configuration.minimumFrameInterval = CMTimeMake(1, state->profile.frameRate);
    configuration.queueDepth = 3;
    configuration.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    configuration.showsCursor = YES;
    configuration.capturesAudio = NO;
    return configuration;
}

NSDictionary *encoderSpecification()
{
    return @{(__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES};
}

bool hardwareEncoderConfirmed(OSStatus status, CFTypeRef value)
{
    return status == noErr && value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

Result confirmHardwareEncoder(VTCompressionSessionRef encoder)
{
    CFTypeRef value = nullptr;
    const auto status = VTSessionCopyProperty(encoder, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
        kCFAllocatorDefault, &value);
    const bool hardware = hardwareEncoderConfirmed(status, value);
    if (value)
        CFRelease(value);
    if (!hardware)
        return {Outcome::Failure, "The native H264 encoder did not confirm required hardware acceleration."};
    return {Outcome::Success, {}};
}

Result createEncoder(const std::shared_ptr<MacState> &state)
{
    NSDictionary *properties = encoderProperties(state->profile);
    if (!properties)
        return {Outcome::Failure, "The negotiated capture profile is invalid."};
    NSDictionary *specification = encoderSpecification();
    OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault, state->width, state->height, kCMVideoCodecType_H264,
        (__bridge CFDictionaryRef)specification, nullptr, nullptr, encoderOutput, nullptr, &state->encoder);
    if (status != noErr || !state->encoder)
        return {Outcome::Failure, "The native hardware H264 encoder is unavailable."};
    status = VTSessionSetProperties(state->encoder, (__bridge CFDictionaryRef)properties);
    if (status == noErr)
        status = VTCompressionSessionPrepareToEncodeFrames(state->encoder);
    if (status != noErr)
        return {Outcome::Failure, "The native H264 encoder cannot satisfy the realtime baseline configuration."};
    return confirmHardwareEncoder(state->encoder);
}
} // namespace
} // namespace dockpipe::desktop

using namespace dockpipe::desktop;

@interface DPNativeDesktopCapture : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::shared_ptr<MacState> state;
}
@end

@implementation DPNativeDesktopCapture
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    const auto owner = state;
    dispatch_async(owner->queue, ^{
        stopOnQueue(owner, {}, {Outcome::Failure, "Native display capture stopped unexpectedly."});
    });
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
    const auto owner = state;
    if (type != SCStreamOutputTypeScreen || !owner->lifecycle.acceptsFrames() || owner->stopping || !CMSampleBufferIsValid(sample))
        return;
    if (owner->input->expired(Clock::now())) {
        stopOnQueue(owner, {}, {Outcome::Cancelled, "The desktop lease expired."});
        return;
    }
    if (!approvedDisplayUnchanged(owner)) {
        stopOnQueue(owner, {}, {Outcome::Failure, "The approved display changed; start a new desktop session."});
        return;
    }
    NSArray *attachments = (__bridge NSArray *)CMSampleBufferGetSampleAttachmentsArray(sample, false);
    NSNumber *frameStatus = [attachments.firstObject objectForKey:SCStreamFrameInfoStatus];
    if (!frameStatus || frameStatus.integerValue != SCFrameStatusComplete)
        return;
    CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample);
    const auto timestamp = CMSampleBufferGetPresentationTimeStamp(sample);
    if (!image || !CMTIME_IS_NUMERIC(timestamp) || (CMTIME_IS_NUMERIC(owner->lastTimestamp) && CMTimeCompare(timestamp, owner->lastTimestamp) <= 0))
        return;
    if (owner->encodingFrames >= MaximumEncodingFrames)
        return; // Drop before retaining another raw IOSurface or queuing encoding.
    owner->lastTimestamp = timestamp;
    auto *ticket = new EncodingTicket{owner};
    ++owner->encodingFrames;
    NSDictionary *properties = owner->forceKeyframe.exchange(false) ? @{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES} : nil;
    const OSStatus status = VTCompressionSessionEncodeFrame(owner->encoder, image, timestamp, CMTimeMake(1, owner->profile.frameRate),
        (__bridge CFDictionaryRef)properties, ticket, nullptr);
    if (status != noErr) {
        delete ticket;
        --owner->encodingFrames;
        stopOnQueue(owner, {}, {Outcome::Failure, "Native H264 frame submission failed."});
    }
}
@end

namespace dockpipe::desktop {
struct NativeDesktopBackend::Impl {
    std::shared_ptr<MacState> state = std::make_shared<MacState>();
    DPNativeDesktopCapture *__strong capture = nil;
    std::atomic_bool used{false};
};

NativeDesktopBackend::NativeDesktopBackend(VirtualDisplayFactory virtualDisplayFactory)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->state->virtualDisplayFactory = std::move(virtualDisplayFactory);
}
NativeDesktopBackend::~NativeDesktopBackend() { stop(); }

NativePermissionStatus NativeDesktopBackend::preflight()
{
    return {bool(CGPreflightScreenCaptureAccess()), bool(AXIsProcessTrusted()), bool(CGPreflightPostEventAccess())};
}

void NativeDesktopBackend::requestPermission(NativePermission permission)
{
    if (permission == NativePermission::ScreenCapture) {
        CGRequestScreenCaptureAccess();
    } else {
        NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
        AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
        CGRequestPostEventAccess();
    }
}

void NativeDesktopBackend::start(SessionGrant grant, CaptureCallbacks callbacks, Completion completion)
{
    if (!validCaptureProfile(grant.capture)) {
        if (completion)
            completion({Outcome::Failure, "The negotiated capture profile is invalid."});
        return;
    }
    if (grant.capture.displaySource == DisplaySource::Virtual && !m_impl->state->virtualDisplayFactory) {
        if (completion)
            completion({Outcome::Failure, "Virtual display ownership is not configured for this worker."});
        return;
    }
    if (m_impl->used.exchange(true)) {
        if (completion)
            completion({Outcome::Failure, "A native desktop backend supports exactly one captured session."});
        return;
    }
    const auto owner = m_impl->state;
    auto *capture = [[DPNativeDesktopCapture alloc] init];
    capture->state = owner;
    m_impl->capture = capture;
    dispatch_async(owner->queue, ^{
        if (grant.expiresAt <= Clock::now() || !owner->lifecycle.begin(grant.captureAllowed)) {
            if (completion)
                completion({Outcome::Failure, "Desktop capture is not granted or this backend was already used."});
            return;
        }
        owner->profile = grant.capture;
        owner->callbacks = callbacks;
        owner->startCompletion = completion;
        owner->input = std::make_unique<InputState>(grant.inputAllowed, grant.expiresAt);
        const auto permission = NativeDesktopBackend::preflight();
        if (!permission.screenCapture) {
            complete(owner->startCompletion, {Outcome::Failure, "Screen Recording permission is required for the signed desktop provider."});
            stopOnQueue(owner);
            return;
        }
        if (owner->profile.displaySource == DisplaySource::Virtual && grant.inputAllowed
            && (!permission.accessibility || !permission.eventPosting)) {
            complete(owner->startCompletion, {Outcome::Failure, "Native input permission is required before creating a controlled virtual display."});
            stopOnQueue(owner);
            return;
        }
        owner->leaseTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, owner->queue);
        const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(grant.expiresAt - Clock::now()).count();
        dispatch_source_set_timer(owner->leaseTimer, dispatch_time(DISPATCH_TIME_NOW, std::max<int64_t>(0, delay)), DISPATCH_TIME_FOREVER, NSEC_PER_MSEC);
        dispatch_source_set_event_handler(owner->leaseTimer, ^{
            stopOnQueue(owner, {}, {Outcome::Cancelled, "The desktop lease expired."});
        });
        dispatch_resume(owner->leaseTimer);
        auto beginCapture = ^(CGDirectDisplayID displayID) {
            if (owner->stopping || owner->input->expired(Clock::now())) {
                stopOnQueue(owner, {}, {Outcome::Cancelled, "Desktop capture was cancelled before display admission."});
                return;
            }
            owner->display = displayID;
            owner->displayBounds = CGDisplayBounds(owner->display);
            owner->displayPixelWidth = CGDisplayPixelsWide(owner->display);
            owner->displayPixelHeight = CGDisplayPixelsHigh(owner->display);
            if (owner->profile.displaySource == DisplaySource::Virtual) {
                if (!approvedDisplayUnchanged(owner)) {
                    stopOnQueue(owner, {}, {Outcome::Failure, "The owned virtual display is unavailable."});
                    return;
                }
                owner->displayTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, owner->queue);
                dispatch_source_set_timer(owner->displayTimer, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC),
                    250 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
                dispatch_source_set_event_handler(owner->displayTimer, ^{
                    if (!owner->stopping && !approvedDisplayUnchanged(owner))
                        stopOnQueue(owner, {}, {Outcome::Failure, "The owned virtual display changed or disappeared."});
                });
                dispatch_resume(owner->displayTimer);
            }
            [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES completionHandler:^(SCShareableContent *content, NSError *error) {
                dispatch_async(owner->queue, ^{
                    if (owner->stopping)
                        return;
                    if (owner->input->expired(Clock::now()) || !approvedDisplayUnchanged(owner)) {
                        stopOnQueue(owner, {}, {Outcome::Cancelled, "Display admission expired or its geometry changed."});
                        return;
                    }
                    SCDisplay *selected = nil;
                    for (SCDisplay *display in content.displays) {
                        if (display.displayID == owner->display) {
                            selected = display;
                            break;
                        }
                    }
                    if (error || !selected || selected.width == 0 || selected.height == 0) {
                        complete(owner->startCompletion, {Outcome::Failure, "The approved display is unavailable."});
                        stopOnQueue(owner);
                        return;
                    }
                    const auto geometry = captureGeometry(owner->profile, owner->displayPixelWidth, owner->displayPixelHeight);
                    if (!geometry) {
                        complete(owner->startCompletion, {Outcome::Failure, "The approved display cannot satisfy the capture profile."});
                        stopOnQueue(owner);
                        return;
                    }
                    owner->width = geometry->width;
                    owner->height = geometry->height;
                    const auto encoder = createEncoder(owner);
                    if (!encoder) {
                        complete(owner->startCompletion, encoder);
                        stopOnQueue(owner);
                        return;
                    }
                    auto *configuration = captureConfiguration(owner);
                    auto *filter = [[SCContentFilter alloc] initWithDisplay:selected excludingWindows:@[]];
                    owner->stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:capture];
                    NSError *outputError = nil;
                    if (![owner->stream addStreamOutput:capture type:SCStreamOutputTypeScreen sampleHandlerQueue:owner->queue error:&outputError]) {
                        complete(owner->startCompletion, {Outcome::Failure, "Native capture output could not be attached."});
                        stopOnQueue(owner);
                        return;
                    }
                    owner->nativeStartPending = true;
                    [owner->stream startCaptureWithCompletionHandler:^(NSError *startError) {
                        dispatch_async(owner->queue, ^{
                            owner->nativeStartPending = false;
                            const bool lateStop = owner->lifecycle.started(startError == nil);
                            if (owner->stopping || lateStop) {
                                if (startError)
                                    captureStopped(owner, true);
                                else
                                    stopStream(owner);
                                return;
                            }
                            if (startError) {
                                complete(owner->startCompletion, {Outcome::Failure, "Native display capture could not start."});
                                stopOnQueue(owner);
                                return;
                            }
                            complete(owner->startCompletion, {Outcome::Success, {}});
                        });
                    }];
                });
            }];
        };
        if (owner->profile.displaySource == DisplaySource::Virtual) {
            const auto preset = capturePreset(owner->profile);
            try {
                owner->virtualDisplay = owner->virtualDisplayFactory();
            } catch (const std::exception &) {
                complete(owner->startCompletion, {Outcome::Failure, "Virtual display ownership could not be initialized."});
                stopOnQueue(owner);
                return;
            }
            if (!owner->virtualDisplay) {
                complete(owner->startCompletion, {Outcome::Failure, "Virtual display ownership is unavailable."});
                stopOnQueue(owner);
                return;
            }
            owner->virtualCreationPending = true;
            owner->virtualDisplay->create({preset->maximumWidth, preset->maximumHeight}, grant.expiresAt,
                [owner, beginCapture](NativeVirtualDisplayResult result) {
                    dispatch_async(owner->queue, ^{
                        owner->virtualCreationPending = false;
                        if (owner->stopping) {
                            completeNativeCleanup(owner);
                            return;
                        }
                        if (!result.success || !result.display) {
                            complete(owner->startCompletion, {Outcome::Failure, "Native virtual display creation was not verified."});
                            stopOnQueue(owner);
                            return;
                        }
                        beginCapture(result.display);
                    });
                });
        } else {
            beginCapture(CGMainDisplayID());
        }
    });
}

void NativeDesktopBackend::renewLease(Clock::time_point expiresAt)
{
    const auto owner = m_impl->state;
    dispatch_async(owner->queue, ^{
        if (!owner->input || owner->stopping || !owner->input->renew(expiresAt, Clock::now()))
            return;
        const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(expiresAt - Clock::now()).count();
        dispatch_source_set_timer(owner->leaseTimer, dispatch_time(DISPATCH_TIME_NOW, std::max<int64_t>(0, delay)), DISPATCH_TIME_FOREVER, NSEC_PER_MSEC);
    });
}

void NativeDesktopBackend::input(InputEvent event, Completion completion)
{
    const auto owner = m_impl->state;
    if (owner->pendingInput.fetch_add(1) >= 128) {
        owner->pendingInput.fetch_sub(1);
        if (completion)
            completion({Outcome::Failure, "Native desktop input exceeds 128 pending events."});
        return;
    }
    dispatch_async(owner->queue, ^{
        owner->pendingInput.fetch_sub(1);
        if (!owner->input || owner->stopping || !owner->lifecycle.acceptsFrames()) {
            if (completion)
                completion({Outcome::Cancelled, "The desktop session is not running."});
            return;
        }
        if (!owner->input->inputAllowed()) {
            if (completion)
                completion({Outcome::Failure, "This worker session is view-only."});
            return;
        }
        const auto permission = NativeDesktopBackend::preflight();
        if (!permission.accessibility || !permission.eventPosting) {
            if (completion)
                completion({Outcome::Failure, "Accessibility and event-posting permission are required for desktop control."});
            return;
        }
        auto result = owner->input->apply(event, Clock::now(), [owner](const NativeInput &native) {
            return postNative(owner, native);
        });
        if (completion)
            completion(std::move(result));
    });
}

void NativeDesktopBackend::requestKeyframe()
{
    const auto owner = m_impl->state;
    owner->forceKeyframe = true;
}

void NativeDesktopBackend::stop(Completion completion)
{
    const auto owner = m_impl->state;
    dispatch_async(owner->queue, ^{ stopOnQueue(owner, completion); });
}
} // namespace dockpipe::desktop
