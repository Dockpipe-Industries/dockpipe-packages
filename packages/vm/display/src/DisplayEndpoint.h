#pragma once
#include <QJsonObject>
#include <QString>

struct DisplayEndpoint {
    QString busAddress;
    QString inputAddress;
    unsigned consoleId = 0;
    int qemuPid = 0;
    QString qemuStartToken;
    static bool processMatches(int pid, const QString &startToken);
    static bool load(const QJsonObject &options, DisplayEndpoint &endpoint, QString &error);
};
