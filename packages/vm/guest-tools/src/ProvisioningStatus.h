#pragma once

#include <QDateTime>
#include <QString>

struct ProvisioningStatus {
    bool valid = false;
    QString state;
    QString phase;
    QString message;
    QDateTime updatedAt;
    bool ssh = false;
    bool agent = false;
    bool startupRepairRegistered = false;
    QString error;

    bool readyForBoot(const QDateTime &bootTime) const;
    static ProvisioningStatus read(const QString &path, const QDateTime &now);
};
