#include "SharedFrameSource.h"
#include <sys/stat.h>
#ifdef Q_OS_LINUX
#include <sys/vfs.h>
#include <linux/magic.h>
#endif
#include <unistd.h>
#include <cerrno>

SharedFrameSource::SharedFrameSource(FrameStore &frames, const std::atomic<bool> &stopped)
    : frames_(frames), stopped_(stopped) {}

SharedFrameSource::~SharedFrameSource()
{
    clear();
}

void SharedFrameSource::clear()
{
    if (fd_ >= 0)
        close(fd_);
    fd_ = -1;
}

bool SharedFrameSource::replace(qintptr ownedFd, quint32 offset, quint32 width,
                                quint32 height, quint32 stride, quint32 format)
{
    clear();
    fd_ = ownedFd;
    struct stat info{};
#ifdef Q_OS_LINUX
    struct statfs filesystem{};
#endif
    const quint64 payload = quint64(stride) * height;
    const bool valid = fd_ >= 0 && width && height &&
        width <= FrameStore::MaximumDimension && height <= FrameStore::MaximumDimension &&
        stride >= quint64(width) * 4 && stride % 4 == 0 &&
        payload <= FrameStore::MaximumBytes && quint64(offset) + payload <= 256 * 1024 * 1024 &&
        (format == 0x20020888 || format == 0x20028888) &&
        fstat(fd_, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == getuid() &&
        info.st_nlink == 0 && info.st_size >= 0 && quint64(info.st_size) >= quint64(offset) + payload
#ifdef Q_OS_LINUX
        && fstatfs(fd_, &filesystem) == 0 && filesystem.f_type == TMPFS_MAGIC
#endif
        ;
    if (!valid) {
        clear();
        return false;
    }
    offset_ = offset;
    width_ = width;
    height_ = height;
    stride_ = stride;
    format_ = format;
    QByteArray bytes;
    if (!readRows(0, 0, int(width), int(height), bytes) ||
        !frames_.scanout(width, height, width * 4, format,
                        reinterpret_cast<const unsigned char *>(bytes.constData()), bytes.size())) {
        clear();
        return false;
    }
    return true;
}

bool SharedFrameSource::readRows(int x, int y, int width, int height, QByteArray &bytes)
{
    if (fd_ < 0 || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        quint64(x) + quint64(width) > width_ || quint64(y) + quint64(height) > height_)
        return false;
    const quint64 rowBytes = quint64(width) * 4;
    bytes.resize(qsizetype(rowBytes * quint64(height)));
    for (int row = 0; row < height; ++row) {
        quint64 consumed = 0;
        while (consumed < rowBytes) {
            if (stopped_)
                return false;
            const auto position = offset_ + quint64(y + row) * stride_ + quint64(x) * 4 + consumed;
            const auto count = pread(fd_, bytes.data() + quint64(row) * rowBytes + consumed,
                                     size_t(rowBytes - consumed), off_t(position));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            consumed += quint64(count);
        }
    }
    return true;
}

bool SharedFrameSource::update(int x, int y, int width, int height)
{
    QByteArray bytes;
    if (!readRows(x, y, width, height, bytes) ||
        !frames_.update(x, y, width, height, quint32(width) * 4, format_,
            reinterpret_cast<const unsigned char *>(bytes.constData()), bytes.size())) {
        clear();
        return false;
    }
    return true;
}
