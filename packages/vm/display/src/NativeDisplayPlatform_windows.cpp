#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include "NativeDisplayPlatform.h"
#include "DisplayAddress.h"
#include <QDir>
#include <QFileInfo>
#include <QDebug>

#ifndef IO_REPARSE_TAG_AF_UNIX
#define IO_REPARSE_TAG_AF_UNIX 0x80000023L
#endif

namespace {
QByteArray userSid(HANDLE process)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray information;
    if (size == 0 || size > 65536) {
        CloseHandle(token);
        return {};
    }
    information.resize(int(size));
    const bool read = GetTokenInformation(token, TokenUser, information.data(), size, &size);
    CloseHandle(token);
    if (!read)
        return {};
    const auto *user = reinterpret_cast<const TOKEN_USER *>(information.constData());
    if (!IsValidSid(user->User.Sid))
        return {};
    return QByteArray(static_cast<const char *>(user->User.Sid), int(GetLengthSid(user->User.Sid)));
}

QByteArray wellKnownSid(WELL_KNOWN_SID_TYPE type)
{
    QByteArray bytes(SECURITY_MAX_SID_SIZE, '\0');
    DWORD size = DWORD(bytes.size());
    if (!CreateWellKnownSid(type, nullptr, bytes.data(), &size))
        return {};
    bytes.resize(int(size));
    return bytes;
}

bool privateAcl(HANDLE handle)
{
    const auto user = userSid(GetCurrentProcess());
    const auto system = wellKnownSid(WinLocalSystemSid);
    const auto administrators = wellKnownSid(WinBuiltinAdministratorsSid);
    if (user.isEmpty() || system.isEmpty() || administrators.isEmpty())
        return false;
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &acl, nullptr, &security) != ERROR_SUCCESS)
        return false;
    bool valid = owner && IsValidSid(owner) && EqualSid(owner, const_cast<char *>(user.constData())) && acl;
    for (DWORD index = 0; valid && index < acl->AceCount; ++index) {
        void *entry = nullptr;
        if (!GetAce(acl, index, &entry)) {
            valid = false;
            break;
        }
        const auto *header = static_cast<const ACE_HEADER *>(entry);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceSize < sizeof(ACCESS_ALLOWED_ACE)) {
            qWarning("Display ACL unsupported ACE: type=%u size=%u", header->AceType, header->AceSize);
            valid = false;
            break;
        }
        auto *sid = reinterpret_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE *>(entry)->SidStart);
        valid = IsValidSid(sid) &&
            (EqualSid(sid, const_cast<char *>(user.constData())) ||
             EqualSid(sid, const_cast<char *>(system.constData())) ||
             EqualSid(sid, const_cast<char *>(administrators.constData())));
        if (!valid)
            qWarning("Display ACL grants an unapproved principal: flags=%u", header->AceFlags);
    }
    if (!valid)
        qWarning("Display private ACL rejected: owner_matches=%d acl_present=%d",
                 owner && IsValidSid(owner) && EqualSid(owner, const_cast<char *>(user.constData())), acl != nullptr);
    LocalFree(security);
    return valid;
}

HANDLE openMetadata(const QString &path, DWORD share)
{
    return CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()),
        FILE_READ_ATTRIBUTES | READ_CONTROL, share, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}
}

bool NativeDisplayPlatform::processMatches(int pid, const QString &startToken)
{
    bool parsed = false;
    const auto expected = startToken.toULongLong(&parsed);
    if (pid <= 0 || !parsed || QString::number(expected) != startToken)
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process)
        return false;
    FILETIME creation{}, exit{}, kernel{}, user{};
    const auto owner = userSid(process);
    const auto current = userSid(GetCurrentProcess());
    const bool valid = !owner.isEmpty() && owner == current &&
        WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
        GetProcessTimes(process, &creation, &exit, &kernel, &user) &&
        ((quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime) == expected;
    CloseHandle(process);
    return valid;
}

bool NativeDisplayPlatform::privateDirectory(const QString &path)
{
    const QFileInfo information(path);
    if (!information.isAbsolute() ||
        information.canonicalFilePath().compare(QDir::cleanPath(path), Qt::CaseInsensitive) != 0)
        return false;
    HANDLE handle = openMetadata(path, FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const bool valid = GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && privateAcl(handle);
    CloseHandle(handle);
    return valid;
}

bool NativeDisplayPlatform::protectDirectory(const QString &path)
{
    const auto sid = userSid(GetCurrentProcess());
    if (sid.isEmpty())
        return false;
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = FILE_ALL_ACCESS;
    entry.grfAccessMode = SET_ACCESS;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(const_cast<char *>(sid.constData()));
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &entry, nullptr, &acl) != ERROR_SUCCESS)
        return false;
    auto nativePath = path.toStdWString();
    const DWORD result = SetNamedSecurityInfoW(
        nativePath.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS && privateDirectory(path);
}

bool NativeDisplayPlatform::readPrivateDescriptor(const QString &directory, QByteArray &bytes)
{
    if (!privateDirectory(directory))
        return false;
    const auto path = directory + "/display.json";
    HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        qWarning("Display private descriptor open failed: error=%lu", GetLastError());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    bool valid = GetFileInformationByHandle(file, &information) &&
        !(information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        information.nNumberOfLinks == 1 && information.nFileSizeHigh == 0 &&
        information.nFileSizeLow > 0 && information.nFileSizeLow <= 4096 && privateAcl(file);
    if (!valid)
        qWarning("Display descriptor metadata rejected: attributes=%lu links=%lu size_high=%lu size_low=%lu error=%lu",
                 information.dwFileAttributes, information.nNumberOfLinks, information.nFileSizeHigh,
                 information.nFileSizeLow, GetLastError());
    if (valid) {
        bytes.resize(4097);
        DWORD read = 0;
        valid = ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read <= 4096;
        bytes.resize(int(read));
        if (!valid)
            qWarning("Display descriptor read failed: count=%lu error=%lu", read, GetLastError());
    }
    CloseHandle(file);
    return valid;
}

bool NativeDisplayPlatform::privateBusAddress(const QString &address)
{
    QString path;
    if (!decodeDisplayBusPath(address, path) || !privateDirectory(QFileInfo(path).absolutePath()))
        return false;
    HANDLE socket = openMetadata(path, FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (socket == INVALID_HANDLE_VALUE)
        return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const bool valid = GetFileInformationByHandleEx(socket, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        attributes.ReparseTag == IO_REPARSE_TAG_AF_UNIX && privateAcl(socket);
    CloseHandle(socket);
    return valid;
}
