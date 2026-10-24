#pragma once

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include "NativeDisplayPlatform.h"
#include <QByteArray>
#include <QFileInfo>
#include <QDebug>

namespace NativeFixtureOwnership {

// Only call for a file/socket just created by this test inside its private dir.
// An elevated token's default object owner need not be its current-user SID.
inline bool assignFreshObjectToCurrentUser(const QString &path)
{
    if (!NativeDisplayPlatform::privateDirectory(QFileInfo(path).absolutePath()))
        return false;

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0 || size > 65536) {
        CloseHandle(token);
        return false;
    }
    QByteArray information(int(size), Qt::Uninitialized);
    const bool tokenRead = GetTokenInformation(token, TokenUser, information.data(), size, &size);
    CloseHandle(token);
    if (!tokenRead)
        return false;
    const auto *user = reinterpret_cast<const TOKEN_USER *>(information.constData());
    if (!IsValidSid(user->User.Sid))
        return false;

    HANDLE object = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()),
        READ_CONTROL | WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (object == INVALID_HANDLE_VALUE)
        return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    constexpr ULONG unixSocketTag = 0x80000023;
    bool valid = GetFileInformationByHandleEx(object, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
        !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        (!(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || attributes.ReparseTag == unixSocketTag);
    if (valid) {
        const DWORD status = SetSecurityInfo(object, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                            user->User.Sid, nullptr, nullptr, nullptr);
        valid = status == ERROR_SUCCESS;
        if (!valid)
            qWarning("Fresh fixture ownership assignment failed: error=%lu", status);
    }
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (valid) {
        valid = GetSecurityInfo(object, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                &owner, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS &&
            owner && IsValidSid(owner) && EqualSid(owner, user->User.Sid);
    }
    if (descriptor)
        LocalFree(descriptor);
    CloseHandle(object);
    return valid;
}

} // namespace NativeFixtureOwnership
#endif
