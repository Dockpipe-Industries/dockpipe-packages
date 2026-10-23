#include "NativePlatform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <Lmcons.h>

bool NativePlatform::isAdministrator()
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators))
        return false;
    BOOL member = FALSE;
    const BOOL checked = CheckTokenMembership(nullptr, administrators, &member);
    FreeSid(administrators);
    return checked && member;
}

QString NativePlatform::interactiveUserName()
{
    wchar_t user[UNLEN + 1];
    DWORD length = UNLEN + 1;
    if (GetUserNameW(user, &length))
        return QString::fromWCharArray(user);
    return qEnvironmentVariable("USERNAME");
}

QDateTime NativePlatform::bootTime()
{
    return QDateTime::currentDateTimeUtc().addMSecs(-static_cast<qint64>(GetTickCount64()));
}

bool NativePlatform::elevate(const ApplicationOptions &options, QString *error)
{
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QString parameters = QStringLiteral("--repair-elevated --interactive-user-b64 ")
        + QString::fromLatin1(options.interactiveUser.toUtf8().toBase64());
    if (!options.runtimeHelper.isEmpty())
        parameters += QStringLiteral(" --runtime-helper-b64 ")
            + QString::fromLatin1(options.runtimeHelper.toUtf8().toBase64());
    SHELLEXECUTEINFOW execution = {};
    execution.cbSize = sizeof(execution);
    execution.lpVerb = L"runas";
    execution.lpFile = reinterpret_cast<LPCWSTR>(executable.utf16());
    execution.lpParameters = reinterpret_cast<LPCWSTR>(parameters.utf16());
    execution.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&execution))
        return true;
    const DWORD code = GetLastError();
    *error = code == ERROR_CANCELLED
        ? QStringLiteral("Administrator approval was cancelled. You can try again when ready.")
        : QStringLiteral("Windows could not start the administrator repair window (error %1).").arg(code);
    return false;
}

QString NativePlatform::statusPath()
{
    return QDir(qEnvironmentVariable("ProgramData", "C:/ProgramData"))
        .filePath("Dockpipe/GuestToolsStatus/status.json");
}

QString NativePlatform::findRuntimeHelper()
{
    const QDir application(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        application.filePath("provision-windows-ssh.ps1"),
        application.filePath("../provision-windows-ssh.ps1"),
        QDir(qEnvironmentVariable("ProgramData", "C:/ProgramData"))
            .filePath("Dockpipe/GuestProvisioning/provision-windows-ssh.ps1")};
    for (const QString &candidate : candidates) {
        const QFileInfo helper(candidate);
        if (helper.isFile())
            return helper.absoluteFilePath();
    }
    return {};
}

bool NativePlatform::applicationInstalled()
{
    const QDir root(QDir(qEnvironmentVariable("ProgramFiles", "C:/Program Files"))
        .filePath("Dockpipe/GuestTools"));
    QFile receipt(root.filePath("installation.json"));
    if (!receipt.open(QIODevice::ReadOnly) || receipt.size() > 16384)
        return false;
    const QJsonDocument document = QJsonDocument::fromJson(receipt.read(16385));
    if (!document.isObject())
        return false;
    const QJsonObject object = document.object();
    if (object["schema"].toInt() != 1 || object["product"].toString() != "DockpipeGuestTools"
        || object["version"].toString() != "1.0.0" || object["ui_task"].toString() != "DockPipeGuestToolsUI")
        return false;
    const QFileInfo executable(object["executable"].toString());
    const QString relative = QDir::fromNativeSeparators(root.relativeFilePath(executable.absoluteFilePath()));
    return executable.isFile() && relative.startsWith("versions/")
        && !relative.split(QLatin1Char('/')).contains(QStringLiteral(".."));
}

QString NativePlatform::currentUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) {
        CloseHandle(token);
        return {};
    }
    QByteArray storage(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (!GetTokenInformation(token, TokenUser, storage.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    LPWSTR text = nullptr;
    const auto *user = reinterpret_cast<const TOKEN_USER *>(storage.constData());
    const BOOL converted = ConvertSidToStringSidW(user->User.Sid, &text);
    CloseHandle(token);
    if (!converted)
        return {};
    const QString sid = QString::fromWCharArray(text);
    LocalFree(text);
    return sid;
}
