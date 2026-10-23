#pragma once
#include "FrameStore.h"
#include <atomic>

// Owns a duplicate of QEMU's shared-memory FD. Bounded pread copies avoid SIGBUS
// exposure from mmap of the unsealed files exported by QEMU's Unix.Map API.
class SharedFrameSource {
public:
    explicit SharedFrameSource(FrameStore &frames, const std::atomic<bool> &stopped);
    ~SharedFrameSource();
    bool replace(qintptr ownedHandle, quint32 offset, quint32 width, quint32 height,
                 quint32 stride, quint32 format);
    bool update(int x, int y, int width, int height);
    void clear();
private:
    bool readRows(int x, int y, int width, int height, QByteArray &bytes);
    FrameStore &frames_;
    const std::atomic<bool> &stopped_;
    qintptr fd_ = -1;
#ifdef Q_OS_WIN
    const unsigned char *view_ = nullptr;
    quint64 viewBytes_ = 0;
#endif
    quint64 offset_ = 0;
    quint32 width_ = 0;
    quint32 height_ = 0;
    quint32 stride_ = 0;
    quint32 format_ = 0;
};
