#include "DisplayWidget.h"
#include "NativeKeyMap.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QGuiApplication>
#include <QCursor>
#include <QHideEvent>
#include <QPixmap>
#include <QtMath>

namespace {
bool containsPointer(const QRect &target, const QPointF &position)
{
    return qIsFinite(position.x()) && qIsFinite(position.y()) &&
        position.x() >= target.x() && position.y() >= target.y() &&
        position.x() < target.x() + target.width() && position.y() < target.y() + target.height();
}
}

DisplayWidget::DisplayWidget(FrameStore &frames, QWidget *parent)
    : QWidget(parent), frames_(frames)
{
    setObjectName(QStringLiteral("dockpipeDisplayInput"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    auto *refresh = new QTimer(this);
    connect(refresh, &QTimer::timeout, this, [this] {
        if (lastAbsolute_ != absolutePointer) {
            releaseInput();
            lastAbsolute_ = absolutePointer;
        }
        updatePointerCursor(mapFromGlobal(QCursor::pos()));
        update();
    });
    refresh->start(33);
    setProperty("pointerCaptured", false);
    setProperty("relativeCaptureSupported", relativeCaptureSupported());
    setToolTip(QStringLiteral("Absolute pointer follows the guest. In relative mode, click to capture; Ctrl+Alt+G releases the pointer."));
    resize(1024, 768);
}

QRect DisplayWidget::imageRect(const QSize &frameSize) const
{
    if (frameSize.isEmpty())
        return {};
    const auto size = frameSize.scaled(this->size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
}

void DisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    const auto frame = frames_.snapshot();
    if (frame.isNull())
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Waiting for local display"));
    else {
        const auto target = imageRect(frame.size());
        painter.drawImage(target, frame);
        // Relative capture hides the recentering host pointer. If QEMU supplies
        // authoritative hardware-cursor feedback, render it in guest space.
        if (!absolutePointer && captured_ && pointerState_.visibilityKnown &&
            pointerState_.visible && !pointerState_.cursor.isNull()) {
            const auto scaleX = qreal(target.width()) / frame.width();
            const auto scaleY = qreal(target.height()) / frame.height();
            const QPointF origin(target.x() + (pointerState_.position.x() - pointerState_.hotSpot.x()) * scaleX,
                                 target.y() + (pointerState_.position.y() - pointerState_.hotSpot.y()) * scaleY);
            painter.save();
            painter.setClipRect(target);
            painter.drawImage(QRectF(origin, QSizeF(pointerState_.cursor.width() * scaleX,
                                                   pointerState_.cursor.height() * scaleY)), pointerState_.cursor);
            painter.restore();
        }
    }
}

void DisplayWidget::key(QKeyEvent *event, bool pressed)
{
    if (event->isAutoRepeat() || !sendInput)
        return;
    const int qnum = qemuKeyNumber(QGuiApplication::platformName(), event->nativeScanCode(),
                                  event->nativeVirtualKey());
    if (!qnum)
        return;
    if (pressed)
        pressedKeys_.insert(qnum);
    else
        pressedKeys_.remove(qnum);
    sendInput({pressed ? DisplayInput::Kind::KeyPress : DisplayInput::Kind::KeyRelease, qnum});
    event->accept();
}

void DisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (captured_ && event->key() == Qt::Key_G &&
        event->modifiers().testFlag(Qt::ControlModifier) && event->modifiers().testFlag(Qt::AltModifier)) {
        releaseInput();
        event->accept();
        return;
    }
    key(event, true);
}
void DisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    key(event, false);
}

void DisplayWidget::setPointerState(const DisplayPointerState &state)
{
    if (absolutePointer != state.absolute)
        releaseInput();
    pointerState_ = state;
    absolutePointer = state.absolute;
    lastAbsolute_ = state.absolute;
    if (state.absolute)
        setToolTip(QStringLiteral("The pointer follows the guest display."));
    else if (relativeCaptureSupported())
        setToolTip(QStringLiteral("Click to capture the pointer. Ctrl+Alt+G releases it."));
    else
        setToolTip(QStringLiteral("Relative pointer capture is unavailable on Wayland. Use the guest's absolute tablet pointer."));
    setProperty("pointerMode", state.absolute ? "absolute" : "relative");
    setProperty("pointerReleaseHint", state.absolute ? "" : "Ctrl+Alt+G releases the pointer");
    setProperty("guestCursorDefined", !state.cursor.isNull());
    setProperty("guestCursorVisible", state.visible);
    setProperty("guestCursorVisibilityKnown", state.visibilityKnown);
    updatePointerCursor(mapFromGlobal(QCursor::pos()));
}

bool DisplayWidget::pointerCaptured() const
{
    return captured_;
}

bool DisplayWidget::relativeCaptureSupported() const
{
    return !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
}

bool DisplayWidget::absolutePosition(const QPointF &position, bool clamp)
{
    const auto frame = frames_.snapshot();
    const auto target = imageRect(frame.size());
    if (!sendInput || frame.isNull() || target.isEmpty() ||
        (!clamp && !containsPointer(target, position)) || !qIsFinite(position.x()) || !qIsFinite(position.y()))
        return false;
    // Pixel endpoints, not the outer rectangle edge, map to pixel endpoints.
    // This reaches the final guest pixel even when the image is scaled down.
    const auto mapAxis = [](qreal value, int origin, int displayed, int pixels) {
        if (displayed <= 1 || pixels <= 1)
            return 0;
        const auto fraction = qBound(qreal(0), (value - origin) / (displayed - 1), qreal(1));
        return qRound(fraction * (pixels - 1));
    };
    sendInput({DisplayInput::Kind::Absolute,
               mapAxis(position.x(), target.x(), target.width(), frame.width()),
               mapAxis(position.y(), target.y(), target.height(), frame.height())});
    return true;
}

void DisplayWidget::capturePointer()
{
    if (!relativeCaptureSupported()) {
        setToolTip(QStringLiteral("Relative pointer capture is unavailable on Wayland. Use the guest's absolute tablet pointer."));
        return;
    }
    setFocus(Qt::MouseFocusReason);
    captured_ = true;
    setProperty("pointerCaptured", true);
    captureCenter_ = rect().center();
    previousPointer_ = captureCenter_;
    relativeRemainder_ = {};
    warpPending_ = true;
    grabMouse(Qt::BlankCursor);
    QCursor::setPos(mapToGlobal(captureCenter_));
}

void DisplayWidget::releasePointerCapture()
{
    captured_ = false;
    warpPending_ = false;
    relativeRemainder_ = {};
    setProperty("pointerCaptured", false);
    if (QWidget::mouseGrabber() == this)
        releaseMouse();
    unsetCursor();
}

void DisplayWidget::relativeMotion(const QPointF &position, const QSize &frameSize, const QRect &target)
{
    if (warpPending_ && (position - captureCenter_).manhattanLength() < 1) {
        previousPointer_ = captureCenter_;
        warpPending_ = false;
        return;
    }
    if (!qIsFinite(position.x()) || !qIsFinite(position.y()))
        return;
    const auto delta = position - previousPointer_;
    previousPointer_ = position;
    const QPointF scaled(qBound(qreal(-32767), delta.x() * frameSize.width() / target.width() + relativeRemainder_.x(), qreal(32767)),
                         qBound(qreal(-32767), delta.y() * frameSize.height() / target.height() + relativeRemainder_.y(), qreal(32767)));
    const QPoint pixels(qRound(scaled.x()), qRound(scaled.y()));
    relativeRemainder_ = scaled - pixels;
    if (!pixels.isNull())
        sendInput({DisplayInput::Kind::Relative, pixels.x(), pixels.y()});
    // Recenter before the host window/monitor edge limits further movement.
    // Ignore the resulting synthetic center event rather than sending it back.
    if (!warpPending_ && (position.x() < 16 || position.y() < 16 ||
        position.x() >= width() - 16 || position.y() >= height() - 16)) {
        captureCenter_ = rect().center();
        warpPending_ = true;
        QCursor::setPos(mapToGlobal(captureCenter_));
    }
}

void DisplayWidget::updatePointerCursor(const QPointF &position)
{
    const auto frame = frames_.snapshot();
    const auto target = imageRect(frame.size());
    setProperty("displayContentRect", target);
    setProperty("displayGuestSize", frame.size());
    if (!isEnabled() || !isActiveWindow() || frame.isNull() ||
        (!captured_ && !containsPointer(target, position))) {
        unsetCursor();
        return;
    }
    if (absolutePointer && !pointerState_.cursor.isNull() && pointerState_.visible) {
        const auto scaleX = qreal(target.width()) / frame.width();
        const auto scaleY = qreal(target.height()) / frame.height();
        const QSize cursorSize(qBound(1, qRound(pointerState_.cursor.width() * scaleX), 512),
                               qBound(1, qRound(pointerState_.cursor.height() * scaleY), 512));
        const QPoint hotSpot(qBound(0, qRound(pointerState_.hotSpot.x() * scaleX), cursorSize.width() - 1),
                             qBound(0, qRound(pointerState_.hotSpot.y() * scaleY), cursorSize.height() - 1));
        if (guestCursorCacheKey_ != pointerState_.cursor.cacheKey() || guestCursorSize_ != cursorSize ||
            guestCursorHotSpot_ != hotSpot) {
            const auto image = pointerState_.cursor.scaled(cursorSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            guestCursor_ = QCursor(QPixmap::fromImage(image), hotSpot.x(), hotSpot.y());
            guestCursorCacheKey_ = pointerState_.cursor.cacheKey();
            guestCursorSize_ = cursorSize;
            guestCursorHotSpot_ = hotSpot;
        }
        setCursor(guestCursor_);
    } else if (absolutePointer || captured_) {
        // Match QEMU's software-cursor policy only within the active guest.
        // CursorDefine supplies a native shape when hardware cursor data exists.
        setCursor(Qt::BlankCursor);
    } else {
        unsetCursor();
    }
}

void DisplayWidget::mouseMoveEvent(QMouseEvent *event)
{
    updatePointerCursor(event->position());
    const auto frame = frames_.snapshot();
    const auto target = imageRect(frame.size());
    if (!sendInput || frame.isNull() || target.isEmpty())
        return;
    if (absolutePointer)
        absolutePosition(event->position(), !pressedButtons_.isEmpty());
    else if (captured_)
        relativeMotion(event->position(), frame.size(), target);
    event->accept();
}

void DisplayWidget::button(QMouseEvent *event, bool pressed)
{
    int value = -1;
    if (event->button() == Qt::LeftButton)
        value = 0;
    else if (event->button() == Qt::MiddleButton)
        value = 1;
    else if (event->button() == Qt::RightButton)
        value = 2;
    if (value < 0 || !sendInput)
        return;
    if (!pressed && !pressedButtons_.contains(value))
        return;
    if (absolutePointer) {
        if (!absolutePosition(event->position(), !pressed))
            return;
    } else if (!captured_) {
        const auto frame = frames_.snapshot();
        if (pressed && !frame.isNull() && containsPointer(imageRect(frame.size()), event->position()))
            capturePointer();
        return;
    }
    if (pressed)
        pressedButtons_.insert(value);
    else
        pressedButtons_.remove(value);
    sendInput({pressed ? DisplayInput::Kind::ButtonPress : DisplayInput::Kind::ButtonRelease, value});
}

void DisplayWidget::mousePressEvent(QMouseEvent *event)
{
    button(event, true);
}
void DisplayWidget::mouseReleaseEvent(QMouseEvent *event)
{
    button(event, false);
}

void DisplayWidget::releaseInput()
{
    if (sendInput) {
        for (int key : pressedKeys_)
            sendInput({DisplayInput::Kind::KeyRelease, key});
        for (int button : pressedButtons_)
            sendInput({DisplayInput::Kind::ButtonRelease, button});
    }
    pressedKeys_.clear();
    pressedButtons_.clear();
    releasePointerCapture();
}

void DisplayWidget::focusOutEvent(QFocusEvent *event)
{
    releaseInput();
    QWidget::focusOutEvent(event);
}

void DisplayWidget::leaveEvent(QEvent *event)
{
    if (!captured_)
        unsetCursor();
    QWidget::leaveEvent(event);
}

void DisplayWidget::hideEvent(QHideEvent *event)
{
    releaseInput();
    QWidget::hideEvent(event);
}

bool DisplayWidget::event(QEvent *event)
{
    if ((event->type() == QEvent::WindowDeactivate || event->type() == QEvent::UngrabMouse) && captured_)
        releaseInput();
    if (event->type() == QEvent::EnabledChange && !isEnabled())
        releaseInput();
    return QWidget::event(event);
}
