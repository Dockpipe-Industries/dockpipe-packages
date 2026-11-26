#pragma once

#include "../native/H264Contract.h"
#include <QJsonObject>
#include <QString>
#include <stdexcept>

namespace dockpipe::desktop {

struct ViewerCapturePreferences {
    QString quality = "high";
    int frameRate = 30;
    bool virtualDisplay = false;
    bool clipboardText = false;
    bool clipboardImages = false;
    bool clipboardFiles = false;

    static ViewerCapturePreferences parse(const QJsonObject &value)
    {
        const auto rate = value.value("frame_rate");
        const auto quality = value.value("quality");
        const bool hasSource = value.contains("display_source");
        const bool hasClipboard = value.contains("clipboard_text");
        const bool hasImages = value.contains("clipboard_images");
        const bool hasFiles = value.contains("clipboard_files");
        if (value.size() != 2 + int(hasSource) + int(hasClipboard) + int(hasImages) + int(hasFiles)
            || (hasImages && value.value("clipboard_images") != QJsonValue(true))
            || (hasFiles && value.value("clipboard_files") != QJsonValue(true))
            || (hasClipboard && value.value("clipboard_text") != QJsonValue(true))
            || (hasSource && value.value("display_source") != "virtual")
            || !quality.isString() || !rate.isDouble()
            || rate.toDouble() != rate.toInteger(-1)
            || (quality != "low" && quality != "balanced" && quality != "high" && quality != "ultra")
            || (rate.toInteger() != 10 && rate.toInteger() != 15 && rate.toInteger() != 24 && rate.toInteger() != 30 && rate.toInteger() != 60))
            throw std::invalid_argument("Unsupported native capture preferences.");
        return {quality.toString(), rate.toInt(), hasSource, hasClipboard, hasImages, hasFiles};
    }
    QJsonObject json() const
    {
        QJsonObject value{{"quality", quality}, {"frame_rate", frameRate}};
        if (virtualDisplay)
            value.insert("display_source", "virtual");
        if (clipboardText)
            value.insert("clipboard_text", true);
        if (clipboardImages)
            value.insert("clipboard_images", true);
        if (clipboardFiles)
            value.insert("clipboard_files", true);
        return value;
    }
    int maximumWidth() const
    {
        if (quality == "low")
            return 1280;
        if (quality == "balanced")
            return 1600;
        if (quality == "ultra")
            return 3840;
        return 1920;
    }
    int maximumHeight() const
    {
        if (quality == "low")
            return 720;
        if (quality == "balanced")
            return 900;
        if (quality == "ultra")
            return 2160;
        return 1080;
    }
    unsigned maximumLevel() const
    {
        if (quality == "ultra")
            return frameRate == 60 ? 52 : 51;
        return frameRate == 60 ? 42 : 40;
    }
    bool acceptsFrameGeometry(unsigned width, unsigned height) const
    {
        if (virtualDisplay)
            return width == static_cast<unsigned>(maximumWidth()) && height == static_cast<unsigned>(maximumHeight());
        return width > 0 && height > 0 && width <= static_cast<unsigned>(maximumWidth())
            && height <= static_cast<unsigned>(maximumHeight());
    }
    H264ProfileBounds bounds() const
    {
        return {static_cast<unsigned>(maximumWidth()), static_cast<unsigned>(maximumHeight()), maximumLevel()};
    }

};

struct ViewerPresentationPreferences {
    QString scale = "fit";
    bool hideCursor = true;
    bool swapControlCommand = false;

    static ViewerPresentationPreferences parse(const QJsonObject &value)
    {
        const auto scale = value.value("scale");
        const auto hide = value.value("hide_cursor");
        const bool hasSwap = value.contains("swap_control_command");
        const auto swap = value.value("swap_control_command");
        if (value.size() != 2 + int(hasSwap) || (scale != "fit" && scale != "actual") || !hide.isBool()
            || (hasSwap && !swap.isBool()))
            throw std::invalid_argument("Unsupported native display preferences.");
        return {scale.toString(), hide.toBool(), swap.toBool()};
    }
    QJsonObject json() const
    {
        QJsonObject value{{"scale", scale}, {"hide_cursor", hideCursor}};
        if (swapControlCommand)
            value.insert("swap_control_command", true);
        return value;
    }
};

} // namespace dockpipe::desktop
