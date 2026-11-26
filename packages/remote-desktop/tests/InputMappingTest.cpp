#include "InputMapping.h"
#include "InputState.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QTest>
#include <array>

#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
#include <QtGui/qguiapplication_platform.h>
#include <X11/XKBlib.h>
#undef KeyPress
#undef KeyRelease
#undef None
#endif

using namespace dockpipe::desktop;

class InputMappingTest : public QObject {
    Q_OBJECT
private slots:
    void physicalRowsAndModifiers();
    void optionalControlCommandPairs();
    void syntheticShiftPunctuation();
    void syntheticKeypadAndUnsupportedKeys();
    void realX11PhysicalNames();
};

void InputMappingTest::optionalControlCommandPairs()
{
    for (std::uint16_t usage = 0; usage < 256; ++usage)
        QCOMPARE(mapControlCommandUsage(usage, false), usage);
    QCOMPARE(mapControlCommandUsage(224, true), std::uint16_t(227));
    QCOMPARE(mapControlCommandUsage(227, true), std::uint16_t(224));
    QCOMPARE(mapControlCommandUsage(228, true), std::uint16_t(231));
    QCOMPARE(mapControlCommandUsage(231, true), std::uint16_t(228));
    for (std::uint16_t usage : {4, 6, 25, 41, 225, 226, 229, 230})
        QCOMPARE(mapControlCommandUsage(usage, true), usage);
}

void InputMappingTest::physicalRowsAndModifiers()
{
    const std::array<const char *, 26> names = {
        "AC01", "AB05", "AB03", "AC03", "AD03", "AC04", "AC05",
        "AC06", "AD08", "AC07", "AC08", "AC09", "AB07", "AB06",
        "AD09", "AD10", "AD01", "AD04", "AC02", "AD05", "AD07",
        "AB04", "AD02", "AB02", "AD06", "AB01"
    };
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto usage = hidForXkbKeyName(QByteArrayView(names[index]));
        QVERIFY(usage);
        QCOMPARE(*usage, std::uint16_t(index + 4));
        QVERIFY(quartzKeyForHid(*usage));
    }
    const std::array<const char *, 8> modifiers = {
        "LCTL", "LFSH", "LALT", "LWIN", "RCTL", "RTSH", "RALT", "RWIN"
    };
    for (std::size_t index = 0; index < modifiers.size(); ++index) {
        const auto usage = hidForXkbKeyName(QByteArrayView(modifiers[index]));
        QVERIFY(usage);
        QCOMPARE(*usage, std::uint16_t(224 + index));
        QVERIFY(quartzKeyForHid(*usage));
    }
    for (const char *unsupported : {"CAPS", "INS", "NMLK", "PRSC", "SCLK", "PAUS", "LSGT", "HKTG", "I129", "", "AD001"})
        QVERIFY(!hidForXkbKeyName(QByteArrayView(unsupported)));
    QCOMPARE(hidForXkbKeyName(QByteArrayView("ESC\0", 4)), std::optional<std::uint16_t>(41));
    QCOMPARE(hidForXkbKeyName(QByteArrayView("UP  ", 4)), std::optional<std::uint16_t>(82));
}

void InputMappingTest::syntheticShiftPunctuation()
{
    InputMapping mapping;
    struct Punctuation {
        int key;
        std::uint16_t usage;
    };
    const Punctuation cases[] = {
        {Qt::Key_Exclam, 30}, {Qt::Key_At, 31}, {Qt::Key_NumberSign, 32},
        {Qt::Key_Dollar, 33}, {Qt::Key_Percent, 34}, {Qt::Key_AsciiCircum, 35},
        {Qt::Key_Ampersand, 36}, {Qt::Key_Asterisk, 37}, {Qt::Key_ParenLeft, 38},
        {Qt::Key_ParenRight, 39}, {Qt::Key_Underscore, 45}, {Qt::Key_Plus, 46},
        {Qt::Key_BraceLeft, 47}, {Qt::Key_BraceRight, 48}, {Qt::Key_Bar, 49},
        {Qt::Key_Colon, 51}, {Qt::Key_QuoteDbl, 52}, {Qt::Key_AsciiTilde, 53},
        {Qt::Key_Less, 54}, {Qt::Key_Greater, 55}, {Qt::Key_Question, 56}
    };
    for (const auto &item : cases) {
        QKeyEvent press(QEvent::KeyPress, item.key, Qt::ShiftModifier);
        QKeyEvent release(QEvent::KeyRelease, item.key, Qt::ShiftModifier);
        QCOMPARE(mapping.hidForNativeKey(press), std::optional<std::uint16_t>(item.usage));
        QCOMPARE(mapping.hidForNativeKey(release), std::optional<std::uint16_t>(item.usage));
        QVERIFY(quartzKeyForHid(item.usage));
    }
    // Every ASCII logical key that can be emitted must remain worker-supported.
    for (int key = 0; key < 128; ++key) {
        QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
        if (const auto usage = mapping.hidForNativeKey(event))
            QVERIFY(quartzKeyForHid(*usage));
    }
}

void InputMappingTest::syntheticKeypadAndUnsupportedKeys()
{
    InputMapping mapping;
    QKeyEvent digit(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier);
    QKeyEvent keypad(QEvent::KeyPress, Qt::Key_1, Qt::KeypadModifier);
    QKeyEvent keypadInsert(QEvent::KeyPress, Qt::Key_Insert, Qt::KeypadModifier);
    QCOMPARE(mapping.hidForNativeKey(digit), std::optional<std::uint16_t>(30));
    QCOMPARE(mapping.hidForNativeKey(keypad), std::optional<std::uint16_t>(89));
    QCOMPARE(mapping.hidForNativeKey(keypadInsert), std::optional<std::uint16_t>(98));
    for (int key : std::array<int, 8>{Qt::Key_CapsLock, Qt::Key_Insert, Qt::Key_NumLock, Qt::Key_Print,
                                    Qt::Key_Pause, Qt::Key_VolumeUp, Qt::Key_unknown, 0x0416}) {
        QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
        QVERIFY(!mapping.hidForNativeKey(event));
    }
    QKeyEvent invalidType(QEvent::ShortcutOverride, Qt::Key_A, Qt::NoModifier);
    QVERIFY(!mapping.hidForNativeKey(invalidType));
    for (quint32 scan : {quint32(1), quint32(7), quint32(256), quint32(0xffffffff)}) {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, scan, 0, 0, QStringLiteral("a"));
        QVERIFY(!mapping.hidForNativeKey(event));
    }
    if (!mapping.nativeMappingAvailable()) {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 38, 0, 0, QStringLiteral("a"));
        QVERIFY(!mapping.hidForNativeKey(event));
    }
}

void InputMappingTest::realX11PhysicalNames()
{
    if (!qEnvironmentVariableIsSet("DOCKPIPE_TEST_INPUT_NATIVE_X11"))
        QSKIP("Explicit read-only native X11 qualification not requested");
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
    auto *native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    QVERIFY(native);
    Display *display = native->display();
    QVERIFY(display);
    XkbDescPtr keyboard = XkbGetMap(display, 0, XkbUseCoreKbd);
    QVERIFY(keyboard);
    const auto freeKeyboard = qScopeGuard([&] { XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True); });
    QCOMPARE(XkbGetNames(display, XkbKeyNamesMask, keyboard), Success);
    QVERIFY(keyboard->names && keyboard->names->keys);
    InputMapping mapping;
    QVERIFY(mapping.nativeMappingAvailable());
    int supported = 0;
    bool sawDigit = false;
    bool sawQ = false;
    bool sawLeftShift = false;
    bool sawRightShift = false;
    for (int code = keyboard->min_key_code; code <= keyboard->max_key_code; ++code) {
        const QByteArrayView name(keyboard->names->keys[code].name, 4);
        const auto expected = hidForXkbKeyName(name);
        // Deliberately unrelated logical character: physical names must win.
        QKeyEvent press(QEvent::KeyPress, Qt::Key_VolumeUp, Qt::ShiftModifier,
                        quint32(code), 0, 0, QStringLiteral("not a physical key"));
        QCOMPARE(mapping.hidForNativeKey(press), expected);
        if (expected) {
            ++supported;
            QVERIFY(quartzKeyForHid(*expected));
            sawDigit = sawDigit || *expected == 30;
            sawQ = sawQ || *expected == 20;
            sawLeftShift = sawLeftShift || *expected == 225;
            sawRightShift = sawRightShift || *expected == 229;
        }
        QKeyEvent repeatRelease(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier,
                                quint32(code), 0, 0, {}, true);
        QCOMPARE(mapping.hidForNativeKey(repeatRelease), expected);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, quint32(code), 0, 0);
        QCOMPARE(mapping.hidForNativeKey(release), expected);
    }
    QVERIFY(supported >= 80);
    QVERIFY(sawDigit && sawQ && sawLeftShift && sawRightShift);
    mapping.reset();
    QVERIFY(mapping.nativeMappingAvailable());
    qInfo() << "Read-only native X11 key-name validation:" << supported << "supported physical keys";
#else
    QFAIL("Native X11 mapping is unsupported in this build");
#endif
}

QTEST_MAIN(InputMappingTest)
#include "InputMappingTest.moc"
