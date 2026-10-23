#include "DisplayPointerStateStore.h"
#include "FrameStore.h"
#include <QMutexLocker>

bool DisplayPointerStateStore::defineCursor(int width, int height, int hotX, int hotY,
                                            const unsigned char *bytes, quint64 size)
{
    if (!bytes || width <= 0 || height <= 0 || width > MaximumCursorDimension ||
        height > MaximumCursorDimension || hotX < 0 || hotY < 0 || hotX >= width || hotY >= height ||
        size != quint64(width) * quint64(height) * 4)
        return false;
    QImage image(bytes, width, height, width * 4, QImage::Format_ARGB32);
    auto owned = image.copy();
    if (owned.isNull())
        return false;
    QMutexLocker lock(&mutex_);
    state_.cursor = std::move(owned);
    state_.hotSpot = QPoint(hotX, hotY);
    ++state_.revision;
    return true;
}

bool DisplayPointerStateStore::setPosition(int x, int y, int visible)
{
    constexpr int bound = int(FrameStore::MaximumDimension) + MaximumCursorDimension;
    if (x < -MaximumCursorDimension || y < -MaximumCursorDimension || x > bound || y > bound ||
        (visible != 0 && visible != 1))
        return false;
    QMutexLocker lock(&mutex_);
    state_.position = QPoint(x, y);
    state_.visibilityKnown = true;
    state_.visible = visible != 0;
    ++state_.revision;
    return true;
}

void DisplayPointerStateStore::clear()
{
    QMutexLocker lock(&mutex_);
    const auto revision = state_.revision + 1;
    state_ = {};
    state_.revision = revision;
}

DisplayPointerState DisplayPointerStateStore::snapshot(bool absolute) const
{
    QMutexLocker lock(&mutex_);
    auto result = state_;
    result.absolute = absolute;
    return result;
}
