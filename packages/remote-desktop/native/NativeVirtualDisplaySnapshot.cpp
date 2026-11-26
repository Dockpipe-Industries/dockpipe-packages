#include "NativeVirtualDisplaySnapshot.h"
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace dockpipe::desktop {
namespace {
bool sameObject(const struct stat &left, const struct stat &right)
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino
        && left.st_uid == right.st_uid && left.st_mode == right.st_mode;
}
bool sameFile(const struct stat &left, const struct stat &right)
{
#ifdef __APPLE__
    const auto leftTime = left.st_mtimespec;
    const auto rightTime = right.st_mtimespec;
#else
    const auto leftTime = left.st_mtim;
    const auto rightTime = right.st_mtim;
#endif
    return sameObject(left, right) && left.st_size == right.st_size && left.st_nlink == right.st_nlink
        && leftTime.tv_sec == rightTime.tv_sec && leftTime.tv_nsec == rightTime.tv_nsec;
}
bool privateDirectory(int descriptor, struct stat &identity)
{
    return descriptor >= 0 && fstat(descriptor, &identity) == 0 && S_ISDIR(identity.st_mode)
        && identity.st_uid == getuid() && (identity.st_mode & 0777) == 0700;
}
bool canonical(const std::string &path)
{
    char resolved[PATH_MAX];
    return !path.empty() && path.size() < PATH_MAX && path.find('\0') == std::string::npos
        && realpath(path.c_str(), resolved) && path == resolved;
}
} // namespace

NativeVirtualDisplaySnapshot::~NativeVirtualDisplaySnapshot()
{
    for (int descriptor : {m_file, m_directory, m_temporary}) {
        if (descriptor >= 0)
            close(descriptor);
    }
}

bool NativeVirtualDisplaySnapshot::admit(const std::string &temporaryDirectory, const std::string &snapshotPath,
    const std::function<bool(const std::string &)> &verifySnapshot)
{
    if (m_temporary >= 0 || !verifySnapshot || !canonical(temporaryDirectory) || !canonical(snapshotPath))
        return false;
    const auto prefix = temporaryDirectory + "/";
    if (snapshotPath.compare(0, prefix.size(), prefix) != 0)
        return false;
    const auto relative = snapshotPath.substr(prefix.size());
    const auto separator = relative.find('/');
    if (separator == std::string::npos || separator == 0 || relative.substr(separator + 1) != "native-worker-module")
        return false;
    m_directoryName = relative.substr(0, separator);
    if (m_directoryName == "." || m_directoryName == ".." || m_directoryName.size() > 255)
        return false;
    m_temporaryPath = temporaryDirectory;
    m_temporary = open(temporaryDirectory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!privateDirectory(m_temporary, m_temporaryIdentity))
        return false;
    m_directory = openat(m_temporary, m_directoryName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!privateDirectory(m_directory, m_directoryIdentity))
        return false;
    m_file = openat(m_directory, "native-worker-module", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (m_file < 0 || fstat(m_file, &m_fileIdentity) != 0 || !S_ISREG(m_fileIdentity.st_mode)
        || m_fileIdentity.st_uid != getuid() || m_fileIdentity.st_nlink != 1 || m_fileIdentity.st_size <= 0
        || m_fileIdentity.st_size > 128 * 1024 * 1024
        || ((m_fileIdentity.st_mode & 0777) != 0400 && (m_fileIdentity.st_mode & 0777) != 0500))
        return false;
    if (!verifySnapshot(snapshotPath))
        return false;
    struct stat current{};
    if (fstatat(m_directory, "native-worker-module", &current, AT_SYMLINK_NOFOLLOW) != 0
        || !sameFile(current, m_fileIdentity))
        return false;
    struct stat directory{};
    if (fstatat(m_temporary, m_directoryName.c_str(), &directory, AT_SYMLINK_NOFOLLOW) != 0
        || !sameObject(directory, m_directoryIdentity))
        return false;
    struct stat temporary{};
    if (lstat(m_temporaryPath.c_str(), &temporary) != 0 || !sameObject(temporary, m_temporaryIdentity))
        return false;
    m_admitted = true;
    return true;
}

bool NativeVirtualDisplaySnapshot::cleanupAfterReap()
{
    if (!m_admitted)
        return false;
    struct stat temporary{};
    if (lstat(m_temporaryPath.c_str(), &temporary) != 0 || !sameObject(temporary, m_temporaryIdentity))
        return false;
    struct stat directory{};
    if (fstatat(m_temporary, m_directoryName.c_str(), &directory, AT_SYMLINK_NOFOLLOW) != 0) {
        // A normally returning host removes its own QTemporaryDir. An open
        // snapshot with no directory entry independently confirms that removal.
        struct stat file{};
        return errno == ENOENT && fstat(m_file, &file) == 0 && file.st_nlink == 0
            && rmdir(m_temporaryPath.c_str()) == 0;
    }
    if (!sameObject(directory, m_directoryIdentity))
        return false;
    struct stat file{};
    if (fstatat(m_directory, "native-worker-module", &file, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!sameFile(file, m_fileIdentity) || unlinkat(m_directory, "native-worker-module", 0) != 0)
            return false;
    } else {
        struct stat retained{};
        if (errno != ENOENT || fstat(m_file, &retained) != 0 || retained.st_nlink != 0)
            return false;
    }
    // These removals fail on unexpected entries; no recursive deletion occurs.
    return unlinkat(m_temporary, m_directoryName.c_str(), AT_REMOVEDIR) == 0
        && rmdir(m_temporaryPath.c_str()) == 0;
}
} // namespace dockpipe::desktop
