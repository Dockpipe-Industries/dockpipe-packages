#include "NativeKeyMap.h"
#include <QtTest>

class NativeKeyMapTest : public QObject {
    Q_OBJECT
private slots:
    void physicalCodes_data()
    {
        QTest::addColumn<QString>("platform");
        QTest::addColumn<quint32>("scan");
        QTest::addColumn<quint32>("virtualKey");
        QTest::addColumn<int>("expected");
        QTest::newRow("x11-a") << QString("xcb") << quint32(38) << quint32(0) << 0x1e;
        QTest::newRow("x11-right-control") << QString("xcb") << quint32(105) << quint32(0) << 0x9d;
        QTest::newRow("windows-a") << QString("windows") << quint32(0x1e) << quint32(65) << 0x1e;
        QTest::newRow("windows-right-control") << QString("windows") << quint32(0xe01d) << quint32(0) << 0x9d;
        QTest::newRow("windows-left-arrow") << QString("windows") << quint32(0xe04b) << quint32(0) << 0xcb;
        QTest::newRow("mac-a-zero") << QString("cocoa") << quint32(0) << quint32(0) << 0x1e;
        QTest::newRow("mac-command") << QString("cocoa") << quint32(0) << quint32(55) << 0xdb;
        QTest::newRow("mac-right-control") << QString("cocoa") << quint32(0) << quint32(62) << 0x9d;
        QTest::newRow("mac-left-arrow") << QString("cocoa") << quint32(0) << quint32(123) << 0xcb;
        QTest::newRow("unknown-platform") << QString("wayland") << quint32(38) << quint32(0) << 0;
        QTest::newRow("unknown-windows") << QString("windows") << quint32(0xdeadbeef) << quint32(0) << 0;
        QTest::newRow("unknown-mac") << QString("cocoa") << quint32(0) << quint32(255) << 0;
    }
    void physicalCodes()
    {
        QFETCH(QString, platform);
        QFETCH(quint32, scan);
        QFETCH(quint32, virtualKey);
        QFETCH(int, expected);
        QCOMPARE(qemuKeyNumber(platform, scan, virtualKey), expected);
    }
};
QTEST_MAIN(NativeKeyMapTest)
#include "NativeKeyMapTest.moc"
