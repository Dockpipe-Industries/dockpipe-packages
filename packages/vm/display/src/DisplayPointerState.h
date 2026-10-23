#pragma once

#include <QImage>
#include <QPoint>

// Immutable snapshot copied from the transport worker to the GUI thread.
struct DisplayPointerState {
    bool absolute = true;
    QImage cursor;
    QPoint hotSpot;
    QPoint position;
    bool visibilityKnown = false;
    bool visible = true;
    quint64 revision = 0;
};
