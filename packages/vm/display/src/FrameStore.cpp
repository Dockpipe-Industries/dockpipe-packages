#include "FrameStore.h"
#include <QMutexLocker>
#include <cstring>

namespace {
constexpr quint32 Xrgb32 = 0x20020888;
constexpr quint32 Argb32 = 0x20028888;

void copyOpaqueRow(unsigned char *destination, const unsigned char *source, int width)
{
    std::memcpy(destination, source, size_t(width) * 4);
    auto *pixels = reinterpret_cast<QRgb *>(destination);
    for (int column = 0; column < width; ++column)
        pixels[column] |= 0xff000000;
}
}

bool FrameStore::validLayout(quint32 width, quint32 height, quint32 stride,
                             quint32 format, quint64 size)
{
    return width && height && width <= MaximumDimension && height <= MaximumDimension &&
        stride >= quint64(width) * 4 && stride % 4 == 0 &&
        quint64(stride) * height <= MaximumBytes && size == quint64(stride) * height &&
        (format == Xrgb32 || format == Argb32);
}

bool FrameStore::validate(quint32 width, quint32 height, quint32 stride,
                          quint32 format, quint64 size)
{
    if (validLayout(width, height, stride, format, size))
        return true;
    error_ = QStringLiteral("Unsupported or out-of-bounds software frame");
    return false;
}

bool FrameStore::resizeCanvas(quint32 width, quint32 height)
{
    QMutexLocker lock(&mutex_);
    if (!validate(width, height, width * 4, Xrgb32, quint64(width) * height * 4))
        return false;
    if (image_.width() == int(width) && image_.height() == int(height))
        return true;
    QImage next(int(width), int(height), QImage::Format_RGB32);
    if (next.isNull()) {
        error_ = QStringLiteral("Software frame allocation failed");
        return false;
    }
    next.fill(Qt::black);
    image_ = std::move(next);
    return true;
}

bool FrameStore::scanout(quint32 width, quint32 height, quint32 stride, quint32 format,
                         const unsigned char *data, quint64 size)
{
    QMutexLocker lock(&mutex_);
    if (!data || !validate(width, height, stride, format, size))
        return false;
    QImage next(int(width), int(height), QImage::Format_RGB32);
    if (next.isNull()) {
        error_ = QStringLiteral("Software frame allocation failed");
        return false;
    }
    // QEMU XRGB leaves the high byte unspecified. Qt RGB32 requires it to be
    // opaque, including when a child widget uses a composited paint path.
    for (quint32 row = 0; row < height; ++row)
        copyOpaqueRow(next.scanLine(int(row)), data + quint64(row) * stride, int(width));
    image_ = std::move(next);
    ++revisions_;
    return true;
}

bool FrameStore::update(int x, int y, int width, int height, quint32 stride,
                        quint32 format, const unsigned char *data, quint64 size)
{
    QMutexLocker lock(&mutex_);
    if (image_.isNull() || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        quint64(x) + quint64(width) > quint64(image_.width()) ||
        quint64(y) + quint64(height) > quint64(image_.height())) {
        error_ = QStringLiteral("Software update rectangle outside scanout");
        return false;
    }
    if (!data || !validate(quint32(width), quint32(height), stride, format, size))
        return false;
    for (int row = 0; row < height; ++row)
        copyOpaqueRow(image_.scanLine(y + row) + x * 4, data + quint64(row) * stride, width);
    ++revisions_;
    return true;
}

void FrameStore::disable()
{
    QMutexLocker lock(&mutex_);
    image_ = {};
    ++revisions_;
}

QImage FrameStore::snapshot() const
{
    QMutexLocker lock(&mutex_);
    return image_;
}

quint64 FrameStore::revisions() const
{
    QMutexLocker lock(&mutex_);
    return revisions_;
}

QString FrameStore::error() const
{
    QMutexLocker lock(&mutex_);
    return error_;
}
