#pragma once
#include "NativeDesktopBackend.h"
#include <bitset>
#include <optional>

namespace dockpipe::desktop {
std::optional<std::uint16_t> quartzKeyForHid(std::uint16_t usage);

struct NativeInput {
    InputKind kind;
    std::uint16_t quartzKey = 0;
    bool down = false;
    bool repeated = false;
    std::uint8_t button = 0;
    double x = 0;
    double y = 0;
    std::int16_t scrollX = 0;
    std::int16_t scrollY = 0;
    std::uint64_t modifierFlags = 0;
    std::bitset<3> pressedButtons{};
};

// Pure worker-side admission/state. No native APIs, networking or global key
// state; one ordered input stream and at most 256 keys/3 buttons are retained.
class InputState final {
public:
    using Sink = std::function<bool(const NativeInput &)>;
    InputState(bool inputAllowed, Clock::time_point expiresAt);
    Result apply(const InputEvent &event, Clock::time_point now, const Sink &sink);
    Result disconnect(const Sink &sink);
    bool renew(Clock::time_point expiresAt, Clock::time_point now);
    bool expired(Clock::time_point now) const;
    bool active() const { return m_active; }
    bool inputAllowed() const { return m_inputAllowed; }
    std::size_t pressedCount() const { return m_keys.count() + m_buttons.count(); }

private:
    std::uint64_t flags(const std::bitset<256> &keys) const;
    bool m_active = true;
    const bool m_inputAllowed;
    Clock::time_point m_expiresAt;
    std::uint64_t m_sequence = 0;
    std::bitset<256> m_keys;
    std::bitset<3> m_buttons;
    double m_x = 0;
    double m_y = 0;
};
} // namespace dockpipe::desktop
