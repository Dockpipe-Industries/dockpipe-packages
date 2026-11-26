#pragma once

#include "NativeVirtualDisplay.h"
#include <cmath>
#include <cstddef>

namespace dockpipe::desktop {

struct NativeVirtualMode {
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t pixelWidth = 0;
    std::size_t pixelHeight = 0;
    double refreshRate = 0;
};

inline constexpr std::size_t MaximumVirtualDisplayModes = 64;

inline bool boundedVirtualModeCount(std::size_t count)
{
    return count > 0 && count <= MaximumVirtualDisplayModes;
}

inline bool exactVirtualMode(NativeVirtualMode mode, NativeVirtualDisplaySpec requested)
{
    return mode.width == requested.width && mode.height == requested.height
        && mode.pixelWidth == requested.width && mode.pixelHeight == requested.height
        && std::isfinite(mode.refreshRate) && std::fabs(mode.refreshRate - 60.0) <= 0.05;
}

} // namespace dockpipe::desktop
