#include "NativePowerAssertion.h"

#import <CoreFoundation/CoreFoundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

namespace dockpipe::desktop {
namespace {
CFStringRef assertionType(PowerAssertionKind kind)
{
    return kind == PowerAssertionKind::System ? kIOPMAssertionTypePreventUserIdleSystemSleep
        : kIOPMAssertionTypePreventUserIdleDisplaySleep;
}

class IOKitPowerPlatform final : public NativePowerAssertion::Platform {
public:
    NativePowerAssertion::Creation create(PowerAssertionKind kind) override
    {
        IOPMAssertionID identifier = kIOPMNullAssertionID;
        const auto name = kind == PowerAssertionKind::System ? CFSTR("Dockpipe remote desktop system awake")
            : CFSTR("Dockpipe remote desktop display awake");
        const auto status = IOPMAssertionCreateWithName(assertionType(kind), kIOPMAssertionLevelOn, name, &identifier);
        return {status == kIOReturnSuccess, identifier};
    }
    bool verify(std::uint32_t identifier, PowerAssertionKind kind) override
    {
        CFDictionaryRef properties = IOPMAssertionCopyProperties(identifier);
        if (!properties)
            return false;
        if (CFGetTypeID(properties) != CFDictionaryGetTypeID()) {
            CFRelease(properties);
            return false;
        }
        const auto type = CFDictionaryGetValue(properties, kIOPMAssertionTypeKey);
        const auto level = CFDictionaryGetValue(properties, kIOPMAssertionLevelKey);
        int value = 0;
        const bool verified = type && CFGetTypeID(type) == CFStringGetTypeID()
            && CFEqual(type, assertionType(kind)) && level && CFGetTypeID(level) == CFNumberGetTypeID()
            && CFNumberGetValue(static_cast<CFNumberRef>(level), kCFNumberIntType, &value)
            && value == kIOPMAssertionLevelOn;
        CFRelease(properties);
        return verified;
    }
    bool release(std::uint32_t identifier) override
    {
        // We never retain an assertion. Apple's release success contract frees
        // and deactivates the exact ID when its single owned reference reaches zero.
        return IOPMAssertionRelease(identifier) == kIOReturnSuccess;
    }
};
} // namespace

NativePowerAssertion::NativePowerAssertion() : NativePowerAssertion(std::make_shared<IOKitPowerPlatform>())
{
}

} // namespace dockpipe::desktop
