#pragma once
#include "FrameStore.h"
#include <deque>

// Raw QEMU Update messages can follow a mode change without a matching Scanout.
// Retain a bounded batch until Console.Width/Height have been queried; never
// infer the canvas from damage extents or silently discard missing dirty rows.
class RawFrameAssembler {
public:
    explicit RawFrameAssembler(FrameStore &frames);
    bool enqueue(int x, int y, int width, int height, quint32 stride, quint32 format,
                 const unsigned char *data, quint64 size);
    bool applyGeometry(quint32 width, quint32 height);
    void clear();
private:
    struct Update {
        int x;
        int y;
        int width;
        int height;
        quint32 stride;
        quint32 format;
        QByteArray bytes;
    };
    FrameStore &frames_;
    std::deque<Update> pending_;
    quint64 bytes_ = 0;
};
