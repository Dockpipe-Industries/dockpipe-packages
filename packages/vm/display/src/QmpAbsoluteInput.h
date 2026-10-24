#pragma once

#include <gio/gio.h>
#include <QJsonObject>
#include <QPoint>
#include <QSize>
#include <QString>
#include <chrono>

// Owned by the existing display input worker. Only capability negotiation and
// one fixed pair of absolute axes are exposed; this is not a QMP command API.
class QmpAbsoluteInput {
public:
    explicit QmpAbsoluteInput(GCancellable *cancel);
    ~QmpAbsoluteInput();
    QmpAbsoluteInput(const QmpAbsoluteInput &) = delete;
    QmpAbsoluteInput &operator=(const QmpAbsoluteInput &) = delete;

    static bool normalize(QPoint pixel, QSize frame, QPoint &normalized);
    bool connect(const QString &address, int expectedPid, const QString &startToken);
    bool move(QPoint pixel, QSize frame);
    void close();

private:
    using Deadline = std::chrono::steady_clock::time_point;
    bool readObject(QJsonObject &object, Deadline deadline);
    bool exchange(const QJsonObject &request);
    bool transfer(char *bytes, qsizetype size, bool writing, Deadline deadline);
    GCancellable *cancel_;
    GSocketConnection *connection_ = nullptr;
    unsigned sequence_ = 0;
    QByteArray received_;
    int expectedPid_ = 0;
    QString startToken_;
};
