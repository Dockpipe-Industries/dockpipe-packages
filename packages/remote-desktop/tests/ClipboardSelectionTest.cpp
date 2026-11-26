#include "ClipboardSelection.h"
#include "ViewerPreferences.h"
#include "../native/ClipboardPng.h"
#include <QtEndian>
#include <QtTest>
#include <QBuffer>
#include <QTemporaryDir>

using namespace dockpipe::desktop;

class ClipboardSelectionTest final : public QObject {
    Q_OBJECT
private slots:
    void richRepresentationsNeverFallBackToText()
    {
        QMimeData mime;
        mime.setText("must not travel as text");
        mime.setImageData(QImage(2, 2, QImage::Format_ARGB32));
        QCOMPARE(clipboardSelectionKind(mime), ClipboardSelectionKind::Png);
        mime.setUrls({QUrl::fromLocalFile("/private/source-only")});
        QCOMPARE(clipboardSelectionKind(mime), ClipboardSelectionKind::Files);
    }
    void gnomeFileSelectionNeverFallsBackToItsPlainTextPaths()
    {
        QMimeData mime;
        mime.setText("file:///tmp/private-source");
        mime.setData("x-special/gnome-copied-files", "cut\nfile:///tmp/private-source\n");
        QCOMPARE(clipboardSelectionKind(mime), ClipboardSelectionKind::Files);
        QCOMPARE(clipboardLocalFiles(mime), QStringList{"/tmp/private-source"});
        const auto copy = clipboardFileCopy({"/tmp/private-snapshot/received"}, "owned");
        QVERIFY(copy->data("x-special/gnome-copied-files").startsWith("copy\n"));
    }
    void rejectsNonlocalFileUrls()
    {
        for (const auto &url : {QUrl("https://example.invalid/a"), QUrl("file://server/a"), QUrl("file:///tmp/a?query")}) {
            QMimeData mime;
            mime.setUrls({url});
            QVERIFY_EXCEPTION_THROWN(clipboardLocalFiles(mime), std::invalid_argument);
        }
    }
    void receivedFilesAreCopyOnlyLocalSnapshots()
    {
        const QStringList paths{"/tmp/private-owned/1/first.txt", "/tmp/private-owned/2/second.png"};
        auto mime = clipboardFileCopy(paths, "opaque-owned-id");
        QCOMPARE(mime->urls().size(), 2);
        QCOMPARE(mime->urls()[0].toLocalFile(), paths[0]);
        QCOMPARE(mime->data(ClipboardOwnershipMime), QByteArray("opaque-owned-id"));
        const auto copy = mime->data("x-special/gnome-copied-files");
        QVERIFY(copy.startsWith("copy\nfile:///tmp/private-owned/"));
        QVERIFY(!copy.contains("cut"));
        QVERIFY(!copy.contains("remote-source"));
        QVERIFY(!mime->hasFormat("text/plain"));
    }
    void captureBitsAreTrueOnlyAndDefaultOff()
    {
        const QJsonObject base{{"quality", "high"}, {"frame_rate", 30}};
        const auto defaults = ViewerCapturePreferences::parse(base);
        QVERIFY(!defaults.clipboardImages && !defaults.clipboardFiles);
        QCOMPARE(defaults.json(), base);
        for (const auto &field : {QString("clipboard_images"), QString("clipboard_files")}) {
            auto value = base;
            value.insert(field, true);
            QCOMPARE(ViewerCapturePreferences::parse(value).json(), value);
            for (const QJsonValue invalid : {QJsonValue(false), QJsonValue(1), QJsonValue("true"), QJsonValue()}) {
                value.insert(field, invalid);
                QVERIFY_EXCEPTION_THROWN(ViewerCapturePreferences::parse(value), std::invalid_argument);
            }
        }
    }
    void compressedMetadataIsRemovedBeforeDecode()
    {
        QImage image(2, 3, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        image.setText("private-keyword", "private metadata");
        QMimeData mime;
        mime.setImageData(image);
        const auto png = encodeClipboardPng(mime);
        QVERIFY(!png.contains("private-keyword"));
        const auto compressed = qCompress(QByteArray(2 * 1024 * 1024, 'A')).mid(4);
        QByteArray body("private-keyword\0\0", 17);
        body.append(compressed);
        QByteArray chunk(4, Qt::Uninitialized);
        qToBigEndian<quint32>(quint32(body.size()), chunk.data());
        chunk.append("zTXt");
        chunk.append(body);
        const auto crc = clipboardPngCrc(reinterpret_cast<const std::uint8_t *>(chunk.constData() + 4), std::size_t(chunk.size() - 4));
        QByteArray checksum(4, Qt::Uninitialized);
        qToBigEndian<quint32>(crc, checksum.data());
        chunk.append(checksum);
        auto withMetadata = png;
        withMetadata.insert(33, chunk);
        QCOMPARE(decodeClipboardPng(withMetadata).size(), image.size());
    }
    void canonicalPngReusesBytesAndPreservesAlphaPixels()
    {
        QImage original(7, 5, QImage::Format_RGBA8888);
        for (int y = 0; y < original.height(); ++y) {
            for (int x = 0; x < original.width(); ++x)
                original.setPixelColor(x, y, QColor(x * 31, y * 47, 193, (x + y) * 19));
        }
        QMimeData raster;
        raster.setImageData(original);
        const auto first = prepareClipboardImage(raster);
        QMimeData encoded;
        encoded.setData("image/png", first.png());
        const auto second = prepareClipboardImage(encoded);
        const auto *begin = reinterpret_cast<const std::uint8_t *>(first.png().constData());
        const auto sanitized = sanitizeClipboardPng(std::vector<std::uint8_t>(begin, begin + first.png().size()));
        QCOMPARE(second.png(), QByteArray(reinterpret_cast<const char *>(sanitized.bytes.data()), qsizetype(sanitized.bytes.size())));
        QCOMPARE(second.image().format(), QImage::Format_RGBA8888);
        for (int y = 0; y < original.height(); ++y) {
            for (int x = 0; x < original.width(); ++x)
                QCOMPARE(second.image().pixelColor(x, y), original.pixelColor(x, y));
        }
    }
    void preparationStripsMetadataFromEncodedPng()
    {
        QImage original(4, 3, QImage::Format_RGBA8888);
        original.fill(QColor(73, 51, 19, 103));
        original.setText("private-label", "private-value");
        QByteArray input;
        QBuffer output(&input);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QVERIFY(original.save(&output, "PNG"));
        QVERIFY(input.contains("private-label"));
        QMimeData mime;
        mime.setData("image/png", input);
        const auto prepared = prepareClipboardImage(mime);
        QVERIFY(!prepared.png().contains("private-label"));
        QVERIFY(prepared.image().textKeys().isEmpty());
        QCOMPARE(prepared.image().pixelColor(1, 1), original.pixelColor(1, 1));
    }
    void noncanonicalRgbIsConvertedToCanonicalRgba8()
    {
        QImage image(4, 6, QImage::Format_RGB888);
        image.fill(QColor(17, 83, 219));
        QByteArray bytes;
        QBuffer output(&bytes);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&output, "PNG"));
        QCOMPARE(quint8(bytes[25]), quint8(2));
        QMimeData mime;
        mime.setData("image/png", bytes);
        const auto prepared = prepareClipboardImage(mime);
        QCOMPARE(quint8(prepared.png()[24]), quint8(8));
        QCOMPARE(quint8(prepared.png()[25]), quint8(6));
        QCOMPARE(prepared.image().pixelColor(1, 2), image.pixelColor(1, 2));
    }
    void reuseIsExactOneShotAndMissRetiresOldResult()
    {
        QImage image(2, 3, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        QMimeData mime;
        mime.setImageData(image);
        const auto prepared = prepareClipboardImage(mime);
        ClipboardPngReuse reuse;
        reuse.remember(prepared);
        QCOMPARE(reuse.consume(prepared.png()), std::optional<QSize>(image.size()));
        QVERIFY(!reuse.consume(prepared.png()));
        reuse.remember(prepared);
        auto changed = prepared.png();
        changed[changed.size() / 2] ^= 1;
        QVERIFY(!reuse.consume(changed));
        QVERIFY(!reuse.consume(prepared.png()));
        reuse.remember(prepared);
        QVERIFY(!reuse.consume({}));
        QVERIFY(!reuse.consume(prepared.png()));
    }
    void matchingHeaderDoesNotBypassFullDecode()
    {
        QImage image(3, 4, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        QMimeData mime;
        mime.setImageData(image);
        auto png = encodeClipboardPng(mime);
        const auto idat = png.indexOf("IDAT");
        QVERIFY(idat > 4);
        const auto length = qFromBigEndian<quint32>(png.constData() + idat - 4);
        QVERIFY(length > 2);
        // Recompute a valid chunk CRC after damaging the zlib header. The
        // structural sanitizer accepts it; the full image decoder must fail.
        png[idat + 4] = 0;
        const auto crc = clipboardPngCrc(reinterpret_cast<const std::uint8_t *>(png.constData() + idat), length + 4);
        qToBigEndian<quint32>(crc, png.data() + idat + 4 + length);
        const auto *begin = reinterpret_cast<const std::uint8_t *>(png.constData());
        QVERIFY(!sanitizeClipboardPng(std::vector<std::uint8_t>(begin, begin + png.size())).bytes.empty());
        QMimeData damaged;
        damaged.setData("image/png", png);
        QVERIFY_EXCEPTION_THROWN(prepareClipboardImage(damaged), std::invalid_argument);
    }
    void pngRoundTripAndBounds()
    {
        QImage original(3, 4, QImage::Format_ARGB32);
        original.fill(Qt::green);
        QMimeData mime;
        mime.setImageData(original);
        const auto png = encodeClipboardPng(mime);
        QCOMPARE(decodeClipboardPng(png).size(), QSize(3, 4));
        QVERIFY_EXCEPTION_THROWN(decodeClipboardPng(png.left(png.size() - 1)), std::invalid_argument);
        auto huge = png;
        huge[16] = char(0x7f);
        QVERIFY_EXCEPTION_THROWN(decodeClipboardPng(huge), std::invalid_argument);
        auto animated = png;
        animated.insert(33, QByteArray::fromHex("000000006163544c00000000"));
        QVERIFY_EXCEPTION_THROWN(decodeClipboardPng(animated), std::invalid_argument);
    }
};
QTEST_MAIN(ClipboardSelectionTest)
#include "ClipboardSelectionTest.moc"
