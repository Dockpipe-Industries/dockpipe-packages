#include "ClipboardRequest.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

class ClipboardRequestTest final : public QObject {
    Q_OBJECT
private:
    ClipboardRequest writeRequest(const QByteArray &text)
    {
        const QJsonObject object = {{"schema", 1}, {"operation", "write"},
            {"text_base64", QString::fromLatin1(text.toBase64())}};
        return ClipboardRequest::parse(QJsonDocument(object).toJson(QJsonDocument::Compact));
    }

private slots:
    void acceptsReadAndShowWithoutExtraFields()
    {
        QVERIFY(ClipboardRequest::parse("{\"schema\":1,\"operation\":\"read\"}").valid);
        QVERIFY(ClipboardRequest::parse("{\"schema\":1,\"operation\":\"show\"}").valid);
        QVERIFY(!ClipboardRequest::parse("{\"schema\":1,\"operation\":\"read\",\"text_base64\":\"\"}").valid);
    }

    void boundsDisplayResolutionAndRejectsExtraFields()
    {
        const auto request = ClipboardRequest::parse(R"({"schema":1,"operation":"display-resolution","width":1920,"height":1080})");
        QVERIFY(request.valid);
        QCOMPARE(request.resolution, QSize(1920, 1080));
        for (const auto &data : {
            R"({"schema":1,"operation":"display-resolution","width":639,"height":1080})",
            R"({"schema":1,"operation":"display-resolution","width":1920,"height":8193})",
            R"({"schema":1,"operation":"display-resolution","width":1920.5,"height":1080})",
            R"({"schema":1,"operation":"display-resolution","width":"1920","height":1080})",
            R"({"schema":1,"operation":"display-resolution","width":1920,"height":1080,"text_base64":""})"})
            QVERIFY(!ClipboardRequest::parse(data).valid);
    }

    void preservesUtf8AndEmptyText()
    {
        const QByteArray text = QString::fromUtf8(u8"Native clipboard — café\nsecond line").toUtf8();
        const auto request = writeRequest(text);
        QVERIFY(request.valid);
        QCOMPARE(request.text.toUtf8(), text);
        QVERIFY(writeRequest({}).valid);
    }

    void exactTextCapacityIsEnforced()
    {
        QVERIFY(writeRequest(QByteArray(ClipboardRequest::maximumTextBytes, 'x')).valid);
        QVERIFY(!writeRequest(QByteArray(ClipboardRequest::maximumTextBytes + 1, 'x')).valid);
    }

    void rejectsInvalidUtf8AndNoncanonicalBase64()
    {
        QVERIFY(!writeRequest(QByteArray::fromHex("c0af")).valid);
        QVERIFY(!ClipboardRequest::parse("{\"schema\":1,\"operation\":\"write\",\"text_base64\":\"YQ\"}").valid);
        QVERIFY(!ClipboardRequest::parse("{\"schema\":1,\"operation\":\"write\",\"text_base64\":\"Y Q==\"}").valid);
        QVERIFY(!ClipboardRequest::parse("{\"schema\":1,\"operation\":\"write\",\"text_base64\":\"!\"}").valid);
    }

    void conditionalWritesRequireAnExactExpectedIdentity()
    {
        QJsonObject object = {{"schema", 1}, {"operation", "write"}, {"text_base64", ""},
            {"expected_sha256", QString(64, QLatin1Char('a'))}};
        QVERIFY(ClipboardRequest::parse(QJsonDocument(object).toJson()).valid);
        object["expected_sha256"] = "unsupported-text";
        QVERIFY(ClipboardRequest::parse(QJsonDocument(object).toJson()).valid);
        for (const QString &invalid : {QStringLiteral(""), QStringLiteral("ABC"), QStringLiteral("latest")}) {
            object["expected_sha256"] = invalid;
            QVERIFY(!ClipboardRequest::parse(QJsonDocument(object).toJson()).valid);
        }
        QVERIFY(!writeRequest(QByteArray(1, '\0')).valid);
    }

    void rejectsMalformedVersionsOperationsAndWireCapacity()
    {
        QVERIFY(!ClipboardRequest::parse("{partial").valid);
        QVERIFY(!ClipboardRequest::parse("{\"schema\":2,\"operation\":\"read\"}").valid);
        QVERIFY(!ClipboardRequest::parse("{\"schema\":1.1,\"operation\":\"read\"}").valid);
        QVERIFY(!ClipboardRequest::parse("{\"schema\":1,\"operation\":\"files\"}").valid);
        QVERIFY(!ClipboardRequest::parse(QByteArray(ClipboardRequest::maximumWireBytes + 1, ' ')).valid);
    }
};

QTEST_APPLESS_MAIN(ClipboardRequestTest)
#include "ClipboardRequestTest.moc"
