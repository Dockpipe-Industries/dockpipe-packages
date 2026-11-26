#pragma once

#include "../native/H264Contract.h"
#include <cstdint>

namespace dockpipe::desktop {

// FFmpeg requests the SPS coded picture before applying visible crop. This is
// macroblock padding, distinct from visible image and GPU pool permissions.
inline bool permitsCodedAllocation(int width, int height, H264ProfileBounds bounds)
{
    if (width < 1 || height < 1 || bounds.maximumWidth < 2 || bounds.maximumHeight < 2
        || bounds.maximumWidth > 3840 || bounds.maximumHeight > 2160)
        return false;
    const auto maximumWidth = (std::uint64_t(bounds.maximumWidth) + 15) / 16 * 16;
    const auto maximumHeight = (std::uint64_t(bounds.maximumHeight) + 15) / 16 * 16;
    return std::uint64_t(width) <= maximumWidth && std::uint64_t(height) <= maximumHeight
        && std::uint64_t(width) * std::uint64_t(height) <= maximumWidth * maximumHeight;
}

} // namespace dockpipe::desktop
