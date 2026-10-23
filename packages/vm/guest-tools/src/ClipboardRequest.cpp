#include "ClipboardRequest.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>
#include <QRegularExpression>

ClipboardRequest ClipboardRequest::parse(const QByteArray &line)
{
    ClipboardRequest request;
    if (line.isEmpty() || line.size() > maximumWireBytes)
        return request;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return request;
    const QJsonObject object = document.object();
    if (!object["schema"].isDouble() || object["schema"].toDouble() != 1
        || !object["operation"].isString())
        return request;
    request.operation = object["operation"].toString();
    if (request.operation == "read" || request.operation == "show" || request.operation == "display-modes") {
        request.valid = object.size() == 2;
        return request;
    }
    if (request.operation == "display-resolution") {
        const auto width = object["width"];
        const auto height = object["height"];
        if (object.size() != 4 || !width.isDouble() || !height.isDouble()
            || width.toDouble() != width.toInt() || height.toDouble() != height.toInt()
            || width.toInt() < 640 || width.toInt() > 8192
            || height.toInt() < 480 || height.toInt() > 8192)
            return request;
        request.resolution = QSize(width.toInt(), height.toInt());
        request.valid = true;
        return request;
    }
    if (request.operation != "write" || !object["text_base64"].isString())
        return request;
    const bool conditional = object.contains("expected_sha256");
    if (object.size() != (conditional ? 4 : 3))
        return request;
    if (conditional) {
        if (!object["expected_sha256"].isString())
            return request;
        request.expectedSha256 = object["expected_sha256"].toString();
        static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
        if (request.expectedSha256 != "unsupported-text" && !digest.match(request.expectedSha256).hasMatch())
            return request;
    }
    const QString encoded = object["text_base64"].toString();
    const QByteArray ascii = encoded.toLatin1();
    if (QString::fromLatin1(ascii) != encoded)
        return request;
    const auto decoded = QByteArray::fromBase64Encoding(ascii, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > maximumTextBytes || decoded.decoded.toBase64() != ascii)
        return request;
    QStringDecoder decoder(QStringDecoder::Utf8);
    request.text = decoder(decoded.decoded);
    if (decoder.hasError() || request.text.contains(QChar(u'\0')))
        return request;
    request.valid = true;
    return request;
}
