#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace dockpipe::desktop {

enum class PowerAssertionKind { System, Display };
enum class PowerAssertionFailure { None, Cancelled, Unavailable, Acquire, Verification, Release };

struct PowerAssertionResult {
    bool success = false;
    // True means both owned requests were verified active, not that the OS
    // overrides manual sleep, screen locking, or every power-management policy.
    bool active = false;
    bool cleanupVerified = false;
    bool cancelled = false;
    PowerAssertionFailure failure = PowerAssertionFailure::None;
};

class NativePowerAssertion final {
public:
    struct Creation {
        bool success = false;
        std::uint32_t identifier = 0;
    };
    class Platform {
    public:
        virtual ~Platform() = default;
        virtual Creation create(PowerAssertionKind kind) = 0;
        virtual bool verify(std::uint32_t identifier, PowerAssertionKind kind) = 0;
        // Success must mean the owner's sole reference was released. A null
        // properties read alone cannot prove release after an API error.
        virtual bool release(std::uint32_t identifier) = 0;
    };
    using Cancellation = std::function<bool()>;

    NativePowerAssertion();
    explicit NativePowerAssertion(std::shared_ptr<Platform> platform);
    ~NativePowerAssertion();
    NativePowerAssertion(const NativePowerAssertion &) = delete;
    NativePowerAssertion &operator=(const NativePowerAssertion &) = delete;

    PowerAssertionResult setActive(bool requested, Cancellation cancelled = {});
    PowerAssertionResult release();
    bool active() const;
    bool cleanupVerified() const;

private:
    PowerAssertionResult releaseLocked();
    PowerAssertionResult resultLocked(bool success, PowerAssertionFailure failure = PowerAssertionFailure::None) const;

    std::shared_ptr<Platform> m_platform;
    mutable std::mutex m_mutex;
    std::array<std::uint32_t, 2> m_identifiers{};
    bool m_verified = false;
    bool m_identityUnknown = false;
};

} // namespace dockpipe::desktop
