#pragma once

#include "ApplicationOptions.h"
#include <QMainWindow>
#include <QProcess>
#include <QTimer>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class GuestToolsWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit GuestToolsWindow(ApplicationOptions options);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void refreshStatus();
    void requestRepair();
    void runInstaller();
    void readInstallerOutput();
    void installerFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void showFailure(const QString &message);

    ApplicationOptions m_options;
    QLabel *m_state = nullptr;
    QLabel *m_message = nullptr;
    QLabel *m_services = nullptr;
    QLabel *m_application = nullptr;
    QLabel *m_updated = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_repair = nullptr;
    QPlainTextEdit *m_details = nullptr;
    QTimer m_poll;
    QTimer m_installerDeadline;
    QProcess m_installer;
    QString m_localError;
    QByteArray m_installerOutput;
};
