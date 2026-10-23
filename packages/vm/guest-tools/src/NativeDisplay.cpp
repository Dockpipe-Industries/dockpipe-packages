#include "NativeDisplay.h"

#include <windows.h>
#include <algorithm>
#include <QJsonArray>

namespace {
constexpr DWORD maximumModes = 4096;

bool usable(const DEVMODEW &mode)
{
    return mode.dmPelsWidth >= 640 && mode.dmPelsWidth <= 8192
        && mode.dmPelsHeight >= 480 && mode.dmPelsHeight <= 8192
        && mode.dmBitsPerPel >= 24;
}

QJsonObject failure(const QString &message)
{
    return {{"schema", 1}, {"success", false}, {"error", message}};
}
}

QList<QSize> NativeDisplay::supportedResolutions()
{
    QList<QSize> resolutions;
    for (DWORD index = 0; index < maximumModes; ++index) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(nullptr, index, &mode))
            break;
        const QSize size(int(mode.dmPelsWidth), int(mode.dmPelsHeight));
        if (usable(mode) && !resolutions.contains(size))
            resolutions.append(size);
    }
    std::sort(resolutions.begin(), resolutions.end(), [](const QSize &first, const QSize &second) {
        if (first.width() != second.width())
            return first.width() < second.width();
        return first.height() < second.height();
    });
    return resolutions;
}

QSize NativeDisplay::currentResolution()
{
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
        return {};
    return QSize(int(mode.dmPelsWidth), int(mode.dmPelsHeight));
}

QJsonObject NativeDisplay::describeResolutions()
{
    QJsonArray modes;
    for (const auto &size : supportedResolutions())
        modes.append(QJsonObject{{"width", size.width()}, {"height", size.height()}});
    const auto current = currentResolution();
    return {{"schema", 1}, {"success", !current.isEmpty()},
            {"width", current.width()}, {"height", current.height()}, {"modes", modes}};
}

QJsonObject NativeDisplay::setResolution(const QSize &resolution)
{
    if (resolution.width() < 640 || resolution.width() > 8192
        || resolution.height() < 480 || resolution.height() > 8192)
        return failure(QStringLiteral("Resolution must be between 640x480 and 8192x8192."));

    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current))
        return failure(QStringLiteral("The interactive display is unavailable."));

    for (DWORD index = 0; index < maximumModes; ++index) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(nullptr, index, &mode))
            break;
        if (!usable(mode) || mode.dmPelsWidth != DWORD(resolution.width())
            || mode.dmPelsHeight != DWORD(resolution.height()))
            continue;
        // Preserve the complete driver-enumerated DEVMODE. Windows documents
        // this as the supported input contract for ChangeDisplaySettingsEx.
        if (ChangeDisplaySettingsExW(nullptr, &mode, nullptr, CDS_TEST, nullptr) != DISP_CHANGE_SUCCESSFUL)
            continue;
        if (ChangeDisplaySettingsExW(nullptr, &mode, nullptr, CDS_UPDATEREGISTRY, nullptr) != DISP_CHANGE_SUCCESSFUL)
            return failure(QStringLiteral("Windows could not apply and save this resolution."));
        if (currentResolution() != resolution)
            return failure(QStringLiteral("Windows did not report the requested resolution."));
        return {{"schema", 1}, {"success", true}, {"width", resolution.width()},
                {"height", resolution.height()}};
    }
    return failure(QStringLiteral("This resolution is not offered by the guest display driver."));
}
