#pragma once

#include "H264Contract.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace dockpipe::desktop {

enum class CaptureQuality { Low, Balanced, High, Ultra };
enum class DisplaySource { Existing, Virtual };

struct CaptureProfile {
    CaptureQuality quality = CaptureQuality::High;
    unsigned frameRate = 30;
    DisplaySource displaySource = DisplaySource::Existing;
    bool clipboardText = false;
    bool clipboardImages = false;
    bool clipboardFiles = false;
};

struct CapturePreset {
    unsigned maximumWidth;
    unsigned maximumHeight;
    unsigned bitsPerSecond;
};

struct CaptureGeometry {
    unsigned width;
    unsigned height;
};

inline bool validCaptureProfile(CaptureProfile profile)
{
    const bool quality = profile.quality == CaptureQuality::Low || profile.quality == CaptureQuality::Balanced
        || profile.quality == CaptureQuality::High || profile.quality == CaptureQuality::Ultra;
    const bool source = profile.displaySource == DisplaySource::Existing || profile.displaySource == DisplaySource::Virtual;
    return quality && source && (profile.frameRate == 10 || profile.frameRate == 15
        || profile.frameRate == 24 || profile.frameRate == 30 || profile.frameRate == 60);
}

inline std::optional<CapturePreset> capturePreset(CaptureProfile profile)
{
    if (!validCaptureProfile(profile))
        return std::nullopt;
    switch (profile.quality) {
    case CaptureQuality::Low:
        return CapturePreset{1280, 720, 1000000};
    case CaptureQuality::Balanced:
        return CapturePreset{1600, 900, 2500000};
    case CaptureQuality::High:
        return CapturePreset{1920, 1080, 4000000};
    case CaptureQuality::Ultra:
        return CapturePreset{3840, 2160, 16000000};
    }
    return std::nullopt;
}

inline std::optional<H264ProfileBounds> captureCodecBounds(CaptureProfile profile)
{
    const auto preset = capturePreset(profile);
    if (!preset)
        return std::nullopt;
    unsigned level = profile.frameRate == 60 ? 42 : 40;
    if (profile.quality == CaptureQuality::Ultra)
        level = profile.frameRate == 60 ? 52 : 51;
    return H264ProfileBounds{preset->maximumWidth, preset->maximumHeight, level};
}

inline std::optional<CaptureGeometry> captureGeometry(CaptureProfile profile, double displayWidth, double displayHeight)
{
    const auto preset = capturePreset(profile);
    if (!preset || !std::isfinite(displayWidth) || !std::isfinite(displayHeight) || displayWidth < 2 || displayHeight < 2)
        return std::nullopt;
    if (profile.displaySource == DisplaySource::Virtual) {
        if (displayWidth != preset->maximumWidth || displayHeight != preset->maximumHeight)
            return std::nullopt;
        return CaptureGeometry{preset->maximumWidth, preset->maximumHeight};
    }
    const double scale = std::min({1.0, preset->maximumWidth / displayWidth, preset->maximumHeight / displayHeight});
    const auto width = static_cast<unsigned>(displayWidth * scale) & ~1U;
    const auto height = static_cast<unsigned>(displayHeight * scale) & ~1U;
    if (width < 2 || height < 2)
        return std::nullopt;
    return CaptureGeometry{width, height};
}

} // namespace dockpipe::desktop
