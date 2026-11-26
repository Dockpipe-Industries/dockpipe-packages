#include "InputMapping.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QThread>
#include <array>
#include <utility>

#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
#include <QtGui/qguiapplication_platform.h>
#include <X11/XKBlib.h>
#undef KeyPress
#undef KeyRelease
#undef None
#endif

namespace dockpipe::desktop {
namespace {
using Usage = std::optional<std::uint16_t>;

struct PhysicalKey {
    const char *name;
    std::uint16_t usage;
};

constexpr PhysicalKey physicalKeys[] = {
    {"AE01", 30}, {"AE02", 31}, {"AE03", 32}, {"AE04", 33},
    {"AE05", 34}, {"AE06", 35}, {"AE07", 36}, {"AE08", 37},
    {"AE09", 38}, {"AE10", 39}, {"AE11", 45}, {"AE12", 46},
    {"AD01", 20}, {"AD02", 26}, {"AD03", 8}, {"AD04", 21},
    {"AD05", 23}, {"AD06", 28}, {"AD07", 24}, {"AD08", 12},
    {"AD09", 18}, {"AD10", 19}, {"AD11", 47}, {"AD12", 48},
    {"AC01", 4}, {"AC02", 22}, {"AC03", 7}, {"AC04", 9},
    {"AC05", 10}, {"AC06", 11}, {"AC07", 13}, {"AC08", 14},
    {"AC09", 15}, {"AC10", 51}, {"AC11", 52},
    {"AB01", 29}, {"AB02", 27}, {"AB03", 6}, {"AB04", 25},
    {"AB05", 5}, {"AB06", 17}, {"AB07", 16}, {"AB08", 54},
    {"AB09", 55}, {"AB10", 56},
    {"TLDE", 53}, {"BKSL", 49}, {"ESC", 41}, {"BKSP", 42},
    {"TAB", 43}, {"RTRN", 40}, {"SPCE", 44},
    {"FK01", 58}, {"FK02", 59}, {"FK03", 60}, {"FK04", 61},
    {"FK05", 62}, {"FK06", 63}, {"FK07", 64}, {"FK08", 65},
    {"FK09", 66}, {"FK10", 67}, {"FK11", 68}, {"FK12", 69},
    {"LCTL", 224}, {"LFSH", 225}, {"LALT", 226}, {"LWIN", 227},
    {"RCTL", 228}, {"RTSH", 229}, {"RALT", 230}, {"RWIN", 231},
    {"HOME", 74}, {"PGUP", 75}, {"DELE", 76}, {"END", 77},
    {"PGDN", 78}, {"RGHT", 79}, {"LEFT", 80}, {"DOWN", 81}, {"UP", 82},
    {"KPDV", 84}, {"KPMU", 85}, {"KPSU", 86}, {"KPAD", 87}, {"KPEN", 88},
    {"KP1", 89}, {"KP2", 90}, {"KP3", 91}, {"KP4", 92}, {"KP5", 93},
    {"KP6", 94}, {"KP7", 95}, {"KP8", 96}, {"KP9", 97}, {"KP0", 98},
    {"KPDL", 99}, {"KPEQ", 103},
};

Usage syntheticUsage(const QKeyEvent &event)
{
    const int key = event.key();
    if (event.modifiers().testFlag(Qt::KeypadModifier)) {
        if (key >= Qt::Key_1 && key <= Qt::Key_9)
            return std::uint16_t(89 + key - Qt::Key_1);
        if (key == Qt::Key_0)
            return 98;
        switch (key) {
        case Qt::Key_Slash:
            return 84;
        case Qt::Key_Asterisk:
            return 85;
        case Qt::Key_Minus:
            return 86;
        case Qt::Key_Plus:
            return 87;
        case Qt::Key_Enter:
        case Qt::Key_Return:
            return 88;
        case Qt::Key_Period:
        case Qt::Key_Delete:
            return 99;
        case Qt::Key_Equal:
            return 103;
        case Qt::Key_Insert:
            return 98;
        case Qt::Key_End:
            return 89;
        case Qt::Key_Down:
            return 90;
        case Qt::Key_PageDown:
            return 91;
        case Qt::Key_Left:
            return 92;
        case Qt::Key_Clear:
            return 93;
        case Qt::Key_Right:
            return 94;
        case Qt::Key_Home:
            return 95;
        case Qt::Key_Up:
            return 96;
        case Qt::Key_PageUp:
            return 97;
        default:
            return std::nullopt;
        }
    }
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return std::uint16_t(4 + key - Qt::Key_A);
    if (key >= Qt::Key_1 && key <= Qt::Key_9)
        return std::uint16_t(30 + key - Qt::Key_1);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return std::uint16_t(58 + key - Qt::Key_F1);
    switch (key) {
    case Qt::Key_0:
    case Qt::Key_ParenRight:
        return 39;
    case Qt::Key_Exclam:
        return 30;
    case Qt::Key_At:
        return 31;
    case Qt::Key_NumberSign:
        return 32;
    case Qt::Key_Dollar:
        return 33;
    case Qt::Key_Percent:
        return 34;
    case Qt::Key_AsciiCircum:
        return 35;
    case Qt::Key_Ampersand:
        return 36;
    case Qt::Key_Asterisk:
        return 37;
    case Qt::Key_ParenLeft:
        return 38;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return 40;
    case Qt::Key_Escape:
        return 41;
    case Qt::Key_Backspace:
        return 42;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return 43;
    case Qt::Key_Space:
        return 44;
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        return 45;
    case Qt::Key_Equal:
    case Qt::Key_Plus:
        return 46;
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft:
        return 47;
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight:
        return 48;
    case Qt::Key_Backslash:
    case Qt::Key_Bar:
        return 49;
    case Qt::Key_Semicolon:
    case Qt::Key_Colon:
        return 51;
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl:
        return 52;
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde:
        return 53;
    case Qt::Key_Comma:
    case Qt::Key_Less:
        return 54;
    case Qt::Key_Period:
    case Qt::Key_Greater:
        return 55;
    case Qt::Key_Slash:
    case Qt::Key_Question:
        return 56;
    case Qt::Key_Home:
        return 74;
    case Qt::Key_PageUp:
        return 75;
    case Qt::Key_Delete:
        return 76;
    case Qt::Key_End:
        return 77;
    case Qt::Key_PageDown:
        return 78;
    case Qt::Key_Right:
        return 79;
    case Qt::Key_Left:
        return 80;
    case Qt::Key_Down:
        return 81;
    case Qt::Key_Up:
        return 82;
    case Qt::Key_Control:
        return 224;
    case Qt::Key_Shift:
        return 225;
    case Qt::Key_Alt:
        return 226;
    case Qt::Key_Meta:
        return 227;
    case Qt::Key_AltGr:
        return 230;
    default:
        return std::nullopt;
    }
}
} // namespace

Usage hidForXkbKeyName(QByteArrayView name)
{
    if (name.size() > 4)
        return std::nullopt;
    while (!name.empty() && (name.back() == '\0' || name.back() == ' '))
        name = name.first(name.size() - 1);
    for (const auto &key : physicalKeys) {
        if (name == QByteArrayView(key.name))
            return key.usage;
    }
    return std::nullopt;
}

class InputMapping::Impl final : public QAbstractNativeEventFilter {
public:
    Impl()
    {
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
        auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
        if (!application || QThread::currentThread() != application->thread()
            || QGuiApplication::platformName() != QStringLiteral("xcb"))
            return;
        auto *native = application->nativeInterface<QNativeInterface::QX11Application>();
        if (!native)
            return;
        display = native->display();
        int opcode = 0;
        int error = 0;
        int major = XkbMajorVersion;
        int minor = XkbMinorVersion;
        if (!display || !XkbQueryExtension(display, &opcode, &eventBase, &error, &major, &minor)) {
            display = nullptr;
            return;
        }
        const unsigned int events = XkbNewKeyboardNotifyMask | XkbMapNotifyMask | XkbNamesNotifyMask;
        if (!XkbSelectEvents(display, XkbUseCoreKbd, events, events)) {
            display = nullptr;
            return;
        }
        owner = application;
        owner->installNativeEventFilter(this);
        XFlush(display);
#endif
    }

    ~Impl() override
    {
        if (owner)
            owner->removeNativeEventFilter(this);
    }

    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override
    {
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
        if (type == QByteArrayLiteral("xcb_generic_event_t") && message) {
            const auto *bytes = static_cast<const unsigned char *>(message);
            const int eventType = bytes[0] & 0x7f;
            if (eventType == MappingNotify || (eventType == eventBase
                && (bytes[1] == XkbNewKeyboardNotify || bytes[1] == XkbMapNotify
                    || bytes[1] == XkbNamesNotify)))
                dirty = true;
        }
#else
        Q_UNUSED(type);
        Q_UNUSED(message);
#endif
        return false;
    }

    void refresh()
    {
        if (!dirty)
            return;
        dirty = false;
        available = false;
        keys.fill(std::nullopt);
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
        if (!owner || !display || QThread::currentThread() != owner->thread())
            return;
        XkbDescPtr keyboard = XkbGetMap(display, 0, XkbUseCoreKbd);
        if (!keyboard)
            return;
        if (XkbGetNames(display, XkbKeyNamesMask, keyboard) == Success
            && keyboard->names && keyboard->names->keys
            && keyboard->min_key_code >= 8 && keyboard->max_key_code < keys.size()) {
            for (int code = keyboard->min_key_code; code <= keyboard->max_key_code; ++code)
                keys[code] = hidForXkbKeyName(QByteArrayView(keyboard->names->keys[code].name, 4));
            available = true;
        }
        XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True);
#endif
    }

    QPointer<QGuiApplication> owner;
    std::array<Usage, 256> keys{};
    std::array<Usage, 256> pressed{};
    bool dirty = true;
    bool available = false;
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
    Display *display = nullptr;
    int eventBase = -1;
#endif
};

std::uint16_t mapControlCommandUsage(std::uint16_t usage, bool swapControlCommand)
{
    if (!swapControlCommand)
        return usage;
    switch (usage) {
    case 224: return 227;
    case 227: return 224;
    case 228: return 231;
    case 231: return 228;
    default: return usage;
    }
}

InputMapping::InputMapping()
    : m_impl(std::make_unique<Impl>())
{
}
InputMapping::~InputMapping() = default;

Usage InputMapping::hidForNativeKey(const QKeyEvent &event)
{
    if (event.type() != QEvent::KeyPress && event.type() != QEvent::KeyRelease)
        return std::nullopt;
    const auto code = event.nativeScanCode();
    if (code == 0)
        return syntheticUsage(event);
    if (code < 8 || code >= m_impl->keys.size())
        return std::nullopt;
    // Keep release/repeat paired with the physical press even if the key map
    // changes while a key is held. Qt's autorepeat release is not a real keyup.
    if (m_impl->pressed[code]) {
        const auto usage = m_impl->pressed[code];
        if (event.type() == QEvent::KeyRelease && !event.isAutoRepeat())
            m_impl->pressed[code].reset();
        return usage;
    }
    m_impl->refresh();
    const auto usage = m_impl->keys[code];
    if (event.type() == QEvent::KeyPress)
        m_impl->pressed[code] = usage;
    return usage;
}

bool InputMapping::nativeMappingAvailable()
{
    m_impl->refresh();
    return m_impl->available;
}

void InputMapping::reset()
{
    m_impl->pressed.fill(std::nullopt);
    m_impl->dirty = true;
}

} // namespace dockpipe::desktop
