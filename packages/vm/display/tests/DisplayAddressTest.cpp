#include "DisplayAddress.h"
#include <QtTest>

class DisplayAddressTest : public QObject {
    Q_OBJECT
private slots:
    void escapedLocalPath()
    {
        QString path;
#ifdef Q_OS_WIN
        QVERIFY(decodeDisplayBusPath("unix:path=C%3A/Private/bus", path));
        QCOMPARE(path, QString("C:/Private/bus"));
#else
        QVERIFY(decodeDisplayBusPath("unix:path=/private/test%20folder/bus", path));
        QCOMPARE(path, QString("/private/test folder/bus"));
#endif
    }
    void rejectsOtherTransportsAndMalformedEscapes()
    {
        QString path;
        const QStringList invalid{"tcp:host=localhost,port=1234", "unix:abstract=bus",
            "unix:path=relative", "unix:path=/tmp/bus;tcp:host=localhost",
            "unix:path=/tmp/bus,guid=other", "unix:path=/tmp/%00", "unix:path=/tmp/%ff",
            "unix:path=/tmp/%", "unix:path=/tmp/%2g", "unix:path=/" + QString(101, 'a')};
        for (const auto &address : invalid)
            QVERIFY2(!decodeDisplayBusPath(address, path), qPrintable(address));
    }
};
QTEST_MAIN(DisplayAddressTest)
#include "DisplayAddressTest.moc"
