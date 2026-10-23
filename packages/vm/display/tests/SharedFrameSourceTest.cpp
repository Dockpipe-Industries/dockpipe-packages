#include "SharedFrameSource.h"
#include <QtTest>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <QDir>

namespace {
int anonymousFile()
{
#ifdef Q_OS_MACOS
    auto path = (QDir::tempPath() + "/display-test-XXXXXX").toUtf8();
    const int fd = mkstemp(path.data());
    if (fd >= 0)
        unlink(path.constData());
    return fd;
#else
    return memfd_create("display-test", MFD_CLOEXEC);
#endif
}
}

class SharedFrameSourceTest : public QObject {
    Q_OBJECT
private slots:
    void sharedRowsOffsetAndTruncation()
    {
        FrameStore frames;
        std::atomic<bool> stopped{false};
        SharedFrameSource source(frames, stopped);
        int fd = anonymousFile();
        QVERIFY(fd >= 0);
        QVERIFY(ftruncate(fd, 32) == 0);
        const quint32 pixels[] = {0, 0, 0x00ff0000, 0x0000ff00, 0, 0x000000ff, 0x00ffffff, 0};
        QCOMPARE(pwrite(fd, pixels, sizeof(pixels), 0), ssize_t(sizeof(pixels)));
        int duplicate = dup(fd);
        QVERIFY(source.replace(duplicate, 8, 2, 2, 12, 0x20020888));
        QCOMPARE(frames.snapshot().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(frames.snapshot().pixelColor(0, 1), QColor(Qt::blue));
        const quint32 yellow = 0x00ffff00;
        QCOMPARE(pwrite(fd, &yellow, 4, 24), ssize_t(4));
        QVERIFY(source.update(1, 1, 1, 1));
        QCOMPARE(frames.snapshot().pixelColor(1, 1), QColor(Qt::yellow));
        QVERIFY(ftruncate(fd, 0) == 0);
        QVERIFY(!source.update(0, 0, 1, 1));
        QCOMPARE(fcntl(duplicate, F_GETFD), -1);
        QCOMPARE(frames.snapshot().pixelColor(1, 1), QColor(Qt::yellow));
        close(fd);
    }
    void rejectsWrongFdAndBounds()
    {
        FrameStore frames;
        std::atomic<bool> stopped{false};
        SharedFrameSource source(frames, stopped);
        int fd = anonymousFile();
        QVERIFY(fd >= 0);
        QVERIFY(ftruncate(fd, 4) == 0);
        QVERIFY(!source.replace(dup(fd), 1, 1, 1, 4, 0x20020888));
        QVERIFY(!source.replace(dup(fd), 0, 4097, 1, 4, 0x20020888));
        QVERIFY(!source.replace(dup(fd), 0, 1, 1, 4, 0x11111111));
        QVERIFY(!source.replace(open("/dev/zero", O_RDONLY), 0, 1, 1, 4, 0x20020888));
        stopped = true;
        QVERIFY(!source.replace(dup(fd), 0, 1, 1, 4, 0x20020888));
        QVERIFY(frames.snapshot().isNull());
        close(fd);
    }
};
QTEST_MAIN(SharedFrameSourceTest)
#include "SharedFrameSourceTest.moc"
