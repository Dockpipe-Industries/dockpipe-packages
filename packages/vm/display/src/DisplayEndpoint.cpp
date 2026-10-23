#include "DisplayEndpoint.h"
#include "NativeDisplayPlatform.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QDir>
#include <cmath>
#include <limits>

bool DisplayEndpoint::processMatches(int pid, const QString &startToken)
{
    return NativeDisplayPlatform::processMatches(pid, startToken);
}

bool DisplayEndpoint::load(const QJsonObject &options, DisplayEndpoint &endpoint, QString &error)
{
    error = QStringLiteral("Invalid or unavailable private display endpoint");
    static const QRegularExpression resourcePattern(QStringLiteral("^[A-Za-z0-9_-]{1,80}$"));
    if (options.size() != 3 || options.value("schema") != 1 ||
        !options.value("resource_id").isString() || !options.value("state_ref").isString())
        return false;
    const auto resource = options.value("resource_id").toString();
    const auto state = options.value("state_ref").toString();
    const QFileInfo directory(state);
    if (!resourcePattern.match(resource).hasMatch() || !directory.isAbsolute() ||
        state.size() > 4096 || directory.fileName() != "display-" + resource ||
        directory.canonicalFilePath() != QDir::cleanPath(state))
        return false;
    QByteArray bytes;
    if (!NativeDisplayPlatform::readPrivateDescriptor(state, bytes))
        return false;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const auto object = document.object();
    if (object.size() != 7 || object.value("schema") != 1 ||
        object.value("resource_id") != resource || object.value("backend") != "qemu-dbus" ||
        !object.value("bus_address").isString() || !object.value("qemu_start_token").isString() ||
        !object.value("qemu_pid").isDouble() || !object.value("console_id").isDouble())
        return false;
    const double pid = object.value("qemu_pid").toDouble();
    const double console = object.value("console_id").toDouble();
    if (!std::isfinite(pid) || std::floor(pid) != pid || pid < 1 ||
        pid > std::numeric_limits<int>::max() || console < 0 || console > 15 || std::floor(console) != console)
        return false;
    endpoint.busAddress = object.value("bus_address").toString();
    endpoint.consoleId = unsigned(console);
    endpoint.qemuPid = int(pid);
    endpoint.qemuStartToken = object.value("qemu_start_token").toString();
    if (!processMatches(endpoint.qemuPid, endpoint.qemuStartToken))
        return false;
    error.clear();
    return true;
}
