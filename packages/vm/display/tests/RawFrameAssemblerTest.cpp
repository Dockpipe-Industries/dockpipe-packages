#include "RawFrameAssembler.h"
#include <QtTest>

class RawFrameAssemblerTest : public QObject {
    Q_OBJECT
private slots:
    void resizeUsesAuthoritativeGeometry()
    {
        FrameStore frames;
        const quint32 placeholder = 0;
        QVERIFY(frames.scanout(1, 1, 4, 0x20020888,
            reinterpret_cast<const unsigned char *>(&placeholder), 4));
        RawFrameAssembler raw(frames);
        const quint32 top[] = {0x00ff0000, 0x0000ff00};
        const quint32 bottom[] = {0x000000ff, 0x00ffffff};
        QVERIFY(raw.enqueue(0, 0, 2, 1, 8, 0x20020888,
            reinterpret_cast<const unsigned char *>(top), 8));
        QVERIFY(raw.enqueue(0, 1, 2, 1, 8, 0x20020888,
            reinterpret_cast<const unsigned char *>(bottom), 8));
        QCOMPARE(frames.snapshot().size(), QSize(1, 1));
        QVERIFY(raw.applyGeometry(2, 2));
        QCOMPARE(frames.snapshot().size(), QSize(2, 2));
        QCOMPARE(frames.snapshot().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(frames.snapshot().pixelColor(0, 1), QColor(Qt::blue));
        QVERIFY(raw.enqueue(0, 0, 1, 1, 4, 0x20020888,
            reinterpret_cast<const unsigned char *>(bottom), 4));
        QVERIFY(raw.applyGeometry(1, 1));
        QCOMPARE(frames.snapshot().size(), QSize(1, 1));
        QCOMPARE(frames.snapshot().pixelColor(0, 0), QColor(Qt::blue));
    }
    void rejectsInconsistentGeometryAndBoundedBacklog()
    {
        FrameStore frames;
        RawFrameAssembler raw(frames);
        const quint32 pixels[] = {0, 0};
        QVERIFY(raw.enqueue(0, 0, 2, 1, 8, 0x20020888,
            reinterpret_cast<const unsigned char *>(pixels), 8));
        QVERIFY(!raw.applyGeometry(1, 1));
        for (int count = 0; count < 128; ++count)
            QVERIFY(raw.enqueue(0, 0, 1, 1, 4, 0x20020888,
                reinterpret_cast<const unsigned char *>(pixels), 4));
        QVERIFY(!raw.enqueue(0, 0, 1, 1, 4, 0x20020888,
            reinterpret_cast<const unsigned char *>(pixels), 4));
        raw.clear();
        QVERIFY(!raw.enqueue(-1, 0, 1, 1, 4, 0x20020888,
            reinterpret_cast<const unsigned char *>(pixels), 4));
        QVERIFY(!raw.applyGeometry(4096, 4096));
    }
};
QTEST_MAIN(RawFrameAssemblerTest)
#include "RawFrameAssemblerTest.moc"
