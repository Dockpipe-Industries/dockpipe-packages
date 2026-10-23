#include "ClipboardServer.h"
#include "ClipboardRequest.h"
#include "NativeClipboard.h"
#include "NativeDisplay.h"

#include <QJsonDocument>
#include <QLocalSocket>
#include <QTimer>
#include <QVariant>
#include <QWidget>

namespace {
QString pipeName(const QString &userSid)
{
    return QStringLiteral("dockpipe-guest-clipboard-v1-") + userSid;
}

QJsonObject failure(const QString &message)
{
    return {{"schema", 1}, {"success", false}, {"error", message}};
}
}

ClipboardServer::ClipboardServer(QWidget *owner, QObject *parent) : QObject(parent), m_owner(owner)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(8);
    connect(&m_server, &QLocalServer::newConnection, this, &ClipboardServer::acceptConnections);
}

bool ClipboardServer::listen(const QString &userSid)
{
    return !userSid.isEmpty() && m_server.listen(pipeName(userSid));
}

bool ClipboardServer::showExisting(const QString &userSid)
{
    QLocalSocket socket;
    socket.connectToServer(pipeName(userSid));
    if (!socket.waitForConnected(1500))
        return false;
    socket.write("{\"schema\":1,\"operation\":\"show\"}\n");
    if (socket.bytesToWrite() && !socket.waitForBytesWritten(1500))
        return false;
    if (!socket.canReadLine() && !socket.waitForReadyRead(1500))
        return false;
    const QJsonDocument response = QJsonDocument::fromJson(socket.readAll());
    return response.isObject() && response.object()["schema"].toInt() == 1
        && response.object()["success"].toBool();
}

void ClipboardServer::acceptConnections()
{
    while (m_server.hasPendingConnections()) {
        QLocalSocket *socket = m_server.nextPendingConnection();
        if (m_requests.size() >= 8) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setReadBufferSize(ClipboardRequest::maximumWireBytes + 1);
        m_requests.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readRequest(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            m_requests.remove(socket);
            socket->deleteLater();
        });
        QTimer::singleShot(5000, socket, [this, socket] {
            if (!m_requests.contains(socket))
                return;
            if (socket->property("replied").toBool())
                socket->abort();
            else
                reply(socket, failure(QStringLiteral("Clipboard request timed out.")));
        });
        if (socket->bytesAvailable())
            readRequest(socket);
    }
}

void ClipboardServer::readRequest(QLocalSocket *socket)
{
    if (socket->property("replied").toBool())
        return;
    auto found = m_requests.find(socket);
    if (found == m_requests.end())
        return;
    found.value() += socket->read(ClipboardRequest::maximumWireBytes + 1 - found.value().size());
    if (found.value().size() > ClipboardRequest::maximumWireBytes) {
        reply(socket, failure(QStringLiteral("Clipboard request exceeds its size limit.")));
        return;
    }
    const qsizetype newline = found.value().indexOf('\n');
    if (newline < 0)
        return;
    if (newline != found.value().size() - 1) {
        reply(socket, failure(QStringLiteral("One clipboard request is allowed per connection.")));
        return;
    }
    const ClipboardRequest request = ClipboardRequest::parse(found.value().left(newline));
    if (!request.valid) {
        reply(socket, failure(QStringLiteral("Clipboard request is invalid or unsupported.")));
        return;
    }
    if (request.operation == "show") {
        if (showRequested)
            showRequested();
        reply(socket, {{"schema", 1}, {"success", true}});
        return;
    }
    if (request.operation == "display-modes") {
        reply(socket, NativeDisplay::describeResolutions());
        return;
    }
    if (request.operation == "display-resolution") {
        reply(socket, NativeDisplay::setResolution(request.resolution));
        return;
    }
    if (request.operation == "write") {
        const auto result = NativeClipboard::writeText(m_owner->winId(), request.text, request.expectedSha256);
        if (result == NativeClipboard::WriteResult::Conflict) {
            reply(socket, failure(QStringLiteral("conflict")));
            return;
        }
        if (result != NativeClipboard::WriteResult::Success) {
            reply(socket, failure(QStringLiteral("clipboard-unavailable")));
            return;
        }
        reply(socket, {{"schema", 1}, {"success", true}});
        return;
    }
    const auto result = NativeClipboard::readText(m_owner->winId());
    if (result.state == NativeClipboard::ReadState::Unsupported) {
        reply(socket, failure(QStringLiteral("unsupported-text")));
        return;
    }
    if (result.state != NativeClipboard::ReadState::Text) {
        reply(socket, failure(QStringLiteral("clipboard-unavailable")));
        return;
    }
    const QByteArray text = result.text.toUtf8();
    reply(socket, {{"schema", 1}, {"success", true},
        {"text_base64", QString::fromLatin1(text.toBase64())}});
}

void ClipboardServer::reply(QLocalSocket *socket, const QJsonObject &response)
{
    socket->setProperty("replied", true);
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
    socket->disconnectFromServer();
}
