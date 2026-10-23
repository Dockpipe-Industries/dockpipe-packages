#include "DisplayWidget.h"
#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QtTest>
#include <vector>

namespace {
void sendMouse(DisplayWidget &widget, QEvent::Type type, const QPointF &position,
               Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton)
{
    QMouseEvent event(type, position, widget.mapToGlobal(position.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

bool installFrame(FrameStore &frames, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::black);
    return frames.scanout(width, height, quint32(image.bytesPerLine()), 0x20020888,
                          image.constBits(), image.sizeInBytes());
}
}

class DisplayWidgetInputTest : public QObject {
    Q_OBJECT
private slots:
    void scaledAbsoluteCoordinatesReachAllEndpoints()
    {
        FrameStore frames;
        QVERIFY(installFrame(frames, 1920, 1080));
        DisplayWidget widget(frames);
        widget.resize(640, 480);
        std::vector<DisplayInput> input;
        widget.sendInput = [&](DisplayInput value) { input.push_back(value); };
        sendMouse(widget, QEvent::MouseMove, {0, 60});
        sendMouse(widget, QEvent::MouseMove, {639, 60});
        sendMouse(widget, QEvent::MouseMove, {0, 419});
        sendMouse(widget, QEvent::MouseMove, {639, 419});
        sendMouse(widget, QEvent::MouseMove, {319.5, 239.5});
        QCOMPARE(input.size(), size_t(5));
        QCOMPARE(input[0].kind, DisplayInput::Kind::Absolute);
        QCOMPARE(input[0].first, 0);
        QCOMPARE(input[0].second, 0);
        QCOMPARE(input[1].first, 1919);
        QCOMPARE(input[1].second, 0);
        QCOMPARE(input[2].first, 0);
        QCOMPARE(input[2].second, 1079);
        QCOMPARE(input[3].first, 1919);
        QCOMPARE(input[3].second, 1079);
        QCOMPARE(input[4].first, 960);
        QCOMPARE(input[4].second, 540);
        sendMouse(widget, QEvent::MouseMove, {320, 10});
        sendMouse(widget, QEvent::MouseMove, {640, 300});
        QCOMPARE(input.size(), size_t(5));
    }

    void clickPositionsPrecedeButtonsAndDragReleaseClamps()
    {
        FrameStore frames;
        QVERIFY(installFrame(frames, 800, 600));
        DisplayWidget widget(frames);
        widget.resize(400, 400);
        std::vector<DisplayInput> input;
        widget.sendInput = [&](DisplayInput value) { input.push_back(value); };
        sendMouse(widget, QEvent::MouseButtonPress, {399, 349}, Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(input.size(), size_t(2));
        QCOMPARE(input[0].kind, DisplayInput::Kind::Absolute);
        QCOMPARE(input[0].first, 799);
        QCOMPARE(input[0].second, 599);
        QCOMPARE(input[1].kind, DisplayInput::Kind::ButtonPress);
        sendMouse(widget, QEvent::MouseButtonRelease, {-10, 10}, Qt::LeftButton);
        QCOMPARE(input.size(), size_t(4));
        QCOMPARE(input[2].first, 0);
        QCOMPARE(input[2].second, 0);
        QCOMPARE(input[3].kind, DisplayInput::Kind::ButtonRelease);
        sendMouse(widget, QEvent::MouseButtonPress, {200, 10}, Qt::LeftButton, Qt::LeftButton);
        sendMouse(widget, QEvent::MouseButtonRelease, {200, 10}, Qt::LeftButton);
        QCOMPARE(input.size(), size_t(4));
    }

    void relativeCaptureScalesMotionAndReleaseChordReleasesButtons()
    {
        FrameStore frames;
        QVERIFY(installFrame(frames, 800, 600));
        DisplayWidget widget(frames);
        widget.resize(400, 400);
        DisplayPointerState pointer;
        pointer.absolute = false;
        widget.setPointerState(pointer);
        widget.show();
        std::vector<DisplayInput> input;
        widget.sendInput = [&](DisplayInput value) { input.push_back(value); };
        sendMouse(widget, QEvent::MouseMove, {210, 205});
        QVERIFY(input.empty());
        sendMouse(widget, QEvent::MouseButtonPress, {200, 200}, Qt::LeftButton, Qt::LeftButton);
        QVERIFY(widget.pointerCaptured());
        sendMouse(widget, QEvent::MouseButtonRelease, {200, 200}, Qt::LeftButton);
        QVERIFY(input.empty());
        sendMouse(widget, QEvent::MouseMove, {199, 199});
        sendMouse(widget, QEvent::MouseMove, {209, 204});
        QCOMPARE(input.size(), size_t(1));
        QCOMPARE(input[0].kind, DisplayInput::Kind::Relative);
        QCOMPARE(input[0].first, 20);
        QCOMPARE(input[0].second, 10);
        sendMouse(widget, QEvent::MouseButtonPress, {209, 204}, Qt::LeftButton, Qt::LeftButton);
        QKeyEvent release(QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier | Qt::AltModifier);
        QApplication::sendEvent(&widget, &release);
        QVERIFY(!widget.pointerCaptured());
        QCOMPARE(input.back().kind, DisplayInput::Kind::ButtonRelease);
        QCOMPARE(widget.cursor().shape(), Qt::ArrowCursor);
    }

    void relativeRecenteringDoesNotSendReverseWarpMotion()
    {
        FrameStore frames;
        QVERIFY(installFrame(frames, 800, 600));
        DisplayWidget widget(frames);
        widget.resize(400, 400);
        DisplayPointerState pointer;
        pointer.absolute = false;
        widget.setPointerState(pointer);
        widget.show();
        std::vector<DisplayInput> input;
        widget.sendInput = [&](DisplayInput value) { input.push_back(value); };
        sendMouse(widget, QEvent::MouseButtonPress, {200, 200}, Qt::LeftButton, Qt::LeftButton);
        sendMouse(widget, QEvent::MouseMove, {199, 199});
        sendMouse(widget, QEvent::MouseMove, {389, 199});
        sendMouse(widget, QEvent::MouseMove, {199, 199});
        sendMouse(widget, QEvent::MouseMove, {209, 199});
        QCOMPARE(input.size(), size_t(2));
        QCOMPARE(input[0].first, 380);
        QCOMPARE(input[1].first, 20);
        QCOMPARE(input[0].second, 0);
        QCOMPARE(input[1].second, 0);
        widget.releaseInput();
    }

    void relativeFractionalPixelsArePreserved()
    {
        FrameStore frames;
        QVERIFY(installFrame(frames, 100, 100));
        DisplayWidget widget(frames);
        widget.resize(400, 400);
        DisplayPointerState pointer;
        pointer.absolute = false;
        widget.setPointerState(pointer);
        widget.show();
        int total = 0;
        widget.sendInput = [&](DisplayInput value) {
            if (value.kind == DisplayInput::Kind::Relative)
                total += value.first;
        };
        sendMouse(widget, QEvent::MouseButtonPress, {200, 200}, Qt::LeftButton, Qt::LeftButton);
        sendMouse(widget, QEvent::MouseMove, {199, 199});
        for (int x = 200; x <= 203; ++x)
            sendMouse(widget, QEvent::MouseMove, {qreal(x), 199});
        QCOMPARE(total, 1);
        widget.releaseInput();
    }

    void guestHardwareCursorScalesHotspotAndRestoresOutside()
    {
        FrameStore frames;
        QVERIFY(installFrame(frames, 100, 100));
        DisplayWidget widget(frames);
        widget.resize(200, 200);
        widget.show();
        widget.activateWindow();
        QApplication::processEvents();
        QVERIFY(widget.isActiveWindow());
        DisplayPointerState pointer;
        pointer.cursor = QImage(2, 2, QImage::Format_ARGB32);
        pointer.cursor.fill(qRgba(1, 2, 3, 128));
        pointer.hotSpot = QPoint(1, 1);
        widget.setPointerState(pointer);
        sendMouse(widget, QEvent::MouseMove, {10, 10});
        QCOMPARE(widget.cursor().shape(), Qt::BitmapCursor);
        QCOMPARE(widget.cursor().pixmap().size(), QSize(4, 4));
        QCOMPARE(widget.cursor().hotSpot(), QPoint(2, 2));
        pointer.visible = false;
        pointer.visibilityKnown = true;
        widget.setPointerState(pointer);
        sendMouse(widget, QEvent::MouseMove, {10, 10});
        QCOMPARE(widget.cursor().shape(), Qt::BlankCursor);
        sendMouse(widget, QEvent::MouseMove, {210, 10});
        QCOMPARE(widget.cursor().shape(), Qt::ArrowCursor);
        widget.setEnabled(false);
        QCOMPARE(widget.cursor().shape(), Qt::ArrowCursor);
    }

    void focusLossReleasesCaptureAndHeldButton()
    {
        FrameStore frames;
        QVERIFY(installFrame(frames, 800, 600));
        DisplayWidget widget(frames);
        widget.resize(400, 400);
        DisplayPointerState pointer;
        pointer.absolute = false;
        widget.setPointerState(pointer);
        widget.show();
        std::vector<DisplayInput> input;
        widget.sendInput = [&](DisplayInput value) { input.push_back(value); };
        sendMouse(widget, QEvent::MouseButtonPress, {200, 200}, Qt::LeftButton, Qt::LeftButton);
        sendMouse(widget, QEvent::MouseButtonRelease, {200, 200}, Qt::LeftButton);
        sendMouse(widget, QEvent::MouseButtonPress, {200, 200}, Qt::LeftButton, Qt::LeftButton);
        QFocusEvent lost(QEvent::FocusOut);
        QApplication::sendEvent(&widget, &lost);
        QVERIFY(!widget.pointerCaptured());
        QCOMPARE(input.back().kind, DisplayInput::Kind::ButtonRelease);
        QCOMPARE(widget.objectName(), QStringLiteral("dockpipeDisplayInput"));
    }
};
QTEST_MAIN(DisplayWidgetInputTest)
#include "DisplayWidgetInputTest.moc"
