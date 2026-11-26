#include "ProcessChannel.h"
#include "../native/ClipboardProtocol.h"
#include "../native/AssetClipboardProtocol.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using dockpipe::desktop::ProcessChannel;

namespace {
QByteArray frame(const QByteArray &payload)
{
    QByteArray result(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), result.data());
    result.append(payload);
    return result;
}

bool writeAll(int descriptor, const QByteArray &value)
{
    qsizetype offset = 0;
    while (offset < value.size()) {
        const auto written = ::write(descriptor, value.constData() + offset,
                                     static_cast<std::size_t>(value.size() - offset));
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        offset += written;
    }
    return true;
}

void recordPID(const QString &path, pid_t pid)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(QByteArray::number(pid));
}

int fixtureChild(int argc, char **argv)
{
    if (argc != 8 || QByteArray(argv[1]) != "connect" || QByteArray(argv[2]) != "--config"
        || QByteArray(argv[4]) != "--control-fd" || QByteArray(argv[5]) != "3"
        || QByteArray(argv[6]) != "--status-fd" || QByteArray(argv[7]) != "4")
        return 101;
    struct stat control {};
    struct stat status {};
    if (::fstat(3, &control) < 0 || !S_ISSOCK(control.st_mode)
        || ::fstat(4, &status) < 0 || !S_ISFIFO(status.st_mode)
        || (::fcntl(3, F_GETFD) & FD_CLOEXEC) || (::fcntl(4, F_GETFD) & FD_CLOEXEC)
        || ::getsid(0) != ::getpid() || ::getpgrp() != ::getpid())
        return 102;
    QFile input(QString::fromLocal8Bit(argv[3]));
    if (!input.open(QIODevice::ReadOnly))
        return 103;
    const auto configuration = QJsonDocument::fromJson(input.readAll()).object();
    const QString mode = configuration.value("fixture").toString();
    const QString root = configuration.value("root").toString();
    recordPID(root + "/helper.pid", ::getpid());
    ::signal(SIGALRM, SIG_DFL);
    ::alarm(15);

    QJsonObject receipt{{"type", "ready"}, {"reference", configuration.value("reference")},
                        {"role", configuration.value("role")}, {"mode", configuration.value("mode")},
                        {"capture", QJsonObject{{"quality", "high"}, {"frame_rate", 30}}}};
    if (configuration.contains("capture"))
        receipt.insert("capture", configuration.value("capture"));
    const QByteArray packet = frame(QByteArray(1, '\1') + "fixture-frame");
    if (mode == "data-before-status") {
        if (!writeAll(STDOUT_FILENO, packet))
            return 104;
        ::usleep(150000);
    } else if (mode == "wrong-reference") {
        auto reference = receipt.value("reference").toObject();
        reference.insert("id", QString(64, QLatin1Char('f')));
        receipt.insert("reference", reference);
    } else if (mode == "wrong-role") {
        receipt.insert("role", "worker");
    } else if (mode == "wrong-mode") {
        receipt.insert("mode", "view");
    } else if (mode == "wrong-capture") {
        receipt.insert("capture", QJsonObject{{"quality", "low"}, {"frame_rate", 10}});
    } else if (mode == "legacy-ready") {
        receipt.remove("capture");
    } else if (mode == "extra-capture") {
        receipt.insert("capture", QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"width", 3840}});
    } else if (mode == "truncated-status") {
        writeAll(4, frame("incomplete").left(7));
        ::close(4);
        return 0;
    } else if (mode == "oversized-status") {
        QByteArray header(4, Qt::Uninitialized);
        qToBigEndian<quint32>(4097, header.data());
        writeAll(4, header);
        ::close(4);
        return 0;
    } else if (mode == "broken-control") {
        ::close(3);
    } else if (mode == "stderr-flood") {
        const QByteArray discarded(65536, 'x');
        for (int index = 0; index < 256; ++index) {
            if (!writeAll(STDERR_FILENO, discarded))
                return 105;
        }
    }
    if (mode.startsWith("wrong-"))
        writeAll(STDOUT_FILENO, packet);
    if (!writeAll(4, frame(QJsonDocument(receipt).toJson(QJsonDocument::Compact))))
        return 106;
    ::close(4);
    if (mode == "oversized-packet") {
        QByteArray header(4, Qt::Uninitialized);
        qToBigEndian<quint32>(2 * 1024 * 1024 + 2, header.data());
        writeAll(STDOUT_FILENO, header);
    } else if (mode == "truncated-packet") {
        writeAll(STDOUT_FILENO, frame(QByteArray(1, '\1') + "truncated").left(7));
        return 0;
    } else if (mode != "data-before-status" && !mode.startsWith("wrong-") && !mode.startsWith("clipboard")) {
        writeAll(STDOUT_FILENO, packet);
    }
    if (mode == "hung-group") {
        ::signal(SIGTERM, SIG_IGN);
        const pid_t child = ::fork();
        if (child < 0)
            return 107;
        if (child == 0) {
            recordPID(root + "/descendant.pid", ::getpid());
            ::alarm(15);
            for (;;)
                ::pause();
        }
        for (;;)
            ::pause();
    }
    if (mode == "broken-control") {
        for (;;)
            ::pause();
    }
    if (mode.startsWith("clipboard")) {
        using namespace dockpipe::desktop;
        const auto operation = mode == "clipboard-wrong-direction" ? ClipboardOperation::Write : ClipboardOperation::Changed;
        const auto encoded = encodeClipboardRecord({operation, 1, 1, "fixture"});
        writeAll(STDOUT_FILENO, frame(QByteArray(1, '\4') + QByteArray(reinterpret_cast<const char *>(encoded.data()), encoded.size())));
    }
    QByteArray authority;
    int renewals = 0;
    char buffer[1024];
    for (;;) {
        const auto received = ::read(3, buffer, sizeof(buffer));
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
            return 0;
        if (mode != "lease")
            continue;
        authority.append(buffer, static_cast<qsizetype>(received));
        if (authority.size() > 4096)
            return 108;
        while (authority.size() >= 4) {
            const auto length = qFromBigEndian<quint32>(authority.constData());
            if (!length || length > 1024)
                return 109;
            if (authority.size() < length + 4)
                break;
            const auto renewal = QJsonDocument::fromJson(authority.mid(4, length)).object();
            if (renewal.size() != 2 || renewal.value("type") != "lease"
                || !QDateTime::fromString(renewal.value("expires_at").toString(), Qt::ISODateWithMs).isValid())
                return 110;
            authority.remove(0, length + 4);
            ++renewals;
            if (!writeAll(STDOUT_FILENO, frame(QByteArray(1, '\3') + QByteArray::number(renewals))))
                return 111;
        }
    }
}

QJsonObject reference()
{
    return {{"schema", "dockpipe.remote.desktop/v1"}, {"id", QString(64, QLatin1Char('a'))},
            {"epoch", QString(64, QLatin1Char('b'))},
            {"target", QJsonObject{{"node", "fixture"}, {"fingerprint", QString(64, QLatin1Char('c'))},
                                   {"generation", QString(64, QLatin1Char('d'))}}}};
}

QString configuration(QTemporaryDir &directory, const QString &fixture, const QString &mode = "control")
{
    const QString path = directory.filePath("config.json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(QJsonObject{{"fixture", fixture}, {"root", directory.path()},
        {"reference", reference()}, {"role", "operator"}, {"mode", mode}}).toJson(QJsonDocument::Compact));
    return path;
}

pid_t recordedPID(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    return static_cast<pid_t>(file.readAll().toLongLong());
}
}

class ProcessChannelTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    }

    void richClipboardKeepsMaximumChunkAndInputBudgetIndependent()
    {
        using namespace dockpipe::desktop;
        QTemporaryDir directory;
        const auto path = configuration(directory, "data-before-status");
        QFile config(path);
        QVERIFY(config.open(QIODevice::ReadOnly));
        auto value = QJsonDocument::fromJson(config.readAll()).object();
        config.close();
        value.insert("role", "viewer");
        const QJsonObject capture{{"quality", "high"}, {"frame_rate", 30},
            {"clipboard_text", true}, {"clipboard_files", true}};
        value.insert("capture", capture);
        QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
        config.write(QJsonDocument(value).toJson());
        config.close();
        ProcessChannel channel;
        QSignalSpy connected(&channel, &ProcessChannel::connected);
        channel.start(QCoreApplication::applicationFilePath(), path, reference(), "viewer", "control", capture);
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 2000);
        AssetRecord record;
        record.operation = AssetOperation::Chunk;
        record.epoch = 1;
        record.serial = 1;
        record.transfer.fill(7);
        record.item = 1;
        record.data.assign(AssetClipboardMaximumChunkBytes, 42);
        const auto chunk = encodeAssetClipboardRecord(record);
        const QByteArray bytes(reinterpret_cast<const char *>(chunk.data()), qsizetype(chunk.size()));
        QVERIFY(channel.send(5, bytes));
        QVERIFY(!channel.send(5, bytes)); // only one asset chunk can enter QProcess
        const auto text = encodeClipboardRecord({ClipboardOperation::Write, 1, 1,
            std::string(ClipboardMaximumTextBytes, 'x')});
        QVERIFY(channel.send(4, QByteArray(reinterpret_cast<const char *>(text.data()), qsizetype(text.size()))));
        QVERIFY(channel.send(2, QByteArray(4096, 'x'))); // original input budget still available
        AssetRecord wrong;
        wrong.operation = AssetOperation::EpochAcknowledged;
        wrong.control = AssetOperation::Arm;
        wrong.epoch = 1;
        const auto encoded = encodeAssetClipboardRecord(wrong);
        QVERIFY(!channel.send(5, QByteArray(reinterpret_cast<const char *>(encoded.data()), qsizetype(encoded.size()))));
        channel.close();
        ProcessChannel view;
        QSignalSpy failures(&view, &ProcessChannel::failed);
        view.start(QCoreApplication::applicationFilePath(), path, reference(), "viewer", "view", capture);
        QCOMPARE(failures.count(), 1);
    }

    void clipboardAdmissionAndMaximumFrame()
    {
        using namespace dockpipe::desktop;
        QTemporaryDir directory;
        const auto path = configuration(directory, "clipboard");
        QFile config(path);
        QVERIFY(config.open(QIODevice::ReadOnly));
        auto value = QJsonDocument::fromJson(config.readAll()).object();
        config.close();
        value.insert("role", "viewer");
        const QJsonObject capture{{"quality", "high"}, {"frame_rate", 30}, {"clipboard_text", true}};
        value.insert("capture", capture);
        QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
        config.write(QJsonDocument(value).toJson());
        config.close();
        ProcessChannel channel;
        QSignalSpy connected(&channel, &ProcessChannel::connected);
        QSignalSpy packets(&channel, &ProcessChannel::packet);
        QSignalSpy failures(&channel, &ProcessChannel::failed);
        channel.start(QCoreApplication::applicationFilePath(), path, reference(), "viewer", "control", capture);
        QTRY_COMPARE(connected.count(), 1);
        QTRY_COMPARE(packets.count(), 1);
        QCOMPARE(packets.first().first().value<quint8>(), quint8(4));
        const auto maximum = encodeClipboardRecord({ClipboardOperation::Write, 1, 1, std::string(ClipboardMaximumTextBytes, 'x')});
        QVERIFY(channel.send(4, QByteArray(reinterpret_cast<const char *>(maximum.data()), maximum.size())));
        const auto wrong = encodeClipboardRecord({ClipboardOperation::Changed, 1, 1, "text"});
        QVERIFY(!channel.send(4, QByteArray(reinterpret_cast<const char *>(wrong.data()), wrong.size())));
        QVERIFY(!channel.send(4, QByteArray(18, '\0')));
        QCOMPARE(failures.count(), 0);
        channel.close();
        ProcessChannel view;
        QSignalSpy viewFailures(&view, &ProcessChannel::failed);
        view.start(QCoreApplication::applicationFilePath(), path, reference(), "viewer", "view", capture);
        QCOMPARE(viewFailures.count(), 1);
        QVERIFY(!view.ready());
    }

    void clipboardUnnegotiatedAndWrongDirectionFailClosed_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<bool>("enabled");
        QTest::newRow("not-negotiated") << QString("clipboard") << false;
        QTest::newRow("wrong-direction") << QString("clipboard-wrong-direction") << true;
    }

    void clipboardUnnegotiatedAndWrongDirectionFailClosed()
    {
        QFETCH(QString, fixture);
        QFETCH(bool, enabled);
        QTemporaryDir directory;
        const auto path = configuration(directory, fixture);
        QFile config(path);
        QVERIFY(config.open(QIODevice::ReadOnly));
        auto value = QJsonDocument::fromJson(config.readAll()).object();
        config.close();
        value.insert("role", "viewer");
        QJsonObject capture{{"quality", "high"}, {"frame_rate", 30}};
        if (enabled)
            capture.insert("clipboard_text", true);
        value.insert("capture", capture);
        QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
        config.write(QJsonDocument(value).toJson());
        config.close();
        ProcessChannel channel;
        QSignalSpy packets(&channel, &ProcessChannel::packet);
        QSignalSpy failures(&channel, &ProcessChannel::failed);
        channel.start(QCoreApplication::applicationFilePath(), path, reference(), "viewer", "control", capture);
        QTRY_COMPARE(failures.count(), 1);
        QCOMPARE(packets.count(), 0);
    }

    void dataBeforeVerifiedStatusIsBuffered()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ProcessChannel channel;
        QSignalSpy connected(&channel, &ProcessChannel::connected);
        QSignalSpy packets(&channel, &ProcessChannel::packet);
        QSignalSpy failures(&channel, &ProcessChannel::failed);
        channel.start(QCoreApplication::applicationFilePath(), configuration(directory, "data-before-status"),
                      reference(), "operator", "control");
        QTest::qWait(60);
        QCOMPARE(connected.count(), 0);
        QCOMPARE(packets.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(packets.count(), 1, 1500);
        QCOMPARE(packets.first().at(0).value<quint8>(), quint8(1));
        QCOMPARE(packets.first().at(1).toByteArray(), QByteArray("fixture-frame"));
        QCOMPARE(failures.count(), 0);
        channel.close();
        QVERIFY(!channel.ready());
    }

    void invalidReceiptsAndPacketsFailClosed_data()
    {
        QTest::addColumn<QString>("fixture");
        for (const auto *mode : {"wrong-reference", "wrong-role", "wrong-mode", "wrong-capture", "legacy-ready", "extra-capture", "truncated-status",
                                 "oversized-status", "oversized-packet", "truncated-packet"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void invalidReceiptsAndPacketsFailClosed()
    {
        QFETCH(QString, fixture);
        QTemporaryDir directory;
        ProcessChannel channel;
        QSignalSpy packets(&channel, &ProcessChannel::packet);
        QSignalSpy failures(&channel, &ProcessChannel::failed);
        channel.start(QCoreApplication::applicationFilePath(), configuration(directory, fixture),
                      reference(), "operator", "control");
        QTRY_COMPARE_WITH_TIMEOUT(failures.count(), 1, 2000);
        QCOMPARE(packets.count(), 0);
        QVERIFY(!channel.ready());
        auto *process = channel.findChild<QProcess *>();
        QVERIFY(process);
        QCOMPARE(process->state(), QProcess::NotRunning);
    }

    void repeatedLeaseRenewalsAfterReadinessKeepTheAuthoritySocket()
    {
        QTemporaryDir directory;
        ProcessChannel channel;
        QSignalSpy packets(&channel, &ProcessChannel::packet);
        QSignalSpy failures(&channel, &ProcessChannel::failed);
        channel.start(QCoreApplication::applicationFilePath(), configuration(directory, "lease"),
                      reference(), "operator", "control");
        QTRY_COMPARE_WITH_TIMEOUT(packets.count(), 1, 1500);
        for (int index = 1; index <= 3; ++index) {
            channel.renew(QDateTime::currentDateTimeUtc().addSecs(20 + index));
            QTRY_COMPARE_WITH_TIMEOUT(packets.count(), index + 1, 1500);
            QCOMPARE(packets.last().at(0).value<quint8>(), quint8(3));
            QCOMPARE(packets.last().at(1).toByteArray(), QByteArray::number(index));
        }
        QCOMPARE(failures.count(), 0);
        QVERIFY(channel.ready());
        channel.close();
    }

    void brokenAuthoritySocketDoesNotRaiseSIGPIPE()
    {
        QTemporaryDir directory;
        ProcessChannel channel;
        QSignalSpy packets(&channel, &ProcessChannel::packet);
        QSignalSpy failures(&channel, &ProcessChannel::failed);
        channel.start(QCoreApplication::applicationFilePath(), configuration(directory, "broken-control"),
                      reference(), "operator", "control");
        QTRY_COMPARE_WITH_TIMEOUT(packets.count(), 1, 1500);
        channel.renew(QDateTime::currentDateTimeUtc().addSecs(20));
        QCOMPARE(failures.count(), 1);
        QVERIFY(!channel.ready());
        QCOMPARE(channel.findChild<QProcess *>()->state(), QProcess::NotRunning);
    }

    void queuedInputRespectsTheWholeRecordBound()
    {
        QTemporaryDir directory;
        ProcessChannel channel;
        QSignalSpy connected(&channel, &ProcessChannel::connected);
        QVERIFY(!channel.send(2, "before-ready"));
        channel.start(QCoreApplication::applicationFilePath(), configuration(directory, "normal"),
                      reference(), "operator", "control");
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 1500);
        const QByteArray input(4096, 'x');
        int accepted = 0;
        while (accepted < 100 && channel.send(2, input))
            ++accepted;
        QCOMPARE(accepted, 7);
        QVERIFY(!channel.send(2, QByteArray(4097, 'x')));
        QVERIFY(!channel.send(3, "invalid-keyframe"));
        QVERIFY(!channel.send(1, "operator-media"));
        channel.close();
    }

    void viewOnlyRejectsInputButAllowsKeyframeRequest()
    {
        QTemporaryDir directory;
        ProcessChannel channel;
        QSignalSpy connected(&channel, &ProcessChannel::connected);
        channel.start(QCoreApplication::applicationFilePath(), configuration(directory, "normal", "view"),
                      reference(), "operator", "view");
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 1500);
        QVERIFY(!channel.send(2, "physical-key"));
        QVERIFY(channel.send(3, QByteArray(1, '\1')));
        channel.close();
    }

    void helperDiagnosticsAreDiscardedWithoutBuffering()
    {
        QTemporaryDir directory;
        ProcessChannel channel;
        QSignalSpy connected(&channel, &ProcessChannel::connected);
        channel.start(QCoreApplication::applicationFilePath(), configuration(directory, "stderr-flood"),
                      reference(), "operator", "control");
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 1500);
        QCOMPARE(channel.findChild<QProcess *>()->readAllStandardError(), QByteArray());
        channel.close();
    }

    void hungHelperAndRelayGroupAreKilledAndReaped()
    {
        QTemporaryDir directory;
        ProcessChannel channel;
        QSignalSpy connected(&channel, &ProcessChannel::connected);
        channel.start(QCoreApplication::applicationFilePath(), configuration(directory, "hung-group"),
                      reference(), "operator", "control");
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 1500);
        const QString descendantPath = directory.filePath("descendant.pid");
        QTRY_VERIFY_WITH_TIMEOUT(recordedPID(descendantPath) > 0, 1500);
        const pid_t helper = recordedPID(directory.filePath("helper.pid"));
        const pid_t descendant = recordedPID(descendantPath);
        QVERIFY(helper > 0 && descendant > 0);
        QElapsedTimer elapsed;
        elapsed.start();
        channel.close();
        QVERIFY2(elapsed.elapsed() < 5000, "Owned hung process teardown exceeded its bound");
        QVERIFY(elapsed.elapsed() >= 2900);
        QCOMPARE(channel.findChild<QProcess *>()->state(), QProcess::NotRunning);
        int status = 0;
        pid_t reaped = 0;
        const auto reapDescendant = [&] {
            if (reaped == descendant)
                return true;
            reaped = ::waitpid(descendant, &status, WNOHANG);
            return reaped == descendant;
        };
        QTRY_VERIFY_WITH_TIMEOUT(reapDescendant(), 1500);
        QVERIFY(WIFSIGNALED(status));
        QCOMPARE(WTERMSIG(status), SIGKILL);
        errno = 0;
        QCOMPARE(::waitpid(helper, &status, WNOHANG), pid_t(-1));
        QCOMPARE(errno, ECHILD);
    }

    void closeWhileStartingReleasesTheOwnedChild()
    {
        QTemporaryDir directory;
        ProcessChannel channel;
        channel.start(QCoreApplication::applicationFilePath(), configuration(directory, "normal"),
                      reference(), "operator", "control");
        channel.close();
        QCOMPARE(channel.findChild<QProcess *>()->state(), QProcess::NotRunning);
        QVERIFY(!channel.ready());
    }
};

int main(int argc, char **argv)
{
    if (argc > 1 && QByteArray(argv[1]) == "connect")
        return fixtureChild(argc, argv);
    QCoreApplication application(argc, argv);
    ProcessChannelTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ProcessChannelTest.moc"
