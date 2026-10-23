#include "DisplayAddress.h"
#include <QFileInfo>
#include <QDir>

bool decodeDisplayBusPath(const QString &address, QString &path)
{
    const QString prefix = QStringLiteral("unix:path=");
    if (!address.startsWith(prefix) || address.size() > 1024)
        return false;
    const auto encoded = address.mid(prefix.size()).toUtf8();
    if (encoded.contains(',') || encoded.contains(';') || encoded.contains('\0'))
        return false;
    const auto hex = [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'A' && character <= 'F') || (character >= 'a' && character <= 'f');
    };
    for (qsizetype index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '%') {
            if (index + 2 >= encoded.size() || !hex(encoded[index + 1]) || !hex(encoded[index + 2]))
                return false;
            index += 2;
        }
    }
    const auto decoded = QByteArray::fromPercentEncoding(encoded);
    const auto text = QString::fromUtf8(decoded);
    if (decoded.isEmpty() || decoded.size() > 100 || decoded.contains('\0') || text.toUtf8() != decoded)
        return false;
    path = QDir::fromNativeSeparators(text);
    return QFileInfo(path).isAbsolute();
}
