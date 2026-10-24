#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc_info.h>
#endif
#endif
#include <gio/gunixsocketaddress.h>
#include "QmpAbsoluteInput.h"
#include "FrameStore.h"
#include "NativeDisplayPlatform.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <thread>
#include "NativeFixtureOwnership.h"

namespace {
int ownProcessId()
{
#ifdef Q_OS_WIN
    return int(GetCurrentProcessId());
#else
    return int(getpid());
#endif
}

QString ownStartToken()
{
#ifdef Q_OS_WIN
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return {};
    return QString::number((quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime);
#elif defined(Q_OS_MACOS)
    struct proc_bsdinfo information{};
    if (proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &information, sizeof(information)) != sizeof(information))
        return {};
    return QString::number(information.pbi_start_tvsec) + ":" + QString::number(information.pbi_start_tvusec);
#else
    QFile file(QStringLiteral("/proc/self/stat"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto data = file.readAll();
    return QString::fromLatin1(data.mid(data.lastIndexOf(')') + 2).split(' ')[19]);
#endif
}

class MonitorFixture {
public:
    explicit MonitorFixture(QString mode = {})
        : directory_(QDir::tempPath() + "/dp-qmp-XXXXXX"), mode_(std::move(mode))
    {
        if (!directory_.isValid() || !NativeDisplayPlatform::protectDirectory(directory_.path()))
            qFatal("Cannot protect isolated QMP fixture directory");
        const auto canonical = QFileInfo(directory_.path()).canonicalFilePath();
        path_ = canonical + "/input";
        if (!NativeDisplayPlatform::privateDirectory(canonical) || path_.toUtf8().size() > 100)
            qFatal("Cannot admit isolated QMP fixture path");
        cancel_ = g_cancellable_new();
        listener_ = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, nullptr);
        if (!listener_)
            qFatal("Cannot create native AF_UNIX fixture socket");
        auto *address = g_unix_socket_address_new(path_.toUtf8().constData());
        const bool bound = g_socket_bind(listener_, address, false, nullptr) && g_socket_listen(listener_, nullptr);
        g_object_unref(address);
        if (!bound)
            qFatal("Cannot bind isolated QMP fixture socket");
#ifdef Q_OS_WIN
        if (!NativeFixtureOwnership::assignFreshObjectToCurrentUser(path_))
            qFatal("Cannot establish current-user ownership for fresh QMP fixture socket");
#endif
        g_socket_set_blocking(listener_, false);
        worker_ = std::thread([this] { serve(); });
    }

    ~MonitorFixture()
    {
        g_cancellable_cancel(cancel_);
        finish();
        g_socket_close(listener_, nullptr);
        g_object_unref(listener_);
        g_object_unref(cancel_);
        QFile::remove(path_);
    }

    QString address() const { return "unix:path=" + path_; }

    void finish()
    {
        if (worker_.joinable())
            worker_.join();
    }

    QList<QJsonObject> requests;

private:
    using Deadline = std::chrono::steady_clock::time_point;

    bool wait(GSocket *socket, GIOCondition condition, Deadline deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
        return remaining > 0 && g_socket_condition_timed_wait(socket, condition, remaining, cancel_, nullptr);
    }

    bool write(GSocket *socket, const QByteArray &bytes)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            if (!wait(socket, G_IO_OUT, deadline))
                return false;
            GError *error = nullptr;
            const auto count = g_socket_send(socket, bytes.constData() + offset, size_t(bytes.size() - offset), cancel_, &error);
            const bool pending = count < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
            g_clear_error(&error);
            if (pending)
                continue;
            if (count <= 0)
                return false;
            offset += count;
        }
        return true;
    }

    bool readRequest(GSocket *socket, QJsonObject &request)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        QByteArray line;
        while (line.size() < 4096) {
            if (!wait(socket, G_IO_IN, deadline))
                return false;
            char byte;
            GError *error = nullptr;
            const auto count = g_socket_receive(socket, &byte, 1, cancel_, &error);
            const bool pending = count < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
            g_clear_error(&error);
            if (pending)
                continue;
            if (count != 1)
                return false;
            if (byte == '\n') {
                request = QJsonDocument::fromJson(line).object();
                return !request.isEmpty();
            }
            line.append(byte);
        }
        return false;
    }

    void serve()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
        if (!wait(listener_, G_IO_IN, deadline))
            return;
        auto *connection = g_socket_accept(listener_, cancel_, nullptr);
        if (!connection)
            return;
        g_socket_set_blocking(connection, false);
        serveConnection(connection);
        g_socket_close(connection, nullptr);
        g_object_unref(connection);
    }

    void serveConnection(GSocket *connection)
    {
        if (!write(connection, "{\"QMP\":{}}\n"))
            return;
        for (int count = 0; count < 3; ++count) {
            QJsonObject request;
            if (!readRequest(connection, request))
                return;
            requests.append(request);
            if (count == 1 && mode_ == "stall") {
                std::this_thread::sleep_for(std::chrono::milliseconds(2500));
                return;
            }
            if (count == 1 && mode_ == "cancel") {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                return;
            }
            if (count == 1 && mode_ == "oversize") {
                write(connection, QByteArray(65537, 'x'));
                return;
            }
            if (count == 1 && mode_ == "flood") {
                for (int event = 0; event < 33; ++event) {
                    if (!write(connection, "{\"event\":\"FIXTURE\"}\n"))
                        return;
                }
                return;
            }
            QJsonObject reply{{"id", request.value("id")}, {"return", QJsonObject{}}};
            if (count == 1 && mode_ == "mismatch")
                reply.insert("id", "wrong-request");
            if (count == 1 && mode_ == "error") {
                reply.remove("return");
                reply.insert("error", QJsonObject{{"class", "GenericError"}});
            }
            if (count == 1 && mode_ == "events" &&
                !write(connection, "{\"event\":\"ONE\"}\n{\"event\":\"TWO\"}\n"))
                return;
            if (!write(connection, QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n'))
                return;
        }
    }

    QTemporaryDir directory_;
    QString path_;
    QString mode_;
    GSocket *listener_ = nullptr;
    GCancellable *cancel_ = nullptr;
    std::thread worker_;
};
}

class QmpAbsoluteInputTest : public QObject {
    Q_OBJECT
private slots:
    void endpointsAndOriginAcrossSupportedDimensions()
    {
        for (int dimension = 1; dimension <= int(FrameStore::MaximumDimension); ++dimension) {
            QPoint result;
            QVERIFY(QmpAbsoluteInput::normalize({0, 0}, {dimension, dimension}, result));
            QCOMPARE(result, QPoint(1, 1));
            QCOMPARE(result.x() * (dimension - 1) / 32767, 0);
            QCOMPARE((result.x() * (dimension - 1) + 16383) / 32767, 0);
            if (dimension > 1) {
                QVERIFY(QmpAbsoluteInput::normalize({dimension - 1, dimension - 1}, {dimension, dimension}, result));
                QCOMPARE(result, QPoint(32767, 32767));
            }
        }
        QPoint result;
        QVERIFY(!QmpAbsoluteInput::normalize({0, 0}, {4097, 800}, result));
        QVERIFY(!QmpAbsoluteInput::normalize({-1, 0}, {1280, 800}, result));
        QVERIFY(!QmpAbsoluteInput::normalize({1280, 0}, {1280, 800}, result));
        QVERIFY(!QmpAbsoluteInput::normalize({0, 0}, {0, 800}, result));
    }
    void fixedProtocolAndOrderedMoves()
    {
        MonitorFixture fixture("events");
        auto *cancel = g_cancellable_new();
        {
            QmpAbsoluteInput input(cancel);
            QVERIFY(input.connect(fixture.address(), ownProcessId(), ownStartToken()));
            QVERIFY(input.move({0, 0}, {1280, 800}));
            QVERIFY(input.move({1279, 799}, {1280, 800}));
            input.close();
        }
        g_object_unref(cancel);
        // Connection shutdown allows the server to finish before inspecting its receipt.
        fixture.finish();
        QCOMPARE(fixture.requests.size(), 3);
        QCOMPARE(fixture.requests[0].value("execute").toString(), QString("qmp_capabilities"));
        for (int index = 1; index < 3; ++index) {
            const auto request = fixture.requests[index];
            QCOMPARE(request.value("execute").toString(), QString("input-send-event"));
            QCOMPARE(request.value("id").toString(), QString("dockpipe-input-%1").arg(index));
            const auto arguments = request.value("arguments").toObject();
            QCOMPARE(arguments.size(), 1);
            const auto events = arguments.value("events").toArray();
            QCOMPARE(events.size(), 2);
            QCOMPARE(events[0].toObject().value("data").toObject().value("value").toInt(), index == 1 ? 1 : 32767);
            QCOMPARE(events[1].toObject().value("data").toObject().value("value").toInt(), index == 1 ? 1 : 32767);
        }
    }
    void rejectsProtocolFailures_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"mismatch", "error", "oversize", "flood"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void rejectsProtocolFailures()
    {
        QFETCH(QString, mode);
        MonitorFixture fixture(mode);
        auto *cancel = g_cancellable_new();
        QmpAbsoluteInput input(cancel);
        QVERIFY(input.connect(fixture.address(), ownProcessId(), ownStartToken()));
        QVERIFY(!input.move({100, 100}, {1280, 800}));
        // A failed response poisons the stream: no later request may consume a stale ACK.
        QVERIFY(!input.move({200, 200}, {1280, 800}));
        input.close();
        g_object_unref(cancel);
    }
    void responseDeadlineIsOverallBounded()
    {
        MonitorFixture fixture("stall");
        auto *cancel = g_cancellable_new();
        QmpAbsoluteInput input(cancel);
        QVERIFY(input.connect(fixture.address(), ownProcessId(), ownStartToken()));
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!input.move({100, 100}, {1280, 800}));
        QVERIFY(elapsed.elapsed() >= 1800);
        QVERIFY(elapsed.elapsed() < 2400);
        QVERIFY(!input.move({200, 200}, {1280, 800}));
        input.close();
        g_object_unref(cancel);
    }
    void changedProcessIdentityRejectedBeforeConnect()
    {
        MonitorFixture fixture;
        auto *cancel = g_cancellable_new();
        QmpAbsoluteInput input(cancel);
        QVERIFY(!input.connect(fixture.address(), ownProcessId(), "0"));
        QVERIFY(!input.move({100, 100}, {1280, 800}));
        g_object_unref(cancel);
    }
    void cancellationUnblocksOwnedWorker()
    {
        MonitorFixture fixture("cancel");
        auto *cancel = g_cancellable_new();
        QmpAbsoluteInput input(cancel);
        QVERIFY(input.connect(fixture.address(), ownProcessId(), ownStartToken()));
        std::thread cancellation([cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            g_cancellable_cancel(cancel);
        });
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!input.move({100, 100}, {1280, 800}));
        cancellation.join();
        QVERIFY(elapsed.elapsed() < 500);
        input.close();
        g_object_unref(cancel);
    }
};
QTEST_MAIN(QmpAbsoluteInputTest)
#include "QmpAbsoluteInputTest.moc"
