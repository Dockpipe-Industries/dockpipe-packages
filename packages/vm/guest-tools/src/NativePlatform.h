#pragma once

#include "ApplicationOptions.h"
#include <QDateTime>
#include <QString>

namespace NativePlatform {
bool isAdministrator();
QString interactiveUserName();
QString currentUserSid();
QDateTime bootTime();
bool elevate(const ApplicationOptions &options, QString *error);
QString statusPath();
bool applicationInstalled();
QString findRuntimeHelper();
}
