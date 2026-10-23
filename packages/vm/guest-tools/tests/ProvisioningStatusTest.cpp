#include "ProvisioningStatus.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class ProvisioningStatusTest final : public QObject {
    Q_OBJECT
private:
    const QDateTime now = QDateTime::fromString("2026-10-22T20:00:00Z", Qt::ISODate);

    QJsonObject readyDocument() const
    {
        return {{"schema", 1}, {"state", "ready"}, {"phase", "complete"},
            {"message", "Guest services are ready."}, {"updated_at", "2026-10-22T19:59:30Z"},
            {"ssh", true}, {"agent", true}, {"startup_repair_registered", true}};
    }

    ProvisioningStatus readDocument(const QJsonObject &object)
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("status.json");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Could not create status fixture");
        file.write(QJsonDocument(object).toJson());
        file.close();
        return ProvisioningStatus::read(path, now);
    }

private slots:
    void currentBootReadinessRequiresSshAndStartupRepair()
    {
        const auto status = readDocument(readyDocument());
        QVERIFY(status.valid);
        QVERIFY(status.readyForBoot(now.addSecs(-120)));
        QVERIFY(!status.readyForBoot(now));
        for (const QString &field : {QStringLiteral("ssh"), QStringLiteral("startup_repair_registered")}) {
            auto document = readyDocument();
            document[field] = false;
            QVERIFY(!readDocument(document).readyForBoot(now.addSecs(-120)));
        }
    }

    void agentMayBeDisabledByTheRuntimeConfiguration()
    {
        auto document = readyDocument();
        document["agent"] = false;
        QVERIFY(readDocument(document).readyForBoot(now.addSecs(-120)));
    }

    void provisioningAndFailureNeverCountAsReady()
    {
        for (const QString &state : {QStringLiteral("provisioning"), QStringLiteral("failed")}) {
            auto document = readyDocument();
            document["state"] = state;
            const auto status = readDocument(document);
            QVERIFY(status.valid);
            QVERIFY(!status.readyForBoot(now.addSecs(-120)));
        }
    }

    void futureTimestampRejected()
    {
        auto document = readyDocument();
        document["updated_at"] = "2026-10-22T20:06:00Z";
        QVERIFY(!readDocument(document).valid);
    }

    void unknownFieldsAndWrongTypesRejected()
    {
        auto document = readyDocument();
        document["extra"] = "unsupported";
        QVERIFY(!readDocument(document).valid);
        document = readyDocument();
        document["ssh"] = "true";
        QVERIFY(!readDocument(document).valid);
        document = readyDocument();
        document["schema"] = 1.1;
        QVERIFY(!readDocument(document).valid);
    }

    void malformedOrOversizedStatusRejected()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("status.json");
        QVERIFY(!ProvisioningStatus::read(path, now).valid);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{partial");
        file.close();
        QVERIFY(!ProvisioningStatus::read(path, now).valid);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QByteArray(65537, ' '));
        file.close();
        QVERIFY(!ProvisioningStatus::read(path, now).valid);
    }
};

QTEST_APPLESS_MAIN(ProvisioningStatusTest)
#include "ProvisioningStatusTest.moc"
