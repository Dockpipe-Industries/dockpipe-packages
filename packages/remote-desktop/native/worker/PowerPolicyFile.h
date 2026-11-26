#pragma once

#include "PowerPolicy.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <optional>

namespace dockpipe::desktop {
class PowerPolicyDirectory final {
public:
    explicit PowerPolicyDirectory(const std::string &path)
    {
        m_descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat metadata{};
        if (m_descriptor < 0 || fstat(m_descriptor, &metadata) != 0 || !S_ISDIR(metadata.st_mode)
            || metadata.st_uid != getuid() || (metadata.st_mode & 0777) != 0700) {
            if (m_descriptor >= 0)
                close(m_descriptor);
            throw std::runtime_error("Desktop power state requires a private owned directory.");
        }
    }
    ~PowerPolicyDirectory() { close(m_descriptor); }
    PowerPolicyDirectory(const PowerPolicyDirectory &) = delete;
    PowerPolicyDirectory &operator=(const PowerPolicyDirectory &) = delete;

    std::optional<Json> read(const char *name) const
    {
        const int descriptor = openat(m_descriptor, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (descriptor < 0 && errno == ENOENT)
            return std::nullopt;
        require(descriptor >= 0, "Cannot open desktop power metadata.");
        struct stat metadata{};
        if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != getuid()
            || (metadata.st_mode & 0777) != 0600 || metadata.st_size <= 0 || metadata.st_size > 4096) {
            close(descriptor);
            throw std::runtime_error("Invalid private desktop power metadata.");
        }
        std::string bytes(static_cast<std::size_t>(metadata.st_size), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                close(descriptor);
                throw std::runtime_error("Cannot read desktop power metadata.");
            }
            offset += static_cast<std::size_t>(count);
        }
        char extra;
        const bool complete = ::read(descriptor, &extra, 1) == 0;
        close(descriptor);
        require(complete, "Desktop power metadata changed while reading.");
        return Json::parse(bytes);
    }

    void write(const char *name, const Json &value) const
    {
        const auto bytes = value.dump() + "\n";
        require(bytes.size() <= 4096, "Desktop power metadata exceeds its bound.");
        // Existing symlinks or malformed files are rejected rather than repaired.
        read(name);
        std::string temporary = std::string(name) + ".temporary-" + std::to_string(getpid());
        const int descriptor = openat(m_descriptor, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(descriptor >= 0, "Cannot create desktop power metadata.");
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            offset += static_cast<std::size_t>(count);
        }
        const bool written = offset == bytes.size() && fsync(descriptor) == 0;
        close(descriptor);
        if (!written || renameat(m_descriptor, temporary.c_str(), m_descriptor, name) != 0) {
            unlinkat(m_descriptor, temporary.c_str(), 0);
            throw std::runtime_error("Cannot atomically save desktop power metadata.");
        }
        require(fsync(m_descriptor) == 0, "Cannot synchronize desktop power metadata.");
    }

    int lock() const
    {
        const int descriptor = openat(m_descriptor, "desktop-power.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat metadata{};
        if (descriptor < 0 || fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode)
            || metadata.st_uid != getuid() || (metadata.st_mode & 0777) != 0600) {
            if (descriptor >= 0)
                close(descriptor);
            throw std::runtime_error("Invalid desktop power companion lock.");
        }
        return descriptor;
    }

private:
    int m_descriptor = -1;
};
} // namespace dockpipe::desktop
