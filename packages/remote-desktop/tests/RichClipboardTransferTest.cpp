#include "RichClipboardTransfer.h"
#include "ClipboardSelection.h"
#include "ProcessChannel.h"
#include "../native/AssetClipboardProtocol.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace dockpipe::desktop {
class RichClipboardTransferTest final : public QObject {
    Q_OBJECT
    QTemporaryDir m_cache;
    static QByteArray encode(const AssetRecord &record)
    {
        const auto data = encodeAssetClipboardRecord(record);
        return QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()));
    }
    void drain(RichClipboardTransfer &adapter)
    {
        auto done = std::make_shared<std::atomic_bool>(false);
        adapter.scheduleIo([done] { *done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done->load(), 3000);
        QCoreApplication::processEvents();
    }
    void arm(RichClipboardTransfer &adapter)
    {
        adapter.refresh();
        drain(adapter);
        AssetRecord ack;
        ack.operation = AssetOperation::EpochAcknowledged;
        ack.control = AssetOperation::Arm;
        ack.epoch = 1;
        adapter.receive(encode(ack));
        drain(adapter);
    }
    AssetRecord offer(std::vector<AssetItem> items = {{1, 3, "received.txt"}})
    {
        AssetRecord record;
        record.operation = AssetOperation::Offer;
        record.epoch = 1;
        record.serial = 1;
        record.transfer.fill(5);
        record.format = AssetFormat::Files;
        record.items = std::move(items);
        for (const auto &item : record.items)
            record.totalBytes += item.size;
        return record;
    }
    void data(RichClipboardTransfer &adapter, AssetRecord record, const QByteArray &bytes = "abc")
    {
        record.operation = AssetOperation::Chunk;
        record.items.clear();
        record.item = 1;
        record.offset = 0;
        record.data.assign(bytes.begin(), bytes.end());
        adapter.receive(encode(record));
        record.operation = AssetOperation::Finish;
        record.offset = bytes.size();
        record.data.clear();
        const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        std::copy(hash.begin(), hash.end(), record.sha256.begin());
        adapter.receive(encode(record));
    }
private slots:
    void initTestCase()
    {
        QVERIFY(m_cache.isValid());
        qputenv("XDG_CACHE_HOME", m_cache.path().toUtf8());
        QCoreApplication::setApplicationName("dockpipe-rich-clipboard-fixture");
        QGuiApplication::clipboard()->setText("fixture baseline");
    }
    void incomingFilesPublishPrivateCopyUrlsAndDoNotEcho()
    {
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, false, true, [] { return true; }, nullptr);
        QSignalSpy published(&adapter, &RichClipboardTransfer::published);
        QSignalSpy outgoing(&adapter, &RichClipboardTransfer::outgoingCommitted);
        arm(adapter);
        const auto record = offer();
        adapter.receive(encode(record));
        data(adapter, record);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 3000);
        const auto *mime = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mime->hasUrls());
        const auto path = mime->urls().value(0).toLocalFile();
        QVERIFY(path.startsWith(m_cache.path() + '/'));
        QVERIFY(QFileInfo(path).isFile());
        QCOMPARE(QFileInfo(path).fileName(), QString("received.txt"));
        QVERIFY(mime->data("x-special/gnome-copied-files").startsWith("copy\n"));
        QVERIFY(adapter.ownsClipboard());
        QCOMPARE(outgoing.count(), 0);
        QVERIFY(!adapter.outgoingReady());
        QVERIFY(adapter.paste()); // same session/generation is already ready remotely
        adapter.changed();
        QVERIFY(adapter.paste()); // synchronous own-publication signal does not re-upload
        RichClipboardTransfer other(channel, false, true, [] { return false; }, nullptr);
        QVERIFY(!other.ownsClipboard());
        other.stop();
        adapter.stop();
        QVERIFY(adapter.wait());
        QVERIFY(QFileInfo(path).isFile()); // published URLs outlive this surface
    }
    void incomingPngIsValidatedAndCanonicalBeforePublication()
    {
        QImage image(2, 3, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        QMimeData source;
        source.setImageData(image);
        const auto bytes = encodeClipboardPng(source);
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, true, false, [] { return true; }, nullptr);
        QSignalSpy published(&adapter, &RichClipboardTransfer::published);
        arm(adapter);
        auto record = offer({{1, std::uint64_t(bytes.size()), "clipboard.png"}});
        record.format = AssetFormat::Png;
        record.width = 2;
        record.height = 3;
        adapter.receive(encode(record));
        data(adapter, record, bytes);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 3000);
        const auto *mime = QGuiApplication::clipboard()->mimeData();
        QCOMPARE(qvariant_cast<QImage>(mime->imageData()).format(), QImage::Format_RGBA8888);
        QCOMPARE(decodeClipboardPng(mime->data("image/png")).size(), image.size());
    }
    void mismatchedPngManifestNeverPublishes()
    {
        QGuiApplication::clipboard()->setText("kept baseline");
        QImage image(2, 3, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        QMimeData source;
        source.setImageData(image);
        const auto bytes = encodeClipboardPng(source);
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, true, false, [] { return true; }, nullptr);
        QSignalSpy published(&adapter, &RichClipboardTransfer::published);
        arm(adapter);
        auto record = offer({{1, std::uint64_t(bytes.size()), "clipboard.png"}});
        record.format = AssetFormat::Png;
        record.width = 3;
        record.height = 2;
        adapter.receive(encode(record));
        data(adapter, record, bytes);
        drain(adapter);
        QCOMPARE(published.count(), 0);
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("kept baseline"));
    }
    void pauseFencesQueuedPublication()
    {
        QGuiApplication::clipboard()->setText("preserved baseline");
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, false, true, [] { return true; }, nullptr);
        QSignalSpy published(&adapter, &RichClipboardTransfer::published);
        arm(adapter);
        const auto record = offer();
        adapter.receive(encode(record));
        data(adapter, record);
        adapter.pause();
        drain(adapter);
        QCOMPARE(published.count(), 0);
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("preserved baseline"));
    }
    void normalizedDuplicateManifestRejected()
    {
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, false, true, [] { return true; }, nullptr);
        arm(adapter);
        const auto record = offer({{1, 3, "\xc3\x89.txt"}, {2, 3, "E\xcc\x81.TXT"}});
        QVERIFY_EXCEPTION_THROWN(adapter.receive(encode(record)), std::invalid_argument);
    }
    void outgoingCompletionAndProgressRequireValidatedPeerRecords()
    {
        QTemporaryDir sources;
        QVERIFY(sources.isValid());
        const auto path = sources.path() + "/generated.bin";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("abc"), qint64(3));
        file.close();
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, false, true, [] { return true; }, nullptr);
        QSignalSpy committed(&adapter, &RichClipboardTransfer::outgoingCommitted);
        QSignalSpy progress(&adapter, &RichClipboardTransfer::progress);
        QSignalSpy statuses(&adapter, &RichClipboardTransfer::status);
        QSignalSpy invalidated(&adapter, &RichClipboardTransfer::invalidated);
        arm(adapter);
        QVERIFY(adapter.isAdmitted());
        QCOMPARE(adapter.focusEpoch(), quint64(1));
        adapter.drop({path});
        drain(adapter);
        const auto bytes = adapter.queuedRecord();
        QVERIFY(!bytes.isEmpty());
        auto record = decodeAssetClipboardRecord(reinterpret_cast<const std::uint8_t *>(bytes.constData()), bytes.size());
        const auto generation = adapter.selectionGeneration();
        QVERIFY(!adapter.outgoingReady());
        record.operation = AssetOperation::Accept;
        record.grantBytes = 3;
        record.window = 4;
        adapter.receive(encode(record));
        drain(adapter);
        QVERIFY(!adapter.outgoingReady());
        QCOMPARE(committed.count(), 0);
        record.operation = AssetOperation::Credit;
        record.item = 1;
        record.offset = 3;
        adapter.receive(encode(record));
        drain(adapter);
        QVERIFY(!adapter.outgoingReady());
        QVERIFY(progress.count() > 0);
        QCOMPARE(progress.last().at(0).toInt(), 100);
        QVERIFY(statuses.last().at(0).toString().contains("Verifying"));
        record.operation = AssetOperation::Committed;
        adapter.receive(encode(record));
        drain(adapter);
        QCOMPARE(committed.count(), 1);
        QCOMPARE(committed.first().at(0).toLongLong(), generation);
        QCOMPARE(committed.first().at(1).toULongLong(), quint64(1));
        QVERIFY(adapter.outgoingReady());
        QCOMPARE(progress.last().at(0).toInt(), -2);
        for (const auto &sample : progress) {
            const int value = sample.at(0).toInt();
            QVERIFY(value >= -2 && value <= 100);
        }
        adapter.superseded();
        QVERIFY(!adapter.outgoingReady());
        QVERIFY(invalidated.count() >= 2);
        adapter.pause();
        QVERIFY(!adapter.isAdmitted());
        QCOMPARE(committed.count(), 1);
    }
    void priorCancelCannotInvalidateNewOutgoingSelection_data()
    {
        QTest::addColumn<bool>("receivedOldEpoch");
        QTest::addColumn<bool>("currentCancel");
        QTest::newRow("trusted-old-transfer-cancel") << false << false;
        QTest::newRow("received-old-epoch-cancel") << true << false;
        QTest::newRow("received-current-transfer-cancel") << false << true;
    }

    void priorCancelCannotInvalidateNewOutgoingSelection()
    {
        QFETCH(bool, receivedOldEpoch);
        QFETCH(bool, currentCancel);
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, true, false, [] { return true; }, nullptr);
        QSignalSpy published(&adapter, &RichClipboardTransfer::published);
        QSignalSpy invalidated(&adapter, &RichClipboardTransfer::invalidated);
        QSignalSpy committed(&adapter, &RichClipboardTransfer::outgoingCommitted);
        arm(adapter);
        QImage incomingImage(2, 3, QImage::Format_RGBA8888);
        incomingImage.fill(Qt::red);
        QMimeData incomingMime;
        incomingMime.setImageData(incomingImage);
        const auto png = encodeClipboardPng(incomingMime);
        auto previous = offer({{1, std::uint64_t(png.size()), "clipboard.png"}});
        previous.format = AssetFormat::Png;
        previous.width = 2;
        previous.height = 3;
        adapter.receive(encode(previous));
        data(adapter, previous, png);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 3000);
        drain(adapter);
        if (receivedOldEpoch) {
            adapter.pause();
            adapter.refresh();
            drain(adapter);
            AssetRecord ack;
            ack.operation = AssetOperation::EpochAcknowledged;
            ack.control = AssetOperation::Pause;
            ack.epoch = 1;
            adapter.receive(encode(ack));
            drain(adapter);
            ack.control = AssetOperation::Arm;
            ack.epoch = 2;
            adapter.receive(encode(ack));
            drain(adapter);
        }
        QImage outgoingImage(3, 2, QImage::Format_RGBA8888);
        outgoingImage.fill(Qt::blue);
        auto *mime = new QMimeData;
        mime->setImageData(outgoingImage);
        QGuiApplication::clipboard()->setMimeData(mime);
        adapter.changed();
        drain(adapter);
        const auto bytes = adapter.queuedRecord();
        QVERIFY(!bytes.isEmpty());
        auto outgoing = decodeAssetClipboardRecord(reinterpret_cast<const std::uint8_t *>(bytes.constData()), bytes.size());
        invalidated.clear();
        if (currentCancel) {
            outgoing.operation = AssetOperation::Cancel;
            outgoing.reason = "user_cancelled";
            adapter.receive(encode(outgoing));
            drain(adapter);
            QCOMPARE(invalidated.count(), 1);
            QVERIFY(!adapter.outgoingReady());
            QCOMPARE(committed.count(), 0);
            adapter.stop();
            QVERIFY(adapter.wait());
            return;
        }
        previous.operation = AssetOperation::Cancel;
        previous.reason = "user_cancelled";
        if (receivedOldEpoch)
            adapter.receive(encode(previous));
        else
            adapter.enqueue(encode(previous));
        drain(adapter);
        QCOMPARE(invalidated.count(), 0);
        QVERIFY(adapter.isAdmitted());
        outgoing.operation = AssetOperation::Accept;
        outgoing.grantBytes = outgoing.totalBytes;
        outgoing.window = 4;
        adapter.receive(encode(outgoing));
        drain(adapter);
        outgoing.operation = AssetOperation::Credit;
        outgoing.item = 1;
        outgoing.offset = outgoing.totalBytes;
        adapter.receive(encode(outgoing));
        drain(adapter);
        outgoing.operation = AssetOperation::Committed;
        adapter.receive(encode(outgoing));
        drain(adapter);
        QCOMPARE(committed.count(), 1);
        QVERIFY(adapter.outgoingReady());
        adapter.stop();
        QVERIFY(adapter.wait());
    }

    void queuedOldOfferCannotRelabelCurrentGeneration()
    {
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, true, false, [] { return true; }, nullptr);
        QSignalSpy invalidated(&adapter, &RichClipboardTransfer::invalidated);
        arm(adapter);
        QImage first(2, 2, QImage::Format_RGBA8888);
        first.fill(Qt::red);
        auto *firstMime = new QMimeData;
        firstMime->setImageData(first);
        QGuiApplication::clipboard()->setMimeData(firstMime);
        adapter.changed();
        drain(adapter);
        const auto oldBytes = adapter.queuedRecord();
        QVERIFY(!oldBytes.isEmpty());
        const auto oldGeneration = adapter.selectionGeneration();
        auto oldOffer = decodeAssetClipboardRecord(reinterpret_cast<const std::uint8_t *>(oldBytes.constData()), oldBytes.size());
        QImage second(3, 3, QImage::Format_RGBA8888);
        second.fill(Qt::blue);
        auto *secondMime = new QMimeData;
        secondMime->setImageData(second);
        QGuiApplication::clipboard()->setMimeData(secondMime);
        adapter.changed();
        QVERIFY(adapter.selectionGeneration() > oldGeneration);
        invalidated.clear();
        // Force the delayed GUI emission after the new selection/gesture bind.
        adapter.enqueue(encode(oldOffer), oldGeneration);
        oldOffer.operation = AssetOperation::Cancel;
        oldOffer.reason = "source_changed";
        adapter.enqueue(encode(oldOffer));
        QCOMPARE(invalidated.count(), 0);
        adapter.stop();
        QVERIFY(adapter.wait());
    }

    void preparingAndInvalidationPhasesAreVisible()
    {
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, true, false, [] { return true; }, nullptr);
        QSignalSpy progress(&adapter, &RichClipboardTransfer::progress);
        QSignalSpy statuses(&adapter, &RichClipboardTransfer::status);
        QSignalSpy invalidated(&adapter, &RichClipboardTransfer::invalidated);
        arm(adapter);
        QImage image(2, 2, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        auto *mime = new QMimeData;
        mime->setImageData(image);
        QGuiApplication::clipboard()->setMimeData(mime);
        adapter.changed();
        QVERIFY(statuses.last().at(0).toString().contains("Preparing clipboard image"));
        QCOMPARE(progress.last().at(0).toInt(), -1);
        adapter.pause();
        QVERIFY(invalidated.count() >= 2);
        QCOMPARE(progress.last().at(0).toInt(), -2);
        drain(adapter);
        adapter.stop();
        QVERIFY(adapter.wait());
    }
    void rapidCopiesKeepOnlyOnePendingNormalization()
    {
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, true, false, [] { return true; }, nullptr);
        arm(adapter);
        std::mutex mutex;
        std::condition_variable changed;
        bool released = false;
        auto entered = std::make_shared<std::atomic_bool>(false);
        adapter.scheduleIo([&mutex, &changed, &released, entered] {
            *entered = true;
            std::unique_lock lock(mutex);
            changed.wait(lock, [&released] { return released; });
        });
        QTRY_VERIFY_WITH_TIMEOUT(entered->load(), 3000);
        for (int index = 0; index < 64; ++index) {
            QImage image(64, 64, QImage::Format_RGBA8888);
            image.fill(QColor(index, 0, 0));
            auto *mime = new QMimeData;
            mime->setImageData(image);
            QGuiApplication::clipboard()->setMimeData(mime);
            adapter.changed();
        }
        const auto queued = adapter.pendingIoJobs();
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
        QVERIFY2(queued <= 3, "Blocked job plus one replaceable selection and one coalesced poll only");
        drain(adapter);
        adapter.stop();
        QVERIFY(adapter.wait());
    }
    void failedAsyncJobInvalidatesIntentAndHidesProgress()
    {
        ProcessChannel channel;
        RichClipboardTransfer adapter(channel, true, false, [] { return true; }, nullptr);
        QSignalSpy invalidated(&adapter, &RichClipboardTransfer::invalidated);
        QSignalSpy progress(&adapter, &RichClipboardTransfer::progress);
        QSignalSpy statuses(&adapter, &RichClipboardTransfer::status);
        arm(adapter);
        QImage image(2, 2, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        auto *mime = new QMimeData;
        mime->setImageData(image);
        QGuiApplication::clipboard()->setMimeData(mime);
        adapter.changed();
        drain(adapter);
        QVERIFY(!progress.isEmpty());
        QVERIFY(progress.last().at(0).toInt() >= -1);
        invalidated.clear();
        progress.clear();
        statuses.clear();
        adapter.scheduleIo([] { throw std::runtime_error("Generated async fixture failure"); });
        QTRY_COMPARE_WITH_TIMEOUT(invalidated.count(), 1, 3000);
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last().at(0).toInt(), -2);
        QVERIFY(!statuses.isEmpty());
        QVERIFY(statuses.last().at(0).toString().contains("transfer stopped"));
        QVERIFY(!adapter.isAdmitted());
        QVERIFY(!adapter.outgoingReady());
        adapter.stop();
        QVERIFY(adapter.wait());
    }

    void delayedIoCloseRemainsUnverifiedAndRetainsState()
    {
        ProcessChannel channel;
        auto adapter = std::make_unique<RichClipboardTransfer>(channel, false, true, [] { return true; }, nullptr);
        arm(*adapter);
        std::mutex mutex;
        std::condition_variable changed;
        bool released = false;
        auto entered = std::make_shared<std::atomic_bool>(false);
        auto exited = std::make_shared<std::atomic_bool>(false);
        adapter->scheduleIo([&mutex, &changed, &released, entered, exited] {
            *entered = true;
            std::unique_lock lock(mutex);
            changed.wait(lock, [&released] { return released; });
            *exited = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(entered->load(), 3000);
        adapter->pause();
        adapter->stop();
        QVERIFY(!adapter->stopped());
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!adapter->wait());
        QVERIFY(elapsed.elapsed() < 4000);
        adapter.reset(); // another bounded wait; task/state/module remain retained
        QVERIFY(!exited->load());
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
        QTRY_VERIFY_WITH_TIMEOUT(exited->load(), 3000);
    }
};
} // namespace dockpipe::desktop
QTEST_MAIN(dockpipe::desktop::RichClipboardTransferTest)
#include "RichClipboardTransferTest.moc"
