#pragma once

#include <QImage>
#include <QMutex>
#include <QString>

// One current software frame. Consumers take implicitly shared snapshots; no event queue
// carries images. A writer detaches at most one current image while a snapshot is painted.
class FrameStore {
public:
    static constexpr quint64 MaximumBytes = 32 * 1024 * 1024;
    static constexpr quint32 MaximumDimension = 4096;
    static bool validLayout(quint32 width, quint32 height, quint32 stride,
                            quint32 format, quint64 size);
    bool resizeCanvas(quint32 width, quint32 height);
    bool scanout(quint32 width, quint32 height, quint32 stride, quint32 format,
                 const unsigned char *data, quint64 size);
    bool update(int x, int y, int width, int height, quint32 stride, quint32 format,
                const unsigned char *data, quint64 size);
    void disable();
    QImage snapshot() const;
    quint64 revisions() const;
    QString error() const;
private:
    bool validate(quint32 width, quint32 height, quint32 stride, quint32 format,
                  quint64 size);
    mutable QMutex mutex_;
    QImage image_;
    quint64 revisions_ = 0;
    QString error_;
};
