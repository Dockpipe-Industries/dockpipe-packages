#pragma once

#include <QByteArray>
#include <QString>
#include <QSize>

struct ClipboardRequest {
    static constexpr qsizetype maximumTextBytes = 1048576;
    static constexpr qsizetype maximumWireBytes = 1572864;
    bool valid = false;
    QString operation;
    QString text;
    QString expectedSha256;
    QSize resolution;

    static ClipboardRequest parse(const QByteArray &line);
};
