#include "RawFrameAssembler.h"

RawFrameAssembler::RawFrameAssembler(FrameStore &frames) : frames_(frames)
{
}

bool RawFrameAssembler::enqueue(int x, int y, int width, int height, quint32 stride,
                               quint32 format, const unsigned char *data, quint64 size)
{
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || !data ||
        quint64(x) + quint64(width) > FrameStore::MaximumDimension ||
        quint64(y) + quint64(height) > FrameStore::MaximumDimension ||
        !FrameStore::validLayout(quint32(width), quint32(height), stride, format, size) ||
        pending_.size() >= 128 || size > FrameStore::MaximumBytes - bytes_)
        return false;
    pending_.push_back({x, y, width, height, stride, format,
        QByteArray(reinterpret_cast<const char *>(data), qsizetype(size))});
    bytes_ += size;
    return true;
}

bool RawFrameAssembler::applyGeometry(quint32 width, quint32 height)
{
    if (!frames_.resizeCanvas(width, height)) {
        clear();
        return false;
    }
    for (const auto &update : pending_)
        if (!frames_.update(update.x, update.y, update.width, update.height,
            update.stride, update.format,
            reinterpret_cast<const unsigned char *>(update.bytes.constData()), update.bytes.size())) {
            clear();
            return false;
        }
    clear();
    return true;
}

void RawFrameAssembler::clear()
{
    pending_.clear();
    bytes_ = 0;
}
