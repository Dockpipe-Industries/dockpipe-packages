#include "InputState.h"
#include <array>
#include <cmath>
#include <limits>

namespace dockpipe::desktop {
std::optional<std::uint16_t> quartzKeyForHid(std::uint16_t usage)
{
    static constexpr std::array<std::uint16_t, 26> letters{
        0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6};
    static constexpr std::array<std::uint16_t, 10> digits{18, 19, 20, 21, 23, 22, 26, 28, 25, 29};
    static constexpr std::array<std::uint16_t, 12> functionKeys{122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111};
    static constexpr std::array<std::uint16_t, 8> modifiers{59, 56, 58, 55, 62, 60, 61, 54};
    if (usage >= 4 && usage <= 29)
        return letters[usage - 4];
    if (usage >= 30 && usage <= 39)
        return digits[usage - 30];
    if (usage >= 58 && usage <= 69)
        return functionKeys[usage - 58];
    if (usage >= 224 && usage <= 231)
        return modifiers[usage - 224];
    switch (usage) {
    case 40: return 36; // Return.
    case 41: return 53; // Escape.
    case 42: return 51; // Backspace.
    case 43: return 48;
    case 44: return 49;
    case 45: return 27;
    case 46: return 24;
    case 47: return 33;
    case 48: return 30;
    case 49: return 42;
    case 51: return 41;
    case 52: return 39;
    case 53: return 50;
    case 54: return 43;
    case 55: return 47;
    case 56: return 44;
    case 74: return 115;
    case 75: return 116;
    case 76: return 117;
    case 77: return 119;
    case 78: return 121;
    case 79: return 124;
    case 80: return 123;
    case 81: return 125;
    case 82: return 126;
    case 84: return 75;
    case 85: return 67;
    case 86: return 78;
    case 87: return 69;
    case 88: return 76;
    case 89: return 83;
    case 90: return 84;
    case 91: return 85;
    case 92: return 86;
    case 93: return 87;
    case 94: return 88;
    case 95: return 89;
    case 96: return 91;
    case 97: return 92;
    case 98: return 82;
    case 99: return 65;
    case 103: return 81;
    default: return std::nullopt; // No invented substitute for an unsupported physical key.
    }
}

InputState::InputState(bool inputAllowed, Clock::time_point expiresAt)
    : m_inputAllowed(inputAllowed), m_expiresAt(expiresAt)
{
}

bool InputState::expired(Clock::time_point now) const
{
    return !m_active || now >= m_expiresAt;
}

bool InputState::renew(Clock::time_point expiresAt, Clock::time_point now)
{
    if (expired(now) || expiresAt <= m_expiresAt)
        return false;
    m_expiresAt = expiresAt;
    return true;
}

std::uint64_t InputState::flags(const std::bitset<256> &keys) const
{
    std::uint64_t value = 0;
    if (keys[225] || keys[229]) value |= 1ULL << 17;
    if (keys[224] || keys[228]) value |= 1ULL << 18;
    if (keys[226] || keys[230]) value |= 1ULL << 19;
    if (keys[227] || keys[231]) value |= 1ULL << 20;
    return value;
}

Result InputState::apply(const InputEvent &event, Clock::time_point now, const Sink &sink)
{
    if (expired(now)) {
        const auto cleanup = disconnect(sink);
        if (!cleanup)
            return cleanup;
        return {Outcome::Cancelled, "The desktop input lease is closed or expired."};
    }
    if (!m_inputAllowed)
        return {Outcome::Failure, "This worker session is view-only."};
    if (m_sequence == std::numeric_limits<std::uint64_t>::max() || event.sequence != m_sequence + 1)
        return {Outcome::Failure, "Desktop input sequence is stale or has a gap."};
    NativeInput native{event.kind};
    native.down = event.down;
    native.button = event.button;
    native.x = event.x;
    native.y = event.y;
    native.scrollX = event.scrollX;
    native.scrollY = event.scrollY;
    auto keys = m_keys;
    auto buttons = m_buttons;
    if (event.kind == InputKind::Key) {
        const auto key = quartzKeyForHid(event.hidUsage);
        if (!key || event.hidUsage >= keys.size())
            return {Outcome::Failure, "Unsupported USB HID keyboard usage."};
        native.quartzKey = *key;
        native.repeated = event.down && keys[event.hidUsage];
        keys[event.hidUsage] = event.down;
    } else {
        if (!std::isfinite(event.x) || !std::isfinite(event.y) || event.x < 0 || event.x > 1 || event.y < 0 || event.y > 1)
            return {Outcome::Failure, "Pointer coordinates must be finite normalized display coordinates."};
        if (event.kind == InputKind::Button) {
            if (event.button >= buttons.size())
                return {Outcome::Failure, "Unsupported mouse button."};
            buttons[event.button] = event.down;
        } else if (event.kind != InputKind::PointerMove && event.kind != InputKind::Scroll) {
            return {Outcome::Failure, "Unknown native input kind."};
        }
    }
    native.modifierFlags = flags(keys);
    native.pressedButtons = buttons;
    if (!sink(native))
        return {Outcome::Failure, "The native input event could not be posted."};
    m_keys = keys;
    m_buttons = buttons;
    if (event.kind != InputKind::Key) {
        m_x = event.x;
        m_y = event.y;
    }
    m_sequence = event.sequence;
    return {Outcome::Success, {}};
}

Result InputState::disconnect(const Sink &sink)
{
    m_active = false;
    bool complete = true;
    // Physical modifier usages sort after ordinary keys, preserving modifiers
    // until the keys they modify have been released.
    for (std::size_t usage = 0; usage < m_keys.size(); ++usage) {
        if (!m_keys[usage])
            continue;
        auto keys = m_keys;
        keys[usage] = false;
        NativeInput release{InputKind::Key};
        release.quartzKey = *quartzKeyForHid(static_cast<std::uint16_t>(usage));
        release.modifierFlags = flags(keys);
        if (sink(release))
            m_keys = keys;
        else
            complete = false;
    }
    for (std::size_t button = 0; button < m_buttons.size(); ++button) {
        if (!m_buttons[button])
            continue;
        NativeInput release{InputKind::Button};
        release.button = static_cast<std::uint8_t>(button);
        release.x = m_x;
        release.y = m_y;
        release.modifierFlags = flags(m_keys);
        if (sink(release))
            m_buttons[button] = false;
        else
            complete = false;
    }
    return complete ? Result{Outcome::Success, {}} : Result{Outcome::Failure, "Native key/button release remains unconfirmed."};
}
} // namespace dockpipe::desktop
