#pragma once

#include <QByteArrayView>
#include <cstdint>
#include <memory>
#include <optional>

class QKeyEvent;

namespace dockpipe::desktop {

// XKB names describe physical positions independently of the active symbols.
// Only usages accepted by the current native worker are returned.
std::optional<std::uint16_t> hidForXkbKeyName(QByteArrayView name);

// Optional viewer presentation policy; physical key lookup remains unchanged.
std::uint16_t mapControlCommandUsage(std::uint16_t usage, bool swapControlCommand);

// GUI-thread only. Real native events require an available X11 key-name map;
// they never fall back to guessing from a logical character. Synthetic events
// with nativeScanCode == 0 have a limited ASCII/keypad fallback.
class InputMapping final {
public:
    InputMapping();
    ~InputMapping();
    InputMapping(const InputMapping &) = delete;
    InputMapping &operator=(const InputMapping &) = delete;

    std::optional<std::uint16_t> hidForNativeKey(const QKeyEvent &event);
    bool nativeMappingAvailable();

    // Call after releasing the remote held keys on focus/session cleanup.
    // Resetting this local bookkeeping does not send any remote key releases.
    void reset();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dockpipe::desktop
