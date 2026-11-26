#pragma once

#include <functional>
#include <string>
#include <sys/stat.h>

namespace dockpipe::desktop {

// Retains only the exact private host snapshot admitted before display creation.
// Cleanup is permitted only after the caller has reaped its owned child.
class NativeVirtualDisplaySnapshot {
public:
    NativeVirtualDisplaySnapshot() = default;
    ~NativeVirtualDisplaySnapshot();
    NativeVirtualDisplaySnapshot(const NativeVirtualDisplaySnapshot &) = delete;
    NativeVirtualDisplaySnapshot &operator=(const NativeVirtualDisplaySnapshot &) = delete;

    bool admit(const std::string &temporaryDirectory, const std::string &snapshotPath,
        const std::function<bool(const std::string &)> &verifySnapshot);
    bool cleanupAfterReap();
private:
    int m_temporary = -1;
    int m_directory = -1;
    int m_file = -1;
    std::string m_temporaryPath;
    std::string m_directoryName;
    struct stat m_temporaryIdentity{};
    struct stat m_directoryIdentity{};
    struct stat m_fileIdentity{};
    bool m_admitted = false;
};

} // namespace dockpipe::desktop
