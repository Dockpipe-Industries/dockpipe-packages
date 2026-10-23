#include "NativeClipboard.h"
#include "ClipboardRequest.h"

#include <QCryptographicHash>
#include <QStringEncoder>
#include <cstring>
#include <cwchar>
#include <windows.h>

namespace {
class ClipboardLock final {
public:
    explicit ClipboardLock(HWND owner) : opened(OpenClipboard(owner)) {}
    ~ClipboardLock()
    {
        if (opened)
            CloseClipboard();
    }
    const bool opened;
};

NativeClipboard::ReadResult readUnlocked()
{
    using NativeClipboard::ReadResult;
    using NativeClipboard::ReadState;
    if (!CountClipboardFormats())
        return {ReadState::Text, {}};
    if (IsClipboardFormatAvailable(CF_HDROP) || !IsClipboardFormatAvailable(CF_UNICODETEXT))
        return {ReadState::Unsupported, {}};
    HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    const SIZE_T size = memory ? GlobalSize(memory) : 0;
    if (size < sizeof(wchar_t) || size > 2 * (ClipboardRequest::maximumTextBytes + 1))
        return {};
    const auto *characters = static_cast<const wchar_t *>(GlobalLock(memory));
    if (!characters)
        return {};
    const auto *end = static_cast<const wchar_t *>(std::wmemchr(characters, L'\0', size / sizeof(wchar_t)));
    QString text;
    if (end)
        text = QString::fromWCharArray(characters, end - characters);
    GlobalUnlock(memory);
    if (!end)
        return {};
    QStringEncoder encoder(QStringEncoder::Utf8);
    const QByteArray bytes = encoder(text);
    if (encoder.hasError() || bytes.size() > ClipboardRequest::maximumTextBytes)
        return {};
    return {ReadState::Text, text};
}
}

NativeClipboard::ReadResult NativeClipboard::readText(quintptr ownerWindow)
{
    ClipboardLock lock(reinterpret_cast<HWND>(ownerWindow));
    if (!lock.opened)
        return {};
    // RPC reads and conditional writes use this same representation. No Qt MIME
    // or platform newline conversion can change the expected identity.
    return readUnlocked();
}

NativeClipboard::WriteResult NativeClipboard::writeText(
    quintptr ownerWindow, const QString &text, const QString &expectedSha256)
{
    ClipboardLock lock(reinterpret_cast<HWND>(ownerWindow));
    if (!lock.opened)
        return WriteResult::Unavailable;
    if (!expectedSha256.isEmpty()) {
        const auto current = readUnlocked();
        if (current.state == ReadState::Unavailable)
            return WriteResult::Unavailable;
        const QString identity = current.state == ReadState::Unsupported
            ? QStringLiteral("unsupported-text")
            : QString::fromLatin1(QCryptographicHash::hash(current.text.toUtf8(), QCryptographicHash::Sha256).toHex());
        if (identity != expectedSha256)
            return WriteResult::Conflict;
    }
    const SIZE_T size = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory)
        return WriteResult::Unavailable;
    auto *characters = static_cast<wchar_t *>(GlobalLock(memory));
    if (!characters) {
        GlobalFree(memory);
        return WriteResult::Unavailable;
    }
    std::memcpy(characters, text.utf16(), text.size() * sizeof(wchar_t));
    characters[text.size()] = L'\0';
    GlobalUnlock(memory);
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        return WriteResult::Unavailable;
    }
    // Windows owns the movable allocation after SetClipboardData succeeds.
    return WriteResult::Success;
}
