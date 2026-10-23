#pragma once
#include <QString>
#include <QtGlobal>

// Maps physical keys, independently of guest layout and host text/IME output.
// Zero means unsupported; Cocoa virtual key zero is the physical A key.
int qemuKeyNumber(const QString &platform, quint32 scanCode, quint32 virtualKey);
