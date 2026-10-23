#pragma once

#include <QJsonObject>
#include <QSize>
#include <QList>

namespace NativeDisplay {
QList<QSize> supportedResolutions();
QSize currentResolution();
QJsonObject describeResolutions();
QJsonObject setResolution(const QSize &resolution);
}
