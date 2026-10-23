#define NOMINMAX
#include <windows.h>
#include "SharedFrameSource.h"

SharedFrameSource::SharedFrameSource(FrameStore &frames, const std::atomic<bool> &stopped)
    : frames_(frames), stopped_(stopped)
{
}

SharedFrameSource::~SharedFrameSource()
{
    clear();
}

void SharedFrameSource::clear()
{
    if (view_)
        UnmapViewOfFile(view_);
    view_ = nullptr;
    viewBytes_ = 0;
    if (fd_ != -1 && fd_ != 0)
        CloseHandle(reinterpret_cast<HANDLE>(fd_));
    fd_ = -1;
}

bool SharedFrameSource::replace(qintptr ownedHandle, quint32 offset, quint32 width,
                                quint32 height, quint32 stride, quint32 format)
{
    clear();
    fd_ = ownedHandle;
    const quint64 payload = quint64(stride) * height;
    if (fd_ == -1 || fd_ == 0 || !width || !height ||
        width > FrameStore::MaximumDimension || height > FrameStore::MaximumDimension ||
        stride < quint64(width) * 4 || stride % 4 || payload > FrameStore::MaximumBytes ||
        quint64(offset) + payload > 256 * 1024 * 1024 ||
        (format != 0x20020888 && format != 0x20028888)) {
        clear();
        return false;
    }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (!system.dwAllocationGranularity) {
        clear();
        return false;
    }
    const quint64 baseOffset = offset - offset % system.dwAllocationGranularity;
    offset_ = quint64(offset) - baseOffset;
    viewBytes_ = offset_ + payload;
    // QEMU duplicates a FILE_MAP_READ | SECTION_QUERY mapping handle into this
    // process. Mapping the exact bounded range validates section type/extent;
    // a read-only view cannot alter the producer's framebuffer.
    view_ = static_cast<const unsigned char *>(MapViewOfFile(reinterpret_cast<HANDLE>(fd_),
        FILE_MAP_READ, DWORD(baseOffset >> 32), DWORD(baseOffset), SIZE_T(viewBytes_)));
    if (!view_) {
        clear();
        return false;
    }
    MEMORY_BASIC_INFORMATION region{};
    const bool valid = VirtualQuery(view_, &region, sizeof(region)) == sizeof(region) &&
        region.State == MEM_COMMIT && region.Type == MEM_MAPPED &&
        region.AllocationBase == view_ && region.RegionSize >= viewBytes_ &&
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
    if (!valid) {
        clear();
        return false;
    }
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
    if (!view_ || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        quint64(x) + quint64(width) > width_ || quint64(y) + quint64(height) > height_)
        return false;
    const quint64 rowBytes = quint64(width) * 4;
    bytes.resize(qsizetype(rowBytes * quint64(height)));
    for (int row = 0; row < height; ++row) {
        if (stopped_)
            return false;
        const quint64 position = offset_ + quint64(y + row) * stride_ + quint64(x) * 4;
        if (position + rowBytes > viewBytes_)
            return false;
        SIZE_T read = 0;
        // ReadProcessMemory handles invalidated/in-page-error views as a bounded
        // failure; no unchecked memcpy or compiler-specific SEH is required.
        if (!ReadProcessMemory(GetCurrentProcess(), view_ + position,
            bytes.data() + quint64(row) * rowBytes, SIZE_T(rowBytes), &read) || read != rowBytes)
            return false;
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
