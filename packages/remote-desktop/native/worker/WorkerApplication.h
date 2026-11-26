#pragma once

#import <AppKit/AppKit.h>
#include <stdexcept>

namespace dockpipe::desktop {
inline void prepareBackgroundWorkerApplication()
{
    if (![NSThread isMainThread])
        throw std::runtime_error("Background worker application requires the main thread.");

    auto *application = [NSApplication sharedApplication];
    if (![application setActivationPolicy:NSApplicationActivationPolicyAccessory])
        throw std::runtime_error("Cannot configure the background worker application.");

    // Complete the AppKit launch handshake without a Dock icon, application
    // menu, window or activation. Permission onboarding uses its separate mode.
    [application finishLaunching];
    if (application.activationPolicy != NSApplicationActivationPolicyAccessory)
        throw std::runtime_error("Background worker activation policy changed during launch.");
}
} // namespace dockpipe::desktop
