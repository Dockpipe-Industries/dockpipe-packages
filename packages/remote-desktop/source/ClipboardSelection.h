#pragma once

#include <QByteArray>
#include <QImage>
#include <QMimeData>
#include <QStringList>
#include <memory>
#include <optional>

namespace dockpipe::desktop {

enum class ClipboardSelectionKind { Files, Png, Text, Unsupported };

// Classification never falls back to text when a richer representation exists.
ClipboardSelectionKind clipboardSelectionKind(const QMimeData &mime);
QStringList clipboardLocalFiles(const QMimeData &mime);
class ValidatedClipboardImage final {
public:
    const QByteArray &png() const { return m_png; }
    const QImage &image() const { return m_image; }
private:
    ValidatedClipboardImage(QByteArray png, QImage image);
    QByteArray m_png;
    QImage m_image;
    friend ValidatedClipboardImage prepareClipboardImage(const QMimeData &mime);
};

// One serial-I/O operation only. A miss also retires the previous result.
class ClipboardPngReuse final {
public:
    void remember(const ValidatedClipboardImage &prepared);
    std::optional<QSize> consume(const QByteArray &bytes);
private:
    QByteArray m_png;
    QSize m_size;
};

ValidatedClipboardImage prepareClipboardImage(const QMimeData &mime);
QByteArray encodeClipboardPng(const QMimeData &mime);
QImage decodeClipboardPng(const QByteArray &bytes);
std::unique_ptr<QMimeData> clipboardFileCopy(const QStringList &privatePaths,
                                            const QByteArray &ownershipMarker);
constexpr auto ClipboardOwnershipMime = "application/x-dockpipe-owned-clipboard";

} // namespace dockpipe::desktop
