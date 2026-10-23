#pragma once

#include "DisplayPointerState.h"
#include <QMutex>

class DisplayPointerStateStore {
public:
    static constexpr int MaximumCursorDimension = 256;
    bool defineCursor(int width, int height, int hotX, int hotY,
                      const unsigned char *bytes, quint64 size);
    bool setPosition(int x, int y, int visible);
    void clear();
    DisplayPointerState snapshot(bool absolute) const;

private:
    mutable QMutex mutex_;
    DisplayPointerState state_;
};
