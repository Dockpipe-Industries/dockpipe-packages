#pragma once

#include <QHash>
#include <QLocalServer>
#include <QJsonObject>
#include <functional>

class QLocalSocket;
class QWidget;

class ClipboardServer final : public QObject {
    Q_OBJECT
public:
    explicit ClipboardServer(QWidget *owner, QObject *parent = nullptr);
    bool listen(const QString &userSid);
    static bool showExisting(const QString &userSid);
    std::function<void()> showRequested;

private:
    void acceptConnections();
    void readRequest(QLocalSocket *socket);
    void reply(QLocalSocket *socket, const QJsonObject &response);

    QWidget *m_owner = nullptr;
    QLocalServer m_server;
    QHash<QLocalSocket *, QByteArray> m_requests;
};
