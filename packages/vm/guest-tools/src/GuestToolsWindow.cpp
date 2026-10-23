#include "GuestToolsWindow.h"
#include "NativeDisplay.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include "NativePlatform.h"
#include "ProvisioningStatus.h"

#include <QCoreApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <utility>

namespace {
QString powerShellLiteral(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QString healthLabel(bool healthy)
{
    return healthy ? QStringLiteral("Ready") : QStringLiteral("Waiting");
}
}

GuestToolsWindow::GuestToolsWindow(ApplicationOptions options)
    : m_options(std::move(options))
{
    setWindowTitle(tr("Dockpipe Guest Tools"));
    resize(620, 475);
    setMinimumSize(540, 410);
    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);
    auto *title = new QLabel(tr("Guest Tools"), content);
    title->setObjectName(QStringLiteral("title"));
    layout->addWidget(title);
    auto *subtitle = new QLabel(tr("Connect this Windows guest to your Dockpipe host."), content);
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);
    m_state = new QLabel(content);
    m_state->setObjectName(QStringLiteral("state"));
    layout->addWidget(m_state);
    m_message = new QLabel(content);
    m_message->setTextFormat(Qt::PlainText);
    m_message->setWordWrap(true);
    layout->addWidget(m_message);
    m_progress = new QProgressBar(content);
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(7);
    layout->addWidget(m_progress);
    m_services = new QLabel(content);
    m_services->setTextFormat(Qt::PlainText);
    m_services->setWordWrap(true);
    layout->addWidget(m_services);
    m_application = new QLabel(content);
    m_application->setTextFormat(Qt::PlainText);
    layout->addWidget(m_application);
    m_updated = new QLabel(content);
    m_updated->setObjectName(QStringLiteral("secondary"));
    m_updated->setWordWrap(true);
    layout->addWidget(m_updated);
    m_repair = new QPushButton(tr("Install or repair"), content);
    m_repair->setMinimumHeight(38);
    connect(m_repair, &QPushButton::clicked, this, &GuestToolsWindow::requestRepair);
    layout->addWidget(m_repair);
    auto *displayRow = new QHBoxLayout;
    auto *resolutions = new QComboBox(content);
    resolutions->setObjectName(QStringLiteral("guestDisplayResolution"));
    for (const auto &resolution : NativeDisplay::supportedResolutions()) {
        resolutions->addItem(QStringLiteral("%1 x %2").arg(resolution.width()).arg(resolution.height()), resolution);
        if (resolution == NativeDisplay::currentResolution())
            resolutions->setCurrentIndex(resolutions->count() - 1);
    }
    auto *applyResolution = new QPushButton(tr("Apply resolution"), content);
    applyResolution->setEnabled(resolutions->count() > 0);
    connect(applyResolution, &QPushButton::clicked, this, [this, resolutions] {
        const auto result = NativeDisplay::setResolution(resolutions->currentData().toSize());
        if (!result["success"].toBool())
            QMessageBox::warning(this, tr("Display resolution"), result["error"].toString());
    });
    displayRow->addWidget(resolutions, 1);
    displayRow->addWidget(applyResolution);
    layout->addLayout(displayRow);
    m_details = new QPlainTextEdit(content);
    m_details->setReadOnly(true);
    m_details->setMaximumBlockCount(100);
    m_details->setPlaceholderText(tr("Repair details appear here when action is required."));
    m_details->setMaximumHeight(100);
    layout->addWidget(m_details);
    auto *independence = new QLabel(tr("Provisioning continues as a Windows SYSTEM task when this window closes."), content);
    independence->setObjectName(QStringLiteral("secondary"));
    independence->setWordWrap(true);
    layout->addWidget(independence);
    setCentralWidget(content);
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget { background: #111c28; color: #edf2f7; font: 10pt 'Segoe UI'; }"
        "QLabel#title { font-size: 24pt; font-weight: 600; }"
        "QLabel#state { font-size: 15pt; font-weight: 600; padding-top: 8px; }"
        "QLabel#secondary { color: #9aaec1; font-size: 9pt; }"
        "QPushButton { background: #176da1; border: 1px solid #2a92cf; border-radius: 5px; padding: 5px; }"
        "QPushButton:hover { background: #2083bc; }"
        "QPushButton:disabled { background: #263b4c; color: #8ba2b5; border-color: #3c5264; }"
        "QPlainTextEdit { background: #0c151f; border: 1px solid #314759; border-radius: 4px; }"
        "QProgressBar { border: 0; background: #263b4c; border-radius: 3px; }"
        "QProgressBar::chunk { background: #3bbda1; border-radius: 3px; }"));

    m_poll.setInterval(1000);
    connect(&m_poll, &QTimer::timeout, this, &GuestToolsWindow::refreshStatus);
    m_poll.start();
    m_installer.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_installer, &QProcess::readyReadStandardOutput, this, &GuestToolsWindow::readInstallerOutput);
    connect(&m_installer, &QProcess::finished, this, &GuestToolsWindow::installerFinished);
    connect(&m_installer, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_installerDeadline.stop();
            showFailure(tr("The package installer could not start. Check that the complete Guest Tools bundle is available."));
        }
    });
    m_installerDeadline.setSingleShot(true);
    m_installerDeadline.setInterval(120000);
    connect(&m_installerDeadline, &QTimer::timeout, this, [this] {
        m_installer.kill();
        showFailure(tr("The application installer timed out. Any registered SYSTEM provisioning task continues independently. Check the status and retry."));
    });
    refreshStatus();
    if (m_options.repairElevated)
        QTimer::singleShot(0, this, &GuestToolsWindow::runInstaller);
}

void GuestToolsWindow::refreshStatus()
{
    const auto status = ProvisioningStatus::read(NativePlatform::statusPath(), QDateTime::currentDateTimeUtc());
    const bool current = status.valid && status.updatedAt >= NativePlatform::bootTime().addSecs(-5);
    const bool ready = status.readyForBoot(NativePlatform::bootTime());
    const bool installing = m_installer.state() != QProcess::NotRunning;
    const bool working = installing || (current && status.state == QStringLiteral("provisioning"));
    m_repair->setEnabled(!installing);
    m_progress->setVisible(working);
    if (working)
        m_progress->setRange(0, 0);

    if (!m_localError.isEmpty()) {
        m_state->setText(tr("Action required"));
        m_message->setText(m_localError);
    } else if (ready) {
        m_state->setText(tr("Guest services are ready"));
        m_message->setText(status.message);
    } else if (installing) {
        m_state->setText(tr("Installing Guest Tools"));
        m_message->setText(tr("Verifying the application and registering independent Windows provisioning."));
    } else if (current && status.state == QStringLiteral("provisioning")) {
        m_state->setText(tr("Setting up guest services"));
        m_message->setText(status.message + QStringLiteral("\n") + tr("Current step: %1").arg(status.phase));
    } else if (current && status.state == QStringLiteral("failed")) {
        m_state->setText(tr("Guest services need attention"));
        m_message->setText(status.message);
    } else {
        m_state->setText(tr("Guest services are not ready"));
        m_message->setText(status.valid
            ? tr("The last report belongs to an earlier boot. Install or repair to check this Windows session.")
            : status.error);
    }
    m_services->setText(tr("SSH: %1   ·   Agent: %2   ·   Startup repair: %3")
        .arg(healthLabel(current && status.ssh), healthLabel(current && status.agent),
             healthLabel(current && status.startupRepairRegistered)));
    m_application->setText(NativePlatform::applicationInstalled()
        ? tr("Application: Installed · version 1.0.0")
        : tr("Application: Not installed · running from the current bundle"));
    m_updated->setText(status.valid
        ? tr("Last update: %1").arg(QLocale().toString(status.updatedAt.toLocalTime(), QLocale::ShortFormat))
        : tr("Administrator approval is requested only when you choose Install or repair."));
}

void GuestToolsWindow::requestRepair()
{
    m_localError.clear();
    m_details->clear();
    if (NativePlatform::isAdministrator()) {
        runInstaller();
        return;
    }
    QString error;
    if (!NativePlatform::elevate(m_options, &error))
        showFailure(error);
    else
        m_details->setPlainText(tr("Continue in the administrator window. This window keeps showing the shared service status."));
}

void GuestToolsWindow::runInstaller()
{
    if (!NativePlatform::isAdministrator()) {
        showFailure(tr("Administrator approval is required to install or repair Guest Tools."));
        return;
    }
    if (m_installer.state() != QProcess::NotRunning)
        return;
    const QString installer = QDir(QCoreApplication::applicationDirPath()).filePath("install-guest-tools.ps1");
    if (!QFileInfo::exists(installer)) {
        showFailure(tr("The package installer is missing. Open the complete Guest Tools bundle."));
        return;
    }
    QString command = QStringLiteral("$ErrorActionPreference='Stop'; & ") + powerShellLiteral(installer)
        + QStringLiteral(" -InteractiveUserName ") + powerShellLiteral(m_options.interactiveUser);
    if (!m_options.runtimeHelper.isEmpty())
        command += QStringLiteral(" -RuntimeHelper ") + powerShellLiteral(m_options.runtimeHelper);
    command += QStringLiteral("; if ($LASTEXITCODE -ne $null -and $LASTEXITCODE -ne 0) { exit $LASTEXITCODE }");
    const QString encoded = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(command.utf16()), command.size() * 2).toBase64());
    const QString powershell = QDir(qEnvironmentVariable("SystemRoot", "C:/Windows"))
        .filePath("System32/WindowsPowerShell/v1.0/powershell.exe");
    m_localError.clear();
    m_installerOutput.clear();
    m_installer.setProgram(powershell);
    m_installer.setArguments({"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded});
    m_installer.start();
    m_installerDeadline.start();
    refreshStatus();
}

void GuestToolsWindow::readInstallerOutput()
{
    m_installerOutput += m_installer.readAllStandardOutput();
    if (m_installerOutput.size() > 32768)
        m_installerOutput = m_installerOutput.right(32768);
    m_details->setPlainText(QString::fromLocal8Bit(m_installerOutput));
}

void GuestToolsWindow::installerFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_installerDeadline.stop();
    readInstallerOutput();
    if (exitStatus != QProcess::NormalExit || exitCode != 0)
        showFailure(tr("Guest Tools installation failed. Review the details below, correct the problem and retry."));
    else
        m_localError.clear();
    refreshStatus();
}

void GuestToolsWindow::showFailure(const QString &message)
{
    m_localError = message;
    refreshStatus();
}

void GuestToolsWindow::closeEvent(QCloseEvent *event)
{
    if (m_options.repairElevated) {
        event->accept();
        QCoreApplication::quit();
        return;
    }
    event->ignore();
    hide();
}
