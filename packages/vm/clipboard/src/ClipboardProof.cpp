#include "ClipboardProof.h"
#include "ClipboardBridge.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMimeData>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QUuid>
#include <cstdio>

namespace {
constexpr qsizetype textLimit = 1048576;
constexpr qsizetype wireLimit = 1572864;

QJsonObject exchange(const QString &manager, const QString &resource, const QJsonObject &request) {
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("DOCKPIPE_SURFACE_RESOURCE_ID", resource);
    process.setProcessEnvironment(environment);
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(manager, {"clipboard-exchange"});
    if (!process.waitForStarted(1000))
        return {};
    process.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    process.closeWriteChannel();
    QElapsedTimer clock;
    clock.start();
    QByteArray output;
    while (process.state() != QProcess::NotRunning && clock.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        output += process.readAllStandardOutput();
        if (output.size() > wireLimit)
            break;
        process.waitForFinished(20);
    }
    output += process.readAllStandardOutput();
    if (process.state() != QProcess::NotRunning || output.size() > wireLimit) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    return QJsonDocument::fromJson(output).object();
}

bool readGuest(const QString &manager, const QString &resource, QByteArray &text) {
    const auto response = exchange(manager, resource, {{"schema", 1}, {"operation", "read"}});
    if (response.value("schema").toInt() != 1 || !response.value("success").toBool())
        return false;
    const auto encoded = response.value("text_base64").toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > textLimit || decoded.decoded.toBase64() != encoded)
        return false;
    text = decoded.decoded;
    return true;
}

bool writeGuest(const QString &manager, const QString &resource, const QByteArray &text, const QByteArray &expected) {
    const auto expectedHash = QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex();
    const QJsonObject request{
        {"schema", 1},
        {"operation", "write"},
        {"text_base64", QString::fromLatin1(text.toBase64())},
        {"expected_sha256", QString::fromLatin1(expectedHash)}
    };
    const auto response = exchange(manager, resource, request);
    return response.value("schema").toInt() == 1 && response.value("success").toBool();
}

bool hostText(QByteArray &text, bool &empty) {
    const auto *mime = QGuiApplication::clipboard()->mimeData();
    empty = !mime || mime->formats().isEmpty();
    if (!empty && !mime->hasText())
        return false;
    text = empty ? QByteArray() : mime->text().toUtf8();
    return text.size() <= textLimit;
}
}

int runClipboardProof(const QString &manager, const QString &resource) {
    const auto name = clipboardServerName(qEnvironmentVariable("DOCKPIPE_VM_CLIPBOARD_SCOPE"), resource);
    const auto initialState = clipboardControl(name, "status").value("state").toString();
    bool hostToGuest = false;
    bool guestToHost = false;
    bool hostRestored = false;
    bool guestRestored = false;
    QByteArray originalHost;
    QByteArray originalGuest;
    bool originalHostEmpty = false;
    QJsonObject originalMime;
    qsizetype originalMimeBytes = 0;
    bool mimeSupported = true;
    const auto *originalData = QGuiApplication::clipboard()->mimeData();
    if (originalData) {
        const auto formats = originalData->formats();
        mimeSupported = formats.size() <= 64;
        for (const auto &format : formats) {
            const auto data = originalData->data(format);
            originalMimeBytes += data.size();
            if (format.size() > 256 || originalMimeBytes > textLimit) {
                mimeSupported = false;
                break;
            }
            originalMime.insert(format, QString::fromLatin1(data.toBase64()));
        }
    }
    // Text-only verification refuses unsupported formats before touching either
    // clipboard. Originals stay solely in this process's memory.
    const bool admitted = mimeSupported && hostText(originalHost, originalHostEmpty) && readGuest(manager, resource, originalGuest) &&
        !initialState.isEmpty();
    const auto identity = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
    const auto markerHost = QByteArray("Dockpipe host marker ") + identity + QByteArray::fromStdString(" · 日本語\nUnicode ✓");
    const auto markerGuest = QByteArray("Dockpipe guest marker ") + identity + QByteArray::fromStdString(" · Ελληνικά\nUnicode ✓");
    if (admitted) {
        clipboardControl(name, "resume");
        // Let the worker establish both baselines before generating user-copy
        // events, then keep Qt's native clipboard event loop responsive.
        QElapsedTimer total;
        total.start();
        while (total.elapsed() < 2500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(20);
        }
        QByteArray currentHost;
        QByteArray currentGuest;
        bool currentEmpty = false;
        if (!hostText(currentHost, currentEmpty) || currentHost != originalHost ||
            !readGuest(manager, resource, currentGuest) || currentGuest != originalGuest) {
            const QJsonObject result{
                {"schema", 1},
                {"success", false},
                {"reason", "clipboard-changed-before-proof"}
            };
            const auto output = QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n';
            std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
            return 1;
        }
        QGuiApplication::clipboard()->setText(QString::fromUtf8(markerHost));
        while (total.elapsed() < 35000) {
            QByteArray observed;
            if (readGuest(manager, resource, observed) && observed == markerHost) {
                hostToGuest = true;
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(100);
        }
        if (hostToGuest && writeGuest(manager, resource, markerGuest, markerHost)) {
            while (total.elapsed() < 65000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QByteArray observed;
                bool empty = false;
                if (hostText(observed, empty) && observed == markerGuest) {
                    guestToHost = true;
                    break;
                }
                QThread::msleep(30);
            }
        }
        // Pause completes any in-flight write before returning. Restore only
        // generated markers: a newer user copy always wins over the test.
        const auto paused = clipboardControl(name, "pause", 10000);
        if (!paused.isEmpty()) {
            QByteArray observedGuest;
            if (readGuest(manager, resource, observedGuest) && (observedGuest == markerGuest || observedGuest == markerHost))
                guestRestored = writeGuest(manager, resource, originalGuest, observedGuest);
            QByteArray observedHost;
            bool empty = false;
            if (hostText(observedHost, empty) && (observedHost == markerGuest || observedHost == markerHost)) {
                const auto restored = clipboardControlRequest(name, {{"schema", 1}, {"operation", "restore-host"},
                    {"expected_base64", QString::fromLatin1(observedHost.toBase64())},
                    {"mime_data", originalMime}, {"clear", originalHostEmpty}}, 10000);
                hostRestored = restored.value("restored").toBool();
            }
        }
        if (initialState != "paused" && initialState != "stopped")
            clipboardControl(name, "resume");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
    const bool success = admitted && hostToGuest && guestToHost && hostRestored && guestRestored;
    const auto output = QJsonDocument(QJsonObject{{"schema", 1}, {"success", success},
        {"host_to_guest", hostToGuest}, {"guest_to_host", guestToHost},
        {"host_restored", hostRestored}, {"guest_restored", guestRestored}}).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
    return success ? 0 : 1;
}
