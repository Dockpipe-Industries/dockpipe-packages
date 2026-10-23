#include "ClipboardBridge.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMimeData>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringDecoder>
#include <memory>

namespace {
constexpr qsizetype textLimit = 1048576;
constexpr qsizetype wireLimit = 1572864;
QByteArray digest(const QByteArray &text) {
    return QCryptographicHash::hash(text, QCryptographicHash::Sha256);
}
}

QString clipboardServerName(const QString &scope, const QString &resource) {
    const auto identity = digest((scope + '\n' + resource).toUtf8()).toHex().left(32);
    return QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
        .filePath(QStringLiteral("dockpipe-vm-clipboard-v1-") + QString::fromLatin1(identity));
}

QJsonObject clipboardControl(const QString &name, const QString &operation, int timeout) {
    return clipboardControlRequest(name, {{"schema", 1}, {"operation", operation}}, timeout);
}

QJsonObject clipboardControlRequest(const QString &name, const QJsonObject &request, int timeout) {
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(timeout))
        return {};
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(timeout))
        return {};
    QByteArray response;
    QElapsedTimer clock;
    clock.start();
    while (!response.contains('\n') && clock.elapsed()<timeout) {
        response += socket.read(4097-response.size());
        if (response.size() > 4096)
            return {};
        if (response.contains('\n'))
            break;
        if (!socket.waitForReadyRead(timeout-static_cast<int>(clock.elapsed())))
            return {};
    }
    return QJsonDocument::fromJson(response).object();
}

ClipboardBridge::ClipboardBridge(QString manager, QString resource)
    : manager_(std::move(manager)), resource_(std::move(resource)),
      clipboard_(QGuiApplication::clipboard()), lock_(clipboardServerName(qEnvironmentVariable("DOCKPIPE_VM_CLIPBOARD_SCOPE"), resource_) + ".lock") {
    timer_.setInterval(800);
    deadline_.setSingleShot(true);
    deadline_.setInterval(10000);
    connect(&timer_, &QTimer::timeout, this, &ClipboardBridge::cycle);
    connect(clipboard_, &QClipboard::dataChanged, this, [this] {
        if (!applying_)
            ++hostGeneration_;
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        output_ += process_.readAllStandardOutput();
        if (output_.size() > wireLimit) {
            process_.kill();
            pause(QStringLiteral("paused"));
        }
    });
    connect(&process_, &QProcess::finished, this, [this] { completed(); });
    connect(&process_, &QProcess::errorOccurred, this, [this] { pause(QStringLiteral("paused")); });
    connect(&deadline_, &QTimer::timeout, this, [this] {
        process_.kill();
        pause(QStringLiteral("paused"));
    });
    connect(&server_, &QLocalServer::newConnection, this, &ClipboardBridge::acceptControls);
}

bool ClipboardBridge::start() {
    lock_.setStaleLockTime(0);
    if (!lock_.tryLock(0))
        return false;
    const auto name = clipboardServerName(qEnvironmentVariable("DOCKPIPE_VM_CLIPBOARD_SCOPE"), resource_);
    QLocalServer::removeServer(name);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    server_.setMaxPendingConnections(4);
    if (!server_.listen(name))
        return false;
    timer_.start();
    QTimer::singleShot(0, this, &ClipboardBridge::cycle);
    return true;
}

QByteArray ClipboardBridge::hostText(bool &available) const {
    const auto *mime = clipboard_->mimeData();
    available = mime && mime->hasText();
    if (!available)
        return {};
    auto text = mime->text().toUtf8();
    if (text.size() > textLimit || text.contains('\0')) {
        available = false;
        return {};
    }
    return text;
}

void ClipboardBridge::pause(const QString &state) {
    state_ = state;
    timer_.stop();
    deadline_.stop();
}

void ClipboardBridge::cycle() {
    if (process_.state() != QProcess::NotRunning || state_ == "paused" || state_ == "stopped")
        return;
    exchange({{"schema", 1}, {"operation", "read"}});
}

void ClipboardBridge::exchange(const QJsonObject &request, bool writing) {
    output_.clear();
    writing_ = writing;
    process_.setProgram(manager_);
    process_.setArguments({QStringLiteral("clipboard-exchange")});
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("DOCKPIPE_SURFACE_RESOURCE_ID"), resource_);
    process_.setProcessEnvironment(environment);
    process_.setStandardErrorFile(QProcess::nullDevice());
    deadline_.start();
    process_.start();
    if (!process_.waitForStarted(1000)) {
        pause(QStringLiteral("paused"));
        return;
    }
    const auto payload = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (payload.size() > wireLimit || process_.write(payload) != payload.size()) {
        process_.kill();
        pause(QStringLiteral("paused"));
        return;
    }
    process_.closeWriteChannel();
}

void ClipboardBridge::completed() {
    deadline_.stop();
    output_ += process_.readAllStandardOutput();
    if (state_ == "paused" || state_ == "stopped")
        return;
    if (output_.size() > wireLimit) {
        pause(QStringLiteral("paused"));
        return;
    }
    const auto response = QJsonDocument::fromJson(output_).object();
    if (response.value("schema").toInt() != 1 || !response.value("success").isBool()) {
        pause(QStringLiteral("paused"));
        return;
    }
    if (writing_) {
        if (!response.value("success").toBool()) {
            if (response.value("error").toString() == "conflict") {
                state_ = QStringLiteral("conflict");
                initialized_ = false;
                return;
            }
            pause(QStringLiteral("paused"));
            return;
        }
        lastGuest_ = sentHash_;
        state_ = QStringLiteral("active");
        return;
    }
    const bool guestAvailable = response.value("success").toBool();
    QByteArray guest;
    if (guestAvailable) {
        const auto encoded = response.value("text_base64").toString().toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        QStringDecoder decoder(QStringDecoder::Utf8);
        if (!decoded || decoded.decoded.size() > textLimit || decoded.decoded.toBase64() != encoded) {
            pause(QStringLiteral("paused"));
            return;
        }
        guest = decoded.decoded;
        const auto text = decoder(guest);
        if (decoder.hasError()) {
            pause(QStringLiteral("paused"));
            return;
        }
    } else if (response.value("error").toString() != "unsupported-text") {
        pause(QStringLiteral("paused"));
        return;
    }
    bool hostAvailable = false;
    const auto host = hostText(hostAvailable);
    const auto hostHash = hostAvailable ? digest(host) : QByteArray("unsupported");
    const auto guestHash = guestAvailable ? digest(guest) : QByteArray("unsupported");
    if (!initialized_) {
        initialized_ = true;
        lastHost_ = hostHash;
        lastGuest_ = guestHash;
        lastGeneration_ = hostGeneration_;
        return;
    }
    const bool hostChanged = hostHash != lastHost_ || hostGeneration_ != lastGeneration_;
    const bool guestChanged = guestHash != lastGuest_;
    lastHost_ = hostHash;
    lastGuest_ = guestHash;
    lastGeneration_ = hostGeneration_;
    if (hostChanged && guestChanged && hostHash != guestHash) {
        state_ = QStringLiteral("conflict");
        return;
    }
    if (hostChanged && hostAvailable) {
        sentHash_ = hostHash;
        exchange({{"schema", 1}, {"operation", "write"}, {"text_base64", QString::fromLatin1(host.toBase64())},
                  {"expected_sha256", guestAvailable ? QString::fromLatin1(guestHash.toHex()) : QStringLiteral("unsupported-text")}}, true);
    } else if (guestChanged && guestAvailable) {
        applying_ = true;
        clipboard_->setText(QString::fromUtf8(guest));
        applying_ = false;
        lastHost_ = guestHash;
        lastGeneration_ = hostGeneration_;
        state_ = QStringLiteral("active");
    } else {
        state_ = hostAvailable && guestAvailable ? QStringLiteral("active") : QStringLiteral("unsupported-text");
    }
}

void ClipboardBridge::acceptControls() {
    while (auto *socket = server_.nextPendingConnection()) {
        if (connections_ >= 4) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        ++connections_;
        auto *buffer = new QByteArray;
        connect(socket, &QLocalSocket::disconnected, socket, [this, socket, buffer] {
            --connections_;
            delete buffer;
            socket->deleteLater();
        });
        QTimer::singleShot(1500, socket, [socket] { socket->disconnectFromServer(); });
        connect(socket, &QLocalSocket::readyRead, socket, [this, socket, buffer] {
            buffer->append(socket->read(wireLimit+1));
            if (buffer->size() > wireLimit) {
                socket->abort();
                return;
            }
            if (!buffer->contains('\n'))
                return;
            const auto request = QJsonDocument::fromJson(*buffer).object();
            const auto operation = request.value("operation").toString();
            if (request.value("schema").toInt() != 1)
                return socket->abort();
            bool restored = false;
            if (operation == "restore-host" && state_ == "paused") {
                const auto encoded = request.value("text_base64").toString().toLatin1();
                const auto expected = request.value("expected_base64").toString().toLatin1();
                const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
                const auto expectedDecoded = QByteArray::fromBase64Encoding(expected, QByteArray::AbortOnBase64DecodingErrors);
                QStringDecoder decoder(QStringDecoder::Utf8);
                bool available = false;
                const auto current = hostText(available);
                if (decoded && expectedDecoded && decoded.decoded.size() <= textLimit && expectedDecoded.decoded.size() <= 4096 &&
                    decoded.decoded.toBase64() == encoded && expectedDecoded.decoded.toBase64() == expected && available && current == expectedDecoded.decoded) {
                    const auto text = decoder(decoded.decoded);
                    if (!decoder.hasError()) {
                        applying_ = true;
                        if (request.value("clear").toBool()) {
                            clipboard_->clear();
                        } else if (request.value("mime_data").isObject()) {
                            auto mime = std::make_unique<QMimeData>();
                            qsizetype total = 0;
                            const auto values = request.value("mime_data").toObject();
                            bool valid = values.size() <= 64;
                            for (auto entry = values.begin(); entry != values.end() && valid; ++entry) {
                                const auto base64 = entry.value().toString().toLatin1();
                                const auto item = QByteArray::fromBase64Encoding(base64, QByteArray::AbortOnBase64DecodingErrors);
                                if (!item || item.decoded.toBase64() != base64) {
                                    valid = false;
                                    break;
                                }
                                total += item.decoded.size();
                                valid = entry.key().size() <= 256 && total <= textLimit;
                                if (valid)
                                    mime->setData(entry.key(), item.decoded);
                            }
                            if (valid) {
                                clipboard_->setMimeData(mime.release());
                            } else {
                                applying_ = false;
                                socket->abort();
                                return;
                            }
                        } else {
                            clipboard_->setText(text);
                        }
                        applying_ = false;
                        restored = true;
                    }
                }
            } else if (operation == "resume") {
                initialized_ = false;
                state_ = QStringLiteral("active");
                timer_.start();
            } else if (operation == "pause") {
                pause(QStringLiteral("paused"));
                if (process_.state() != QProcess::NotRunning && !process_.waitForFinished(9000)) {
                    process_.kill();
                    process_.waitForFinished(500);
                }
            } else if (operation == "stop") {
                pause(QStringLiteral("stopped"));
                process_.kill();
                QTimer::singleShot(100, qApp, &QCoreApplication::quit);
            } else if (operation != "status") {
                return socket->abort();
            }
            socket->write(QJsonDocument(QJsonObject{{"schema", 1}, {"state", state_}, {"restored", restored}}).toJson(QJsonDocument::Compact) + '\n');
            socket->disconnectFromServer();
        });
    }
}
