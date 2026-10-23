#include "FrameStore.h"
#include <QtTest>
#include <limits>

class FrameStoreTest : public QObject {
    Q_OBJECT
private slots:
    void paddedRowsAndPartialUpdate()
    {
        FrameStore store;
        // Native-endian PIXMAN_X8R8G8B8. Distinct top/bottom colors prove orientation.
        const quint32 pixels[] = {0x00ff0000, 0x0000ff00, 0, 0x000000ff, 0x00ffffff, 0};
        QVERIFY(store.scanout(2, 2, 12, 0x20020888,
            reinterpret_cast<const unsigned char *>(pixels), sizeof(pixels)));
        QCOMPARE(store.snapshot().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(store.snapshot().pixelColor(0, 1), QColor(Qt::blue));
        QCOMPARE(store.snapshot().pixel(0, 0), QRgb(0xffff0000));
        const auto heldSnapshot = store.snapshot();
        const quint32 patch = 0x00ffff00;
        QVERIFY(store.update(1, 1, 1, 1, 4, 0x20020888,
            reinterpret_cast<const unsigned char *>(&patch), 4));
        QCOMPARE(store.snapshot().pixelColor(1, 1), QColor(Qt::yellow));
        QCOMPARE(store.snapshot().pixel(1, 1), QRgb(0xffffff00));
        QCOMPARE(heldSnapshot.pixelColor(1, 1), QColor(Qt::white));
        QCOMPARE(store.revisions(), quint64(2));
    }
    void rejectsMalformedAndPreservesFrame()
    {
        FrameStore store;
        const quint32 pixel = 0x00123456;
        const auto *bytes = reinterpret_cast<const unsigned char *>(&pixel);
        QVERIFY(store.scanout(1, 1, 4, 0x20020888, bytes, 4));
        QVERIFY(!store.scanout(0, 1, 4, 0x20020888, bytes, 4));
        QVERIFY(!store.scanout(4097, 1, 4, 0x20020888, bytes, 4));
        QVERIFY(!store.scanout(1, 1, 3, 0x20020888, bytes, 3));
        QVERIFY(!store.scanout(1, 1, 4, 0x20020888, bytes, 3));
        QVERIFY(!store.scanout(1, 1, 4, 0x12345678, bytes, 4));
        QVERIFY(!store.scanout(4096, 4096, 16384, 0x20020888, bytes, 67108864));
        QVERIFY(!store.scanout(1, 2, std::numeric_limits<quint32>::max(),
                              0x20020888, bytes, 4));
        QVERIFY(!store.update(-1, 0, 1, 1, 4, 0x20020888, bytes, 4));
        QVERIFY(!store.update(0, 0, 2, 1, 8, 0x20020888, bytes, 8));
        QCOMPARE(store.revisions(), quint64(1));
        QCOMPARE(store.snapshot().pixelColor(0, 0), QColor(0x12, 0x34, 0x56));
        store.disable();
        QVERIFY(store.snapshot().isNull());
        QVERIFY(!store.update(0, 0, 1, 1, 4, 0x20020888, bytes, 4));
    }
};
QTEST_MAIN(FrameStoreTest)
#include "FrameStoreTest.moc"
