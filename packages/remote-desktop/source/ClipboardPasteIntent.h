#pragma once

#include <QJsonObject>
#include <QList>
#include <QtGlobal>
#include <optional>

namespace dockpipe::desktop {

struct ClipboardPasteBinding {
    QJsonObject reference;
    qint64 generation = -1;
    quint64 epoch = 0;
    bool swapControlCommand = false;

    bool operator==(const ClipboardPasteBinding &) const = default;
};

struct ClipboardPasteChord {
    QList<quint16> modifiers;
    quint16 key = 0;

    bool operator==(const ClipboardPasteChord &) const = default;
};

// Records explicit viewer gestures only. The caller owns authority/focus checks,
// physical release tracking, and sending the already-mapped HID chord.
class ClipboardPasteIntent final {
public:
    bool request(ClipboardPasteBinding binding, ClipboardPasteChord chord, bool committed = false);
    void outgoingCommitted(const ClipboardPasteBinding &binding);
    std::optional<ClipboardPasteChord> take(const ClipboardPasteBinding &binding,
        bool eligible, bool physicalReleased);
    void cancel();
    bool pending() const;

private:
    struct Request {
        ClipboardPasteBinding binding;
        ClipboardPasteChord chord;
        bool committed = false;
    };

    std::optional<Request> m_request;
};

} // namespace dockpipe::desktop
