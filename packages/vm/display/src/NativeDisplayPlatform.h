#pragma once
#include <QByteArray>
#include <QString>

namespace NativeDisplayPlatform {
bool processMatches(int pid, const QString &startToken);
bool privateDirectory(const QString &path);
bool readPrivateDescriptor(const QString &directory, QByteArray &bytes);
bool privateBusAddress(const QString &address);
bool protectDirectory(const QString &path);
}
