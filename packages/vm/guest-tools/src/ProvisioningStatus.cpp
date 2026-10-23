#include "ProvisioningStatus.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {
constexpr qint64 maximumStatusBytes = 65536;

ProvisioningStatus invalidStatus(const QString &message)
{
    ProvisioningStatus status;
    status.error = message;
    return status;
}
}

bool ProvisioningStatus::readyForBoot(const QDateTime &bootTime) const
{
    return valid && state == QStringLiteral("ready") && ssh && startupRepairRegistered
        && updatedAt >= bootTime.addSecs(-5);
}

ProvisioningStatus ProvisioningStatus::read(const QString &path, const QDateTime &now)
{
    QFile file(path);
    if (!file.exists())
        return invalidStatus(QStringLiteral("Guest services have not reported their status yet."));
    if (!file.open(QIODevice::ReadOnly))
        return invalidStatus(QStringLiteral("Guest services status could not be read."));
    if (file.size() > maximumStatusBytes)
        return invalidStatus(QStringLiteral("Guest services status exceeds its supported size."));

    const QByteArray bytes = file.read(maximumStatusBytes + 1);
    if (bytes.size() > maximumStatusBytes)
        return invalidStatus(QStringLiteral("Guest services status exceeds its supported size."));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return invalidStatus(QStringLiteral("Guest services status is incomplete or invalid."));

    const QJsonObject object = document.object();
    const QStringList keys = object.keys();
    const QSet<QString> actualKeys(keys.begin(), keys.end());
    const QSet<QString> expectedKeys = {
        QStringLiteral("schema"), QStringLiteral("state"), QStringLiteral("phase"),
        QStringLiteral("message"), QStringLiteral("updated_at"), QStringLiteral("ssh"),
        QStringLiteral("agent"), QStringLiteral("startup_repair_registered")};
    if (actualKeys != expectedKeys || !object["schema"].isDouble() || object["schema"].toDouble() != 1
        || !object["state"].isString() || !object["phase"].isString()
        || !object["message"].isString() || !object["updated_at"].isString()
        || !object["ssh"].isBool() || !object["agent"].isBool()
        || !object["startup_repair_registered"].isBool())
        return invalidStatus(QStringLiteral("Guest services status uses an unsupported contract."));

    ProvisioningStatus status;
    status.state = object["state"].toString();
    status.phase = object["phase"].toString();
    status.message = object["message"].toString();
    if ((status.state != "provisioning" && status.state != "ready" && status.state != "failed")
        || status.phase.isEmpty() || status.phase.size() > 128 || status.message.size() > 2048)
        return invalidStatus(QStringLiteral("Guest services status contains unsupported values."));
    status.updatedAt = QDateTime::fromString(object["updated_at"].toString(), Qt::ISODateWithMs);
    if (!status.updatedAt.isValid())
        status.updatedAt = QDateTime::fromString(object["updated_at"].toString(), Qt::ISODate);
    if (!status.updatedAt.isValid() || status.updatedAt > now.addSecs(300))
        return invalidStatus(QStringLiteral("Guest services status has an invalid update time."));
    status.ssh = object["ssh"].toBool();
    status.agent = object["agent"].toBool();
    status.startupRepairRegistered = object["startup_repair_registered"].toBool();
    status.valid = true;
    return status;
}
