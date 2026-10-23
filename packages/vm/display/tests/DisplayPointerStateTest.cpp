#include "DisplayPointerStateStore.h"
#include <QtTest>
#include <array>
#include <atomic>
#include <thread>

class DisplayPointerStateTest : public QObject {
    Q_OBJECT
private slots:
    void boundedOwnedCursorAndVisibility()
    {
        DisplayPointerStateStore store;
        std::array<quint32, 4> pixels{0xff102030, 0x80405060, 0x00000000, 0xffffffff};
        QVERIFY(store.defineCursor(2, 2, 1, 1, reinterpret_cast<const unsigned char *>(pixels.data()), sizeof(pixels)));
        const auto first = store.snapshot(true);
        QCOMPARE(first.hotSpot, QPoint(1, 1));
        QCOMPARE(first.cursor.pixel(0, 0), pixels[0]);
        QCOMPARE(qAlpha(first.cursor.pixel(1, 0)), 128);
        pixels[0] = 0;
        QCOMPARE(first.cursor.pixel(0, 0), quint32(0xff102030));
        QVERIFY(store.setPosition(100, 200, 1));
        auto visible = store.snapshot(false);
        QVERIFY(!visible.absolute);
        QVERIFY(visible.visibilityKnown);
        QVERIFY(visible.visible);
        QCOMPARE(visible.position, QPoint(100, 200));
        QVERIFY(!store.defineCursor(257, 1, 0, 0, reinterpret_cast<const unsigned char *>(pixels.data()), sizeof(pixels)));
        QVERIFY(!store.defineCursor(2, 2, 2, 0, reinterpret_cast<const unsigned char *>(pixels.data()), sizeof(pixels)));
        QVERIFY(!store.defineCursor(2, 2, 0, 0, reinterpret_cast<const unsigned char *>(pixels.data()), sizeof(pixels) - 1));
        QVERIFY(!store.setPosition(INT_MAX, 0, 1));
        QVERIFY(!store.setPosition(0, 0, 2));
        QCOMPARE(store.snapshot(true).revision, visible.revision);
        store.clear();
        QVERIFY(store.snapshot(true).cursor.isNull());
        QVERIFY(!store.snapshot(true).visibilityKnown);
    }

    void concurrentSnapshotsRemainConsistentAndOwned()
    {
        DisplayPointerStateStore store;
        const std::array<quint32, 4> pixels{0xff102030, 0xff102030, 0xff102030, 0xff102030};
        std::atomic<bool> valid{true};
        std::thread writer([&] {
            for (int iteration = 0; iteration < 1000; ++iteration) {
                const int side = iteration % 2 + 1;
                if (!store.defineCursor(side, side, side - 1, side - 1,
                    reinterpret_cast<const unsigned char *>(pixels.data()), quint64(side * side * 4)))
                    valid = false;
            }
        });
        bool consistent = true;
        for (int iteration = 0; iteration < 1000; ++iteration) {
            const auto state = store.snapshot(true);
            if (!state.cursor.isNull()) {
                consistent = consistent && state.hotSpot.x() >= 0 && state.hotSpot.x() < state.cursor.width() &&
                    state.hotSpot.y() >= 0 && state.hotSpot.y() < state.cursor.height() &&
                    state.cursor.pixel(0, 0) == quint32(0xff102030);
            }
        }
        writer.join();
        QVERIFY(valid.load());
        QVERIFY(consistent);
    }
};
QTEST_GUILESS_MAIN(DisplayPointerStateTest)
#include "DisplayPointerStateTest.moc"
