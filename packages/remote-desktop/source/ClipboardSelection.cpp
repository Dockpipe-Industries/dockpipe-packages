#include "ClipboardSelection.h"
#include "../native/AssetClipboardProtocol.h"
#include "../native/ClipboardPng.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QPixmap>
#include <QUrl>
#include <QtEndian>
#include <stdexcept>
#include <cstring>

namespace dockpipe::desktop {
namespace {
void requireDimensions(const QSize &size)
{
    if (size.width() <= 0 || size.height() <= 0
        || quint64(size.width()) * quint64(size.height()) > AssetClipboardMaximumPixels)
        throw std::invalid_argument("Unsupported clipboard image dimensions.");
}


}

ClipboardSelectionKind clipboardSelectionKind(const QMimeData &mime)
{
    if (mime.hasUrls() || mime.hasFormat("x-special/gnome-copied-files"))
        return ClipboardSelectionKind::Files;
    if (mime.hasImage() || mime.hasFormat("image/png"))
        return ClipboardSelectionKind::Png;
    if (mime.hasText())
        return ClipboardSelectionKind::Text;
    return ClipboardSelectionKind::Unsupported;
}

QStringList clipboardLocalFiles(const QMimeData &mime)
{
    auto urls = mime.urls();
    if (urls.isEmpty() && mime.hasFormat("x-special/gnome-copied-files")) {
        const auto bytes = mime.data("x-special/gnome-copied-files");
        if (bytes.size() > qsizetype(AssetClipboardMaximumManifestBytes))
            throw std::invalid_argument("Clipboard file metadata exceeds its limit.");
        const auto lines = bytes.split('\n');
        if (lines.isEmpty() || (lines.front() != "copy" && lines.front() != "cut"))
            throw std::invalid_argument("Unsupported clipboard file metadata.");
        for (qsizetype index = 1; index < lines.size(); ++index) {
            if (!lines[index].isEmpty())
                urls.append(QUrl::fromEncoded(lines[index], QUrl::StrictMode));
        }
    }
    if (urls.isEmpty() || urls.size() > qsizetype(AssetClipboardMaximumFiles))
        throw std::invalid_argument("Unsupported clipboard file count.");
    QStringList paths;
    for (const auto &url : urls) {
        if (!url.isValid() || !url.isLocalFile() || !url.host().isEmpty() || !url.userInfo().isEmpty()
            || url.port() != -1 || url.hasQuery() || url.hasFragment())
            throw std::invalid_argument("Clipboard files must be local regular files.");
        const auto path = url.toLocalFile();
        if (path.isEmpty() || path.contains(QChar(0)) || QString::fromUtf8(path.toUtf8()) != path
            || !QFileInfo(path).isAbsolute())
            throw std::invalid_argument("Clipboard file path must be absolute.");
        paths.append(path);
    }
    return paths;
}

namespace {
struct DecodedPng {
    QByteArray bytes;
    QImage image;
};

DecodedPng validatedDecode(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > qsizetype(AssetClipboardMaximumPngBytes))
        throw std::invalid_argument("Clipboard PNG exceeds its limit.");
    const auto *begin = reinterpret_cast<const std::uint8_t *>(bytes.constData());
    const auto sanitized = sanitizeClipboardPng(std::vector<std::uint8_t>(begin, begin + bytes.size()));
    QByteArray safe(reinterpret_cast<const char *>(sanitized.bytes.data()), qsizetype(sanitized.bytes.size()));
    QBuffer input(&safe);
    input.open(QIODevice::ReadOnly);
    QImageReader reader(&input, "png");
    reader.setAutoTransform(false);
    requireDimensions(reader.size());
    auto image = reader.read();
    if (image.isNull())
        throw std::invalid_argument("Clipboard PNG could not be decoded.");
    requireDimensions(image.size());
    if (image.width() != int(sanitized.width) || image.height() != int(sanitized.height))
        throw std::invalid_argument("Clipboard PNG dimensions changed.");
    return {std::move(safe), std::move(image)};
}
}

ValidatedClipboardImage::ValidatedClipboardImage(QByteArray png, QImage image)
    : m_png(std::move(png)), m_image(std::move(image))
{
}

void ClipboardPngReuse::remember(const ValidatedClipboardImage &prepared)
{
    m_png = prepared.png();
    m_size = prepared.image().size();
}

std::optional<QSize> ClipboardPngReuse::consume(const QByteArray &bytes)
{
    const bool matches = !m_png.isEmpty() && m_png == bytes;
    const auto size = m_size;
    m_png.clear();
    m_size = {};
    if (matches)
        return size;
    return std::nullopt;
}

QImage decodeClipboardPng(const QByteArray &bytes)
{
    return validatedDecode(bytes).image;
}

ValidatedClipboardImage prepareClipboardImage(const QMimeData &mime)
{
    QImage image;
    QByteArray reusablePng;
    if (mime.hasFormat("image/png")) {
        auto decoded = validatedDecode(mime.data("image/png"));
        image = std::move(decoded.image);
        // The structural sanitizer and full decoder have both accepted these
        // bytes. Only canonical RGBA8 may retain its compressed pixel stream.
        if (quint8(decoded.bytes[24]) == 8 && quint8(decoded.bytes[25]) == 6)
            reusablePng = std::move(decoded.bytes);
    } else {
        const auto value = mime.imageData();
        if (value.canConvert<QImage>())
            image = qvariant_cast<QImage>(value);
        else if (value.canConvert<QPixmap>())
            image = qvariant_cast<QPixmap>(value).toImage();
        requireDimensions(image.size());
    }
    // A fresh RGBA8 surface discards image text and color-profile metadata.
    const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QImage clean(rgba.size(), QImage::Format_RGBA8888);
    if (rgba.isNull() || clean.isNull())
        throw std::runtime_error("Clipboard image allocation failed.");
    for (int row = 0; row < rgba.height(); ++row)
        std::memcpy(clean.scanLine(row), rgba.constScanLine(row), std::size_t(rgba.width()) * 4);
    if (!reusablePng.isEmpty())
        return ValidatedClipboardImage(std::move(reusablePng), std::move(clean));
    QByteArray bytes;
    QBuffer output(&bytes);
    output.open(QIODevice::WriteOnly);
    if (!clean.save(&output, "PNG") || bytes.size() > qsizetype(AssetClipboardMaximumPngBytes))
        throw std::invalid_argument("Clipboard PNG is too large.");
    return ValidatedClipboardImage(std::move(bytes), std::move(clean));
}

QByteArray encodeClipboardPng(const QMimeData &mime)
{
    return prepareClipboardImage(mime).png();
}

std::unique_ptr<QMimeData> clipboardFileCopy(const QStringList &privatePaths,
                                            const QByteArray &ownershipMarker)
{
    if (privatePaths.isEmpty())
        throw std::invalid_argument("Empty clipboard file snapshot.");
    QList<QUrl> urls;
    QByteArray copiedFiles("copy\n");
    for (const auto &path : privatePaths) {
        if (!QFileInfo(path).isAbsolute())
            throw std::invalid_argument("Clipboard snapshot path must be absolute.");
        const auto url = QUrl::fromLocalFile(path);
        urls.append(url);
        copiedFiles.append(url.toEncoded());
        copiedFiles.append('\n');
    }
    auto mime = std::make_unique<QMimeData>();
    mime->setUrls(urls);
    mime->setData("x-special/gnome-copied-files", copiedFiles);
    mime->setData(ClipboardOwnershipMime, ownershipMarker);
    return mime;
}

} // namespace dockpipe::desktop
