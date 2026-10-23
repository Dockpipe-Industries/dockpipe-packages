#include "ApplicationOptions.h"
#include "ClipboardServer.h"
#include "GuestToolsWindow.h"
#include "NativePlatform.h"
#include "ProvisioningStatus.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextStream>

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Dockpipe Guest Tools"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Install and monitor durable Dockpipe Windows guest services."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"startup", "Keep a background tray service and show only when this boot is not ready."});
    parser.addOption({"show", "Show the existing Guest Tools window or start its interactive service."});
    parser.addOption({"repair-elevated", "Run the package installer from an elevated application."});
    parser.addOption({"interactive-user-b64", "Original interactive username, encoded as UTF-8 base64.", "identity"});
    parser.addOption({"runtime-helper-b64", "Runtime-owned provisioning helper path, encoded as UTF-8 base64.", "path"});
    parser.addOption({"check-status", "Print the current nonsecret readiness result and exit."});
    parser.process(application);

    ApplicationOptions options;
    options.startup = parser.isSet("startup");
    options.repairElevated = parser.isSet("repair-elevated");
    options.interactiveUser = parser.isSet("interactive-user-b64")
        ? QString::fromUtf8(QByteArray::fromBase64(parser.value("interactive-user-b64").toLatin1()))
        : NativePlatform::interactiveUserName();
    options.runtimeHelper = parser.isSet("runtime-helper-b64")
        ? QString::fromUtf8(QByteArray::fromBase64(parser.value("runtime-helper-b64").toLatin1()))
        : NativePlatform::findRuntimeHelper();
    if (options.interactiveUser.isEmpty() || options.interactiveUser.size() > 256 || options.runtimeHelper.size() > 32768)
        parser.showHelp(2);

    const auto status = ProvisioningStatus::read(NativePlatform::statusPath(), QDateTime::currentDateTimeUtc());
    const bool ready = status.readyForBoot(NativePlatform::bootTime());
    if (parser.isSet("check-status")) {
        const QJsonObject result = {{"schema", 1}, {"valid", status.valid}, {"ready_current_boot", ready},
            {"state", status.state}, {"phase", status.phase}, {"error", status.error},
            {"application_installed", NativePlatform::applicationInstalled()}};
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        return ready ? 0 : 1;
    }

    GuestToolsWindow window(options);
    if (options.repairElevated) {
        // The elevated repair window must not replace the limited interactive
        // process that owns clipboard access for the logged-in user.
        window.show();
        return application.exec();
    }
    const QString userSid = NativePlatform::currentUserSid();
    const QString stateDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (userSid.isEmpty() || !QDir().mkpath(stateDirectory))
        return 1;
    QLockFile instance(QDir(stateDirectory).filePath("interactive-" + userSid + ".lock"));
    instance.setStaleLockTime(0);
    if (!instance.tryLock(0)) {
        if (options.startup && !parser.isSet("show"))
            return 0;
        return ClipboardServer::showExisting(userSid) ? 0 : 1;
    }
    ClipboardServer clipboard(&window);
    if (!clipboard.listen(userSid)) {
        QMessageBox::critical(nullptr, QStringLiteral("Guest Tools"),
            QStringLiteral("The private interactive connection could not start. Close other Guest Tools instances and try again."));
        return 1;
    }
    const auto show = [&window] {
        window.showNormal();
        window.raise();
        window.activateWindow();
    };
    clipboard.showRequested = show;
    QSystemTrayIcon tray(application.style()->standardIcon(QStyle::SP_ComputerIcon));
    tray.setToolTip(QStringLiteral("Dockpipe Guest Tools"));
    QMenu menu;
    menu.addAction(QStringLiteral("Show Guest Tools"), &window, show);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Exit Guest Tools"), &application, &QCoreApplication::quit);
    tray.setContextMenu(&menu);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &window, [show](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            show();
    });
    tray.show();
    if (!options.startup || !ready || parser.isSet("show"))
        show();
    return application.exec();
}
