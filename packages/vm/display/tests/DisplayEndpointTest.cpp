#include "DisplayEndpoint.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QtTest>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc_info.h>
#endif

class DisplayEndpointTest : public QObject {
    Q_OBJECT
private slots:
    void privateMetadataAndProcessIdentity()
    {
        // Unix-domain sockets have a small fixed path capacity. The runner's
        // ambient TMPDIR may be longer than that capacity on macOS.
        QTemporaryDir temporary(QStringLiteral("/tmp/dockpipe-endpoint-XXXXXX"));
        QVERIFY(temporary.isValid());
        QVERIFY(QFile::setPermissions(temporary.path(),
                                     QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto state = QFileInfo(temporary.path()).canonicalFilePath() + "/display-fixture";
        QVERIFY(QDir().mkdir(state));
        QVERIFY(QFile::setPermissions(state, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
#ifdef Q_OS_MACOS
        struct proc_bsdinfo info{};
        QCOMPARE(proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &info, sizeof(info)), int(sizeof(info)));
        const auto token = QString::number(info.pbi_start_tvsec) + ":" + QString::number(info.pbi_start_tvusec);
#else
        QFile stat(QStringLiteral("/proc/%1/stat").arg(getpid()));
        QVERIFY(stat.open(QIODevice::ReadOnly));
        const auto bytes = stat.readAll();
        const auto fields = bytes.mid(bytes.lastIndexOf(')') + 2).simplified().split(' ');
        const auto token = QString::fromLatin1(fields[19]);
#endif
        QJsonObject descriptor{{"schema", 1}, {"resource_id", "fixture"},
            {"backend", "qemu-dbus"}, {"bus_address", "unix:path=/tmp/unused"},
            {"console_id", 0}, {"qemu_pid", int(getpid())}, {"qemu_start_token", token}};
        const auto path = state + "/display.json";
        auto write = [&] {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            file.write(QJsonDocument(descriptor).toJson());
            file.close();
            return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
        };
        QVERIFY(write());
        QJsonObject options{{"schema", 1}, {"resource_id", "fixture"}, {"state_ref", state}};
        DisplayEndpoint endpoint;
        QString error;
        QVERIFY2(DisplayEndpoint::load(options, endpoint, error), qPrintable(error));
        QVERIFY(DisplayEndpoint::processMatches(getpid(), token));
        QVERIFY(!DisplayEndpoint::processMatches(getpid(), "0"));
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther));
        QVERIFY(!DisplayEndpoint::load(options, endpoint, error));
        QVERIFY(write());
        descriptor.insert("qemu_start_token", "0");
        QVERIFY(write());
        QVERIFY(!DisplayEndpoint::load(options, endpoint, error));
        descriptor.insert("qemu_start_token", token);
        descriptor.insert("schema", 2);
        descriptor.insert("bus_address", "unix:path=" + state + "/bus");
        descriptor.insert("input_address", "unix:path=" + state + "/input");
        const int inputSocket = socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(inputSocket >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto encodedPath = (state + "/input").toUtf8();
        QVERIFY(encodedPath.size() < qsizetype(sizeof(address.sun_path)));
        std::memcpy(address.sun_path, encodedPath.constData(), size_t(encodedPath.size() + 1));
        const int bound = bind(inputSocket, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        QCOMPARE(bound, 0);
        QVERIFY(write());
        QVERIFY(DisplayEndpoint::load(options, endpoint, error));
        QCOMPARE(endpoint.inputAddress, "unix:path=" + state + "/input");
        descriptor.insert("input_address", "unix:path=" + state + "/bus");
        QVERIFY(write());
        QVERIFY(!DisplayEndpoint::load(options, endpoint, error));
        descriptor.insert("schema", 1);
        descriptor.insert("input_address", "");
        QVERIFY(write());
        QVERIFY(!DisplayEndpoint::load(options, endpoint, error));
        ::close(inputSocket);
        descriptor.insert("unexpected", true);
        QVERIFY(write());
        QVERIFY(!DisplayEndpoint::load(options, endpoint, error));
    }
};
QTEST_MAIN(DisplayEndpointTest)
#include "DisplayEndpointTest.moc"
