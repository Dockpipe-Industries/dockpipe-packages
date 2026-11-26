#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <deque>
#include <utility>

class QSocketNotifier;
class QDateTime;

namespace dockpipe::desktop {

// Local private IPC only. The helper owns pinned TLS and broker attachment.
class ProcessChannel final : public QObject {
    Q_OBJECT
public:
    explicit ProcessChannel(QObject *parent = nullptr);
    ~ProcessChannel() override;
    void start(const QString &helper, const QString &configuration, const QJsonObject &reference,
               const QString &role, const QString &mode,
               const QJsonObject &capture = {{"quality", "high"}, {"frame_rate", 30}});
    void renew(const QDateTime &expiresAt);
    bool send(quint8 kind, const QByteArray &payload);
    void closeAsync();
    void close();
    bool stopped() const { return m_process.state() == QProcess::NotRunning; }
    bool ready() const { return m_ready; }

signals:
    void connected();
    void writable();
    void packet(quint8 kind, const QByteArray &payload);
    void failed(const QString &reason);

private:
    void beginClose();
    void terminateGroup(int signal);
    void readPackets();
    void readStatus();
    void fail(const QString &message);
    QProcess m_process;
    QSocketNotifier *m_statusNotifier = nullptr;
    int m_control = -1;
    int m_status = -1;
    int m_childControl = -1;
    int m_childStatus = -1;
    int m_reservedTargets[2] = {-1, -1};
    bool m_ready = false;
    bool m_closing = false;
    // Track bytes in the single QProcess writer without charging clipboard text
    // against the pre-existing input budget.
    struct PendingWrite {
        qint64 remaining;
        quint8 kind;
        bool assetChunk;
    };
    std::deque<PendingWrite> m_pendingWrites;
    qint64 m_assetChunkBacklog = 0;
    qint64 m_inputBacklog = 0;
    QByteArray m_output;
    QByteArray m_statusOutput;
    QJsonObject m_reference;
    QString m_role;
    QString m_mode;
    QJsonObject m_capture;
};

} // namespace dockpipe::desktop
