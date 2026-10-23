#include "ClipboardBridge.h"
#include "ClipboardProof.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <cstdio>
#include <memory>

int main(int argc, char **argv) {
    bool worker = false;
    for (int index = 1; index < argc; ++index)
        worker = worker || QByteArray(argv[index]) == "--worker" || QByteArray(argv[index]) == "--verify";
    std::unique_ptr<QCoreApplication> app;
    if (worker)
        app = std::make_unique<QGuiApplication>(argc, argv);
    else
        app = std::make_unique<QCoreApplication>(argc, argv);
    const auto arguments = app->arguments();
    QString operation;
    QString manager;
    QString resource;
    for (int index = 1; index < arguments.size(); ++index) {
        const auto value = arguments[index];
        if (value == "--resource" && index + 1 < arguments.size())
            resource = arguments[++index];
        else if (value == "--manager" && index + 1 < arguments.size())
            manager = arguments[++index];
        else if (value == "--worker" || value == "--start" || value == "--stop" || value == "--status" || value == "--verify")
            operation = value;
        else
            return 2;
    }
    if (!QRegularExpression("^[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}$").match(resource).hasMatch() ||
        !QFileInfo(manager).isAbsolute() || !QFileInfo(manager).isExecutable())
        return 2;
    const QFileInfo scopeInfo(qEnvironmentVariable("DOCKPIPE_VM_CLIPBOARD_SCOPE"));
    if (!scopeInfo.isAbsolute() || !scopeInfo.isDir() || scopeInfo.canonicalFilePath().isEmpty())
        return 2;
    const auto scope = scopeInfo.canonicalFilePath();
    qputenv("DOCKPIPE_VM_CLIPBOARD_SCOPE", scope.toUtf8());
    if (operation == "--verify")
        return runClipboardProof(manager, resource);
    if (operation == "--worker") {
        ClipboardBridge bridge(manager, resource);
        if (!bridge.start())
            return 1;
        return app->exec();
    }
    const auto name = clipboardServerName(scope, resource);
    auto response = clipboardControl(name, operation == "--start" ? "resume" : operation.mid(2), operation == "--stop" ? 10000 : 700);
    if (response.isEmpty() && operation == "--start") {
        QProcess process;
        process.setProgram(app->applicationFilePath());
        process.setArguments({"--worker", "--resource", resource, "--manager", manager});
        process.setStandardInputFile(QProcess::nullDevice());
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
        if (!process.startDetached())
            return 1;
        for (int attempt = 0; attempt < 25 && response.isEmpty(); ++attempt) {
            QThread::msleep(100);
            response = clipboardControl(name, "status", 100);
        }
    }
    if (response.isEmpty()) {
        response = {{"schema", 1}, {"state", "stopped"}};
        if (operation == "--start")
            return 1;
    }
    const auto data = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(data.constData(), 1, static_cast<size_t>(data.size()), stdout);
    return 0;
}
