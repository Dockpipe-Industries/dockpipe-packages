#include "QemuDisplayTransport.h"
#include "GeometryQueryWorker.h"
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QtTest>
#ifndef Q_OS_WIN
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#endif

class TransportLifecycleTest : public QObject {
    Q_OBJECT
private slots:
    void geometryQueryCancellationJoinsDelayedWorker()
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool cancelled = false;
        std::atomic<bool> entered{false};
        std::atomic<bool> exited{false};
        GeometryQueryWorker worker([&] {
            std::lock_guard lock(mutex);
            cancelled = true;
            changed.notify_all();
        });
        QElapsedTimer timer;
        timer.start();
        QVERIFY(worker.start(7, [&] {
            entered = true;
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::seconds(5), [&] { return cancelled; });
            exited = true;
            return GeometryQueryWorker::Result{};
        }));
        QVERIFY(timer.elapsed() < 250);
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 1000);
        QVERIFY(!worker.start(8, [] { return GeometryQueryWorker::Result{}; }));
        QVERIFY(!worker.take().has_value());
        timer.restart();
        worker.stop();
        QVERIFY2(timer.elapsed() < 1000, "Cancellation must join the delayed geometry worker");
        QVERIFY(exited.load());
        QVERIFY(!worker.take().has_value());
        QVERIFY(!worker.start(9, [] { return GeometryQueryWorker::Result{}; }));
        worker.stop();
    }

    void geometryCompletionPreservesGenerationAndSingleSlot()
    {
        GeometryQueryWorker worker([] {});
        QVERIFY(worker.start(11, [] {
            GeometryQueryWorker::Result result;
            result.width = 720;
            result.height = 400;
            result.success = true;
            return result;
        }));
        std::optional<GeometryQueryWorker::Result> result;
        QElapsedTimer deadline;
        deadline.start();
        while (!result && deadline.elapsed() < 1000) {
            result = worker.take();
            if (!result)
                QTest::qWait(5);
        }
        QVERIFY(result.has_value());
        QCOMPARE(result->generation, std::uint64_t(11));
        QCOMPARE(result->width, std::uint32_t(720));
        QCOMPARE(result->height, std::uint32_t(400));
        QVERIFY(result->success);
        QVERIFY(!worker.take().has_value());
        QVERIFY(worker.start(12, [] { return GeometryQueryWorker::Result{}; }));
        worker.stop();
    }

    void geometryDestructorCancelsAndJoins()
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool cancelled = false;
        std::atomic<bool> exited{false};
        {
            GeometryQueryWorker worker([&] {
                std::lock_guard lock(mutex);
                cancelled = true;
                changed.notify_all();
            });
            QVERIFY(worker.start(1, [&] {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return cancelled; });
                exited = true;
                return GeometryQueryWorker::Result{};
            }));
        }
        QVERIFY(exited.load());
    }

    void rejectNonprivateAddress()
    {
        FrameStore frames;
        QemuDisplayTransport transport(frames, "tcp:host=localhost,port=1234", 0, 0, {});
        transport.start();
        QTRY_VERIFY_WITH_TIMEOUT(!transport.error().isEmpty(), 1000);
        transport.stop();
        transport.stop();
        QVERIFY(!transport.connected());
    }
#ifndef Q_OS_WIN
    void cancelStalledAuthentication()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = (QFileInfo(directory.path()).canonicalFilePath() + "/bus").toUtf8();
        int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(socket >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        QVERIFY(size_t(path.size()) < sizeof(address.sun_path));
        std::memcpy(address.sun_path, path.constData(), size_t(path.size()) + 1);
        QVERIFY(::bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        QVERIFY(::listen(socket, 1) == 0);
        FrameStore frames;
        QemuDisplayTransport transport(frames, "unix:path=" + QString::fromUtf8(path), 0, 0, {});
        transport.start();
        QTest::qWait(100);
        QVERIFY2(transport.error().isEmpty(), qPrintable(transport.error()));
        QElapsedTimer timer;
        timer.start();
        transport.stop();
        QVERIFY2(timer.elapsed() < 1000, "Cancellation must interrupt peer authentication");
        QVERIFY(!transport.connected());
        ::close(socket);
    }
#endif
};
QTEST_MAIN(TransportLifecycleTest)
#include "TransportLifecycleTest.moc"
