#include "NativeDisplayPlatform.h"
#include "DisplayAddress.h"
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc_info.h>
#endif

namespace {
bool readBounded(int fd, QByteArray &bytes)
{
    bytes.resize(4097);
    const auto count = read(fd, bytes.data(), size_t(bytes.size()));
    if (count < 0 || count > 4096)
        return false;
    bytes.resize(qsizetype(count));
    return true;
}
}

bool NativeDisplayPlatform::processMatches(int pid, const QString &startToken)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_MACOS
    struct proc_bsdinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) ||
        info.pbi_uid != getuid() || info.pbi_status == 5)
        return false;
    const auto actual = QString::number(info.pbi_start_tvsec) + ":" +
                        QString::number(info.pbi_start_tvusec);
    return startToken == actual;
#else
    static const QRegularExpression decimal(QStringLiteral("^[0-9]{1,20}$"));
    if (!decimal.match(startToken).hasMatch())
        return false;
    const auto path = QStringLiteral("/proc/%1/stat").arg(pid).toUtf8();
    int fd = open(path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat info{};
    QByteArray bytes;
    const bool valid = fstat(fd, &info) == 0 && info.st_uid == getuid() && readBounded(fd, bytes);
    close(fd);
    if (!valid)
        return false;
    const auto endName = bytes.lastIndexOf(')');
    if (endName < 0)
        return false;
    const auto fields = bytes.mid(endName + 2).simplified().split(' ');
    return fields.size() > 19 && fields[0] != "Z" && fields[0] != "X" &&
           fields[19] == startToken.toLatin1();
#endif
}

bool NativeDisplayPlatform::privateDirectory(const QString &path)
{
    const auto encoded = path.toUtf8();
    struct stat info{};
    return QFileInfo(path).canonicalFilePath() == QDir::cleanPath(path) &&
        lstat(encoded.constData(), &info) == 0 && S_ISDIR(info.st_mode) &&
        info.st_uid == getuid() && (info.st_mode & 0777) == 0700;
}

bool NativeDisplayPlatform::protectDirectory(const QString &path)
{
    struct stat info{};
    const auto encoded = path.toUtf8();
    return lstat(encoded.constData(), &info) == 0 && S_ISDIR(info.st_mode) &&
           info.st_uid == getuid() && chmod(encoded.constData(), 0700) == 0;
}

bool NativeDisplayPlatform::readPrivateDescriptor(const QString &directory, QByteArray &bytes)
{
    if (!privateDirectory(directory))
        return false;
    int parent = open(directory.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return false;
    int fd = openat(parent, "display.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    close(parent);
    if (fd < 0)
        return false;
    struct stat info{};
    const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_uid == getuid() && (info.st_mode & 0777) == 0600 && info.st_nlink == 1 &&
        info.st_size > 0 && info.st_size <= 4096 && readBounded(fd, bytes);
    close(fd);
    return valid;
}

bool NativeDisplayPlatform::privateBusAddress(const QString &address)
{
    QString path;
    if (!decodeDisplayBusPath(address, path))
        return false;
    struct stat info{};
    return lstat(path.toUtf8().constData(), &info) == 0 && S_ISSOCK(info.st_mode) &&
        info.st_uid == getuid() && privateDirectory(QFileInfo(path).absolutePath());
}
