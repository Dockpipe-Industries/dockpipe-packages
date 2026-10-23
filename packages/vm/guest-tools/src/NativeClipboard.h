#pragma once

#include <QString>
#include <QtGlobal>

namespace NativeClipboard {
enum class ReadState { Text, Unsupported, Unavailable };
struct ReadResult {
    ReadState state = ReadState::Unavailable;
    QString text;
};
enum class WriteResult { Success, Conflict, Unavailable };
ReadResult readText(quintptr ownerWindow);
WriteResult writeText(quintptr ownerWindow, const QString &text, const QString &expectedSha256);
}
