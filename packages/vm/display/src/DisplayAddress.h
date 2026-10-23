#pragma once
#include <QString>

// Decode D-Bus percent escaping before native filesystem admission. Only one
// explicit local Unix pathname transport is accepted, never TCP or discovery.
bool decodeDisplayBusPath(const QString &address, QString &path);
