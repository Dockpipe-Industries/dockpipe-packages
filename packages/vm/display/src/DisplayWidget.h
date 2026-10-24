#pragma once
#include "FrameStore.h"
#include "DisplayPointerState.h"
#include <QWidget>
#include <QSet>
#include <QCursor>
#include <functional>

struct DisplayInput {
    enum class Kind { KeyPress, KeyRelease, ButtonPress, ButtonRelease, Absolute, Relative };
    Kind kind;
    int first;
    int second = 0;
    QSize frameSize;
};

// Transport-independent widget: no QEMU, GIO, D-Bus or process ownership.
class DisplayWidget : public QWidget {
public:
    explicit DisplayWidget(FrameStore &frames, QWidget *parent = nullptr);
    std::function<void(DisplayInput)> sendInput;
    bool absolutePointer = true;
    void setPointerState(const DisplayPointerState &state);
    void releaseInput();
    bool pointerCaptured() const;
    bool relativeCaptureSupported() const;
protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void leaveEvent(QEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *) override;
private:
    QRect imageRect(const QSize &frameSize) const;
    bool absolutePosition(const QPointF &position, bool clamp);
    void updatePointerCursor(const QPointF &position);
    void capturePointer();
    void releasePointerCapture();
    void relativeMotion(const QPointF &position, const QSize &frameSize, const QRect &target);
    void key(QKeyEvent *, bool pressed);
    void button(QMouseEvent *, bool pressed);
    FrameStore &frames_;
    DisplayPointerState pointerState_;
    QCursor guestCursor_;
    qint64 guestCursorCacheKey_ = 0;
    QSize guestCursorSize_;
    QPoint guestCursorHotSpot_;
    QSet<int> pressedKeys_;
    QSet<int> pressedButtons_;
    QPointF previousPointer_;
    QPointF relativeRemainder_;
    QPoint captureCenter_;
    bool captured_ = false;
    bool warpPending_ = false;
    bool lastAbsolute_ = true;
};
