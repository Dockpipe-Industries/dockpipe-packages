#pragma once

#include <QClipboard>
#include <QJsonObject>
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QProcess>
#include <QTimer>

QString clipboardServerName(const QString &scope, const QString &resource);
QJsonObject clipboardControlRequest(const QString &name, const QJsonObject &request, int timeout = 700);
QJsonObject clipboardControl(const QString &name, const QString &operation, int timeout = 700);

class ClipboardBridge final : public QObject {
public:
    ClipboardBridge(QString manager, QString resource);
    bool start();

private:
    void cycle();
    void exchange(const QJsonObject &request, bool writing = false);
    void completed();
    void acceptControls();
    void pause(const QString &state);
    QByteArray hostText(bool &available) const;

    QString manager_;
    QString resource_;
    QString state_ = QStringLiteral("active");
    QClipboard *clipboard_;
    QLocalServer server_;
    QLockFile lock_;
    QTimer timer_;
    QTimer deadline_;
    QProcess process_;
    QByteArray output_;
    QByteArray lastHost_;
    QByteArray lastGuest_;
    QByteArray sentHash_;
    quint64 hostGeneration_ = 0;
    quint64 lastGeneration_ = 0;
    bool initialized_ = false;
    bool writing_ = false;
    bool applying_ = false;
    int connections_ = 0;
};
