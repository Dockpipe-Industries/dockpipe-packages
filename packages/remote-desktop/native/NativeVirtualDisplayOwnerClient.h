#pragma once

#include "NativeVirtualDisplayOwner.h"
#include "NativeVirtualMode.h"
#include <vector>

namespace dockpipe::desktop {
struct VirtualDisplayOwnerDisplay {
    std::uint32_t id = 0;
    std::uint32_t vendor = 0;
    std::uint32_t product = 0;
    std::uint32_t serial = 0;
    std::size_t width = 0;
    std::size_t height = 0;
    double boundsWidth = 0;
    double boundsHeight = 0;
    std::uint32_t mirror = 0;
    bool mirrored = false;
    bool main = false;
    bool active = false;
    bool modeKnown = false;
    NativeVirtualMode mode;
};
struct VirtualDisplayOwnerInventory {
    bool complete = false;
    std::vector<VirtualDisplayOwnerDisplay> displays;
};
using VirtualDisplayOwnerInventoryProvider = std::function<VirtualDisplayOwnerInventory(bool includeModes)>;
using VirtualDisplayOwnerIdentityProbe = std::function<bool(std::uint32_t, std::uint32_t)>;

// Explicit OS boundary for portable process fixtures; production supplies only
// the package's public CoreGraphics inventory/identity implementations.
std::shared_ptr<NativeVirtualDisplay::Platform> makeVirtualDisplayOwnerPlatformForSystem(
    VirtualDisplayOwnerContext context, VirtualDisplayOwnerInventoryProvider inventory,
    VirtualDisplayOwnerIdentityProbe identity);
} // namespace dockpipe::desktop
