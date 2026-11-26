#include "NativeVirtualDisplay.h"
#include "CaptureProfile.h"
#include "NativeVirtualMode.h"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/runtime.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Original runtime adapter. These protocols describe only the selectors we
// invoke; no private class is linked or third-party implementation included.
@protocol DPVirtualDescriptor <NSObject>
- (void)setName:(NSString *)name;
- (void)setQueue:(id)queue;
- (void)setMaxPixelsWide:(unsigned int)value;
- (void)setMaxPixelsHigh:(unsigned int)value;
- (void)setVendorID:(unsigned int)value;
- (void)setProductID:(unsigned int)value;
- (void)setSerialNum:(unsigned int)value;
- (void)setSizeInMillimeters:(CGSize)value;
@end
@protocol DPVirtualMode <NSObject>
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)rate;
@end
@protocol DPVirtualSettings <NSObject>
- (void)setHiDPI:(unsigned int)value;
- (void)setModes:(NSArray *)modes;
@end
@protocol DPVirtualDisplay <NSObject>
- (instancetype)initWithDescriptor:(id)descriptor;
- (BOOL)applySettings:(id)settings;
- (unsigned int)displayID;
@end

namespace dockpipe::desktop {
namespace {
std::atomic_bool virtualOwnerInUse{false};

bool methodShape(Class type, SEL selector, const char *result, std::initializer_list<const char *> arguments)
{
    const Method method = class_getInstanceMethod(type, selector);
    if (!method)
        return false;
    NSMethodSignature *signature = [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(method)];
    if (!signature || std::strcmp(signature.methodReturnType, result) != 0
        || signature.numberOfArguments != arguments.size() + 2)
        return false;
    unsigned index = 2;
    for (const char *argument : arguments) {
        if (std::strcmp([signature getArgumentTypeAtIndex:index++], argument) != 0)
            return false;
    }
    return true;
}

bool runtimeAvailable()
{
    Class descriptor = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class mode = NSClassFromString(@"CGVirtualDisplayMode");
    Class settings = NSClassFromString(@"CGVirtualDisplaySettings");
    Class display = NSClassFromString(@"CGVirtualDisplay");
    if (!descriptor || !mode || !settings || !display)
        return false;
    for (Class type : {descriptor, settings}) {
        if (!methodShape(type, @selector(init), "@", {}))
            return false;
    }
    for (SEL selector : {@selector(setName:), @selector(setQueue:)}) {
        if (!methodShape(descriptor, selector, "v", {"@"}))
            return false;
    }
    for (SEL selector : {@selector(setMaxPixelsWide:), @selector(setMaxPixelsHigh:),
             @selector(setVendorID:), @selector(setProductID:), @selector(setSerialNum:)}) {
        if (!methodShape(descriptor, selector, "v", {"I"}))
            return false;
    }
    return methodShape(descriptor, @selector(setSizeInMillimeters:), "v", {@encode(CGSize)})
        && methodShape(mode, @selector(initWithWidth:height:refreshRate:), "@", {"I", "I", "d"})
        && methodShape(settings, @selector(setHiDPI:), "v", {"I"})
        && methodShape(settings, @selector(setModes:), "v", {"@"})
        && methodShape(display, @selector(initWithDescriptor:), "@", {"@"})
        && methodShape(display, @selector(applySettings:), @encode(BOOL), {"@"})
        && methodShape(display, @selector(displayID), "I", {});
}

bool validSpec(NativeVirtualDisplaySpec spec)
{
    for (auto quality : {CaptureQuality::Low, CaptureQuality::Balanced, CaptureQuality::High, CaptureQuality::Ultra}) {
        const auto preset = capturePreset({quality, 30, DisplaySource::Virtual});
        if (preset && spec.width == preset->maximumWidth && spec.height == preset->maximumHeight)
            return true;
    }
    return false;
}

bool onlineDisplays(std::vector<CGDirectDisplayID> &ids)
{
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(0, nullptr, &count) != kCGErrorSuccess || count > 64)
        return false;
    CGDirectDisplayID values[64];
    uint32_t actual = 0;
    if (CGGetOnlineDisplayList(64, values, &actual) != kCGErrorSuccess || actual > 64)
        return false;
    uint32_t after = 0;
    if (CGGetOnlineDisplayList(0, nullptr, &after) != kCGErrorSuccess || after > 64 || after != actual || count != actual)
        return false;
    ids.assign(values, values + actual);
    return true;
}

struct Connection {
    std::size_t width;
    std::size_t height;
    CGDirectDisplayID mirror;
    bool mirrored;
};

class RuntimePlatform final : public NativeVirtualDisplay::Platform {
public:
    bool available() override
    {
        return runtimeAvailable();
    }
    bool ownerAlive() override
    {
        return ownsDisplay(m_createdDisplay);
    }
    NativeVirtualDisplay::Creation create(NativeVirtualDisplaySpec spec) override;
    NativeVirtualDisplay::Observation observe(std::uint32_t display, NativeVirtualDisplaySpec spec) override;
    NativeVirtualDisplay::Observation configureAndObserve(std::uint32_t display, NativeVirtualDisplaySpec spec,
        const NativeVirtualDisplay::SelectionAdmission &admission) override;
    void release() override
    {
        @autoreleasepool {
            m_display = nil;
            m_settings = nil;
            m_mode = nil;
            m_descriptor = nil;
        }
    }
private:
    bool ownsDisplay(std::uint32_t display) const;
    bool preservesConnections(const std::vector<CGDirectDisplayID> &ids) const;
    id<DPVirtualDescriptor> __strong m_descriptor = nil;
    id<DPVirtualMode> __strong m_mode = nil;
    id<DPVirtualSettings> __strong m_settings = nil;
    id<DPVirtualDisplay> __strong m_display = nil;
    std::vector<CGDirectDisplayID> m_before;
    std::map<CGDirectDisplayID, Connection> m_connections;
    CGDirectDisplayID m_protectedMain = 0;
    bool m_configured = false;
    CGDirectDisplayID m_createdDisplay = 0;
    unsigned m_serial = 0;
    bool m_modeSetAttempted = false;
    bool m_modeSetSucceeded = false;
};

NativeVirtualDisplay::Creation RuntimePlatform::create(NativeVirtualDisplaySpec spec)
{
    if (!onlineDisplays(m_before))
        return {};
    // Preserve every existing connection except the OS's recognized headless
    // fallback ('virt'). Removing that fallback is WindowServer policy; its ID
    // is not promised to survive adding/removing a real display object.
    for (auto id : m_before) {
        if (CGDisplayModelNumber(id) == 0x76697274 && CGDisplayVendorNumber(id) == 0x756e6b6e)
            continue;
        m_connections.emplace(id, Connection{CGDisplayPixelsWide(id), CGDisplayPixelsHigh(id),
            CGDisplayMirrorsDisplay(id), bool(CGDisplayIsInMirrorSet(id))});
        if (id == CGMainDisplayID())
            m_protectedMain = id;
    }
    Class descriptor = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class mode = NSClassFromString(@"CGVirtualDisplayMode");
    Class settings = NSClassFromString(@"CGVirtualDisplaySettings");
    Class display = NSClassFromString(@"CGVirtualDisplay");
    @autoreleasepool {
        m_descriptor = [[descriptor alloc] init];
        m_settings = [[settings alloc] init];
        m_mode = [(id<DPVirtualMode>)[mode alloc] initWithWidth:spec.width height:spec.height refreshRate:60.0];
        if (!m_descriptor || !m_settings || !m_mode)
            return {};
        [m_descriptor setName:@"Dockpipe Virtual Desktop"];
        [m_descriptor setQueue:dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0)];
        [m_descriptor setMaxPixelsWide:spec.width];
        [m_descriptor setMaxPixelsHigh:spec.height];
        [m_descriptor setSizeInMillimeters:CGSizeMake(spec.width * 25.4 / 96.0, spec.height * 25.4 / 96.0)];
        [m_descriptor setVendorID:0x4450];
        [m_descriptor setProductID:0x0001];
        unsigned serial = spec.serial ? spec.serial : arc4random();
        if (!serial)
            serial = 1;
        m_serial = serial;
        [m_descriptor setSerialNum:serial];
        [m_settings setHiDPI:0];
        [m_settings setModes:@[m_mode]];
        // Ordinary init-family messaging preserves ARC's retained/consumed
        // semantics. Never replace this with an unannotated objc_msgSend cast.
        m_display = [(id<DPVirtualDisplay>)[display alloc] initWithDescriptor:m_descriptor];
        const auto id = m_display ? [m_display displayID] : 0;
        m_createdDisplay = id;
        m_configured = m_display && id && std::find(m_before.begin(), m_before.end(), id) == m_before.end()
            && [m_display applySettings:m_settings];
        return {true, id};
    }
}

bool RuntimePlatform::ownsDisplay(std::uint32_t display) const
{
    return m_configured && display && display == m_createdDisplay && m_display && [m_display displayID] == display
        && std::find(m_before.begin(), m_before.end(), display) == m_before.end()
        && CGDisplayVendorNumber(display) == 0x4450 && CGDisplayModelNumber(display) == 1
        && CGDisplaySerialNumber(display) == m_serial;
}

bool RuntimePlatform::preservesConnections(const std::vector<CGDirectDisplayID> &ids) const
{
    if (m_protectedMain && CGMainDisplayID() != m_protectedMain)
        return false;
    for (const auto &[existing, before] : m_connections) {
        if (std::find(ids.begin(), ids.end(), existing) == ids.end()
            || CGDisplayPixelsWide(existing) != before.width || CGDisplayPixelsHigh(existing) != before.height
            || CGDisplayMirrorsDisplay(existing) != before.mirror || bool(CGDisplayIsInMirrorSet(existing)) != before.mirrored)
            return false;
    }
    return true;
}

NativeVirtualMode modeDescription(CGDisplayModeRef mode)
{
    return {CGDisplayModeGetWidth(mode), CGDisplayModeGetHeight(mode), CGDisplayModeGetPixelWidth(mode),
        CGDisplayModeGetPixelHeight(mode), CGDisplayModeGetRefreshRate(mode)};
}

NativeVirtualDisplay::Observation RuntimePlatform::observe(std::uint32_t id, NativeVirtualDisplaySpec spec)
{
    if (!id)
        return {};
    std::vector<CGDirectDisplayID> ids;
    if (!onlineDisplays(ids))
        return {};
    if (std::find(ids.begin(), ids.end(), id) == ids.end())
        return {true, false, false};
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(id);
    const bool mode = current && exactVirtualMode(modeDescription(current), spec);
    if (current)
        CFRelease(current);
    const CGRect bounds = CGDisplayBounds(id);
    const bool failedSelection = m_modeSetAttempted && !m_modeSetSucceeded;
    return {true, true, !failedSelection && ownsDisplay(id) && preservesConnections(ids) && mode && CGDisplayIsActive(id)
        && !CGDisplayIsInMirrorSet(id) && CGDisplayMirrorsDisplay(id) == kCGNullDirectDisplay
        && CGDisplayPixelsWide(id) == spec.width && CGDisplayPixelsHigh(id) == spec.height
        && bounds.size.width == spec.width && bounds.size.height == spec.height};
}

NativeVirtualDisplay::Observation RuntimePlatform::configureAndObserve(std::uint32_t id, NativeVirtualDisplaySpec spec,
    const NativeVirtualDisplay::SelectionAdmission &admission)
{
    const auto current = observe(id, spec);
    if (!current.querySucceeded || !current.online || current.matches || m_modeSetAttempted || !admission)
        return current;
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, nullptr);
    if (!modes)
        return current;
    const std::unique_ptr<const void, decltype(&CFRelease)> retainedModes(modes, &CFRelease);
    if (CFGetTypeID(modes) != CFArrayGetTypeID())
        return current;
    const CFIndex count = CFArrayGetCount(modes);
    CGDisplayModeRef selected = nullptr;
    bool valid = count >= 0 && boundedVirtualModeCount(static_cast<std::size_t>(count));
    if (valid) {
        for (CFIndex index = 0; index < count; ++index) {
            const auto value = CFArrayGetValueAtIndex(modes, index);
            if (!value || CFGetTypeID(value) != CGDisplayModeGetTypeID()) {
                valid = false;
                break;
            }
            const auto candidate = static_cast<CGDisplayModeRef>(const_cast<void *>(value));
            if (!selected && exactVirtualMode(modeDescription(candidate), spec))
                selected = candidate;
        }
    }
    const auto latest = observe(id, spec);
    if (!latest.querySucceeded || !latest.online || latest.matches)
        return latest;
    // The candidate remains borrowed from this retained array until the
    // synchronous setter returns. No mode from another display is accepted.
    std::vector<CGDirectDisplayID> ids;
    if (valid && selected && onlineDisplays(ids)
        && std::find(ids.begin(), ids.end(), id) != ids.end() && CGDisplayIsActive(id)
        && ownsDisplay(id) && preservesConnections(ids) && !CGDisplayIsInMirrorSet(id)
        && CGDisplayMirrorsDisplay(id) == kCGNullDirectDisplay && admission()) {
        m_modeSetAttempted = true; // A failed or partial setter is never retried.
        m_modeSetSucceeded = CGDisplaySetDisplayMode(id, selected, nullptr) == kCGErrorSuccess;
    }
    // Both successful and partial effects require fresh strict readback. A
    // nonzero setter result remains failed even if a later query looks exact.
    return observe(id, spec);
}
} // namespace

struct NativeVirtualDisplay::State {
    std::shared_ptr<Platform> platform;
    dispatch_queue_t queue = dispatch_queue_create("dockpipe.desktop.virtual-display", DISPATCH_QUEUE_SERIAL);
    NativeVirtualDisplaySpec spec;
    std::atomic_bool used{false};
    std::atomic_bool stopping{false};
    std::atomic_bool effectsPossible{false};
    std::atomic_uint display{0};
    std::atomic_bool closed{false};
    std::atomic_bool slotOwned{false};
    std::mutex mutex;
    Completion creating;
    Completion closing;
    bool createDelivered = false;
    bool closeDelivered = false;
    Clock::time_point createDeadline;

    static std::mutex &retentionMutex()
    {
        static std::mutex mutex;
        return mutex;
    }
    static std::shared_ptr<State> &retainedUnknown()
    {
        static auto *owner = new std::shared_ptr<State>;
        return *owner;
    }
    static void retainUnknown(const std::shared_ptr<State> &state)
    {
        std::lock_guard lock(retentionMutex());
        if (!state->closed)
            retainedUnknown() = state;
    }
    void verifiedClosed()
    {
        std::lock_guard lock(retentionMutex());
        closed = true;
        if (retainedUnknown().get() == this)
            retainedUnknown().reset();
        if (slotOwned.exchange(false))
            virtualOwnerInUse = false;
    }

    void completeVerifiedClose(Clock::time_point deadline, std::uint32_t id)
    {
        Completion completion;
        {
            std::lock_guard lock(mutex);
            if (closeDelivered || (effectsPossible && Clock::now() >= deadline))
                return;
            closeDelivered = true;
            verifiedClosed();
            completion = std::move(closing);
        }
        if (completion)
            completion({true, true, id, {}});
    }

    void deliver(bool close, NativeVirtualDisplayResult result)
    {
        Completion completion;
        {
            std::lock_guard lock(mutex);
            auto &delivered = close ? closeDelivered : createDelivered;
            if (delivered)
                return;
            if (!close && result.success && (stopping || Clock::now() >= createDeadline))
                result = {false, false, display, "Virtual display creation was cancelled before admission."};
            delivered = true;
            completion = std::move(close ? closing : creating);
        }
        if (completion)
            completion(std::move(result));
    }
};

NativeVirtualDisplay::NativeVirtualDisplay() : NativeVirtualDisplay(std::make_shared<RuntimePlatform>())
{
}
NativeVirtualDisplay::NativeVirtualDisplay(std::shared_ptr<Platform> platform) : m_state(std::make_shared<State>())
{
    m_state->platform = std::move(platform);
}
NativeVirtualDisplay::~NativeVirtualDisplay()
{
    // At most one admitted owner exists per process. Keep unknown state alive
    // without destroying its display while capture/input might still use it;
    // a later verified asynchronous close clears this retained owner.
    if (m_state->used && !m_state->closed)
        State::retainUnknown(m_state);
}

bool NativeVirtualDisplay::ownerAlive() const
{
    try {
        std::lock_guard lock(m_state->mutex);
        return m_state->display != 0 && !m_state->stopping && !m_state->closed
            && m_state->platform && m_state->platform->ownerAlive();
    } catch (...) {
        return false;
    }
}

bool NativeVirtualDisplay::available()
{
    return runtimeAvailable();
}

void NativeVirtualDisplay::create(NativeVirtualDisplaySpec spec, Clock::time_point deadline, Completion completion)
{
    const auto state = m_state;
    NativeVirtualDisplayResult rejected;
    bool accepted = false;
    {
        std::lock_guard lock(state->mutex);
        if (!validSpec(spec)) {
            rejected = {false, !state->used || state->closed, state->display, "Unsupported virtual display request."};
        } else if (state->used.exchange(true)) {
            rejected = {false, state->closed, state->display, "Virtual display request was reused."};
        } else if (state->stopping) {
            state->verifiedClosed();
            rejected = {false, true, 0, "Virtual display creation was cancelled."};
        } else if (virtualOwnerInUse.exchange(true)) {
            state->verifiedClosed();
            rejected = {false, true, 0, "This host process already owns a virtual display."};
        } else {
            state->slotOwned = true;
            state->spec = spec;
            deadline = std::min(deadline, Clock::now() + std::chrono::seconds(2));
            state->creating = std::move(completion);
            state->createDeadline = deadline;
            accepted = true;
        }
    }
    if (!accepted) {
        completion(std::move(rejected));
        return;
    }
    const auto delay = std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count());
    const std::weak_ptr<State> weakState = state;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (const auto owner = weakState.lock())
            owner->deliver(false, {false, false, owner->display, "Virtual display creation was not verified before its deadline."});
    });
    dispatch_async(state->queue, ^{
        try {
            @autoreleasepool {
                @try {
                    if (state->stopping || Clock::now() >= deadline) {
                        state->verifiedClosed();
                        state->deliver(false, {false, true, 0, "Virtual display creation was cancelled."});
                        return;
                    }
                    if (!state->platform || !state->platform->available()) {
                        state->verifiedClosed();
                        state->deliver(false, {false, true, 0, "Native virtual displays are unavailable on this host."});
                        return;
                    }
                    state->effectsPossible = true;
                    const auto created = state->platform->createWithAdmission(spec, deadline, [state, deadline] {
                        std::lock_guard lock(state->mutex);
                        return !state->stopping && !state->createDelivered && Clock::now() < deadline;
                    });
                    state->display = created.display;
                    state->effectsPossible = created.effectsPossible;
                    if (!created.effectsPossible) {
                        state->verifiedClosed();
                        state->deliver(false, {false, true, 0, "Virtual display creation failed before registration."});
                        return;
                    }
                    while (!state->stopping && Clock::now() < deadline && created.display) {
                        const auto observation = state->platform->configureAndObserve(created.display, spec, [state, deadline] {
                            std::lock_guard lock(state->mutex);
                            return !state->stopping && !state->createDelivered && Clock::now() < deadline;
                        });
                        if (observation.querySucceeded && observation.online && observation.matches) {
                            state->deliver(false, {true, false, created.display, {}});
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    state->deliver(false, {false, false, created.display, "The owned virtual display did not satisfy its exact mode."});
                } @catch (NSException *) {
                    state->deliver(false, {false, false, state->display, "Native virtual display creation failed."});
                }
            }
        } catch (...) {
            state->deliver(false, {false, false, state->display, "Unexpected virtual display creation failure."});
        }
    });
}

void NativeVirtualDisplay::close(Clock::time_point deadline, Completion completion)
{
    const auto state = m_state;
    bool duplicate = false;
    {
        std::lock_guard lock(state->mutex);
        state->stopping = true;
        duplicate = bool(state->closing) || state->closeDelivered;
        if (!duplicate)
            state->closing = std::move(completion);
    }
    if (duplicate) {
        completion({bool(state->closed), bool(state->closed), state->display, "Virtual display cleanup was already requested."});
        return;
    }
    const auto delay = std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count());
    const std::weak_ptr<State> weakState = state;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (const auto owner = weakState.lock())
            owner->deliver(true, {false, false, owner->display, "Owned virtual display removal remains unconfirmed."});
    });
    dispatch_async(state->queue, ^{
        try {
            if (!state->effectsPossible) {
                state->completeVerifiedClose(deadline, 0);
                return;
            }
            @autoreleasepool {
                @try {
                    state->platform->releaseBefore(deadline);
                } @catch (NSException *) {
                    state->deliver(true, {false, false, state->display, "Native virtual display release failed."});
                    return;
                }
            } // Drain all temporary strong/autoreleased references before observation.
            unsigned absent = 0;
            while (Clock::now() < deadline) {
                const auto observation = state->platform->observe(state->display, state->spec);
                absent = observation.querySucceeded && !observation.online ? absent + 1 : 0;
                if (absent >= 2 && Clock::now() < deadline) {
                    state->completeVerifiedClose(deadline, state->display);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            state->deliver(true, {false, false, state->display, "Owned virtual display ID remains online or unobservable."});
        } catch (...) {
            state->deliver(true, {false, false, state->display, "Unexpected virtual display cleanup failure."});
        }
    });
}

} // namespace dockpipe::desktop
