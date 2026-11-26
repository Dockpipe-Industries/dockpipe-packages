#include "NativePowerAssertion.h"

#include <utility>

namespace dockpipe::desktop {
namespace {
constexpr unsigned MaximumReleaseAttempts = 3;
constexpr PowerAssertionKind Kinds[]{PowerAssertionKind::System, PowerAssertionKind::Display};
}

NativePowerAssertion::NativePowerAssertion(std::shared_ptr<Platform> platform) : m_platform(std::move(platform))
{
}

NativePowerAssertion::~NativePowerAssertion()
{
    // Callers explicitly inspect release() before claiming verified shutdown.
    // This last bounded attempt cannot convert an earlier failure into proof.
    try {
        release();
    } catch (...) {
        // A destructor cannot report proof. The explicit release result owns it.
    }
}

PowerAssertionResult NativePowerAssertion::resultLocked(bool success, PowerAssertionFailure failure) const
{
    return {success, m_verified && m_identifiers[0] && m_identifiers[1],
        !m_identityUnknown && !m_identifiers[0] && !m_identifiers[1],
        failure == PowerAssertionFailure::Cancelled, failure};
}

PowerAssertionResult NativePowerAssertion::releaseLocked()
{
    m_verified = false;
    // Release in reverse acquisition order, including partial acquisition.
    for (unsigned slot = m_identifiers.size(); slot > 0; --slot) {
        auto &identifier = m_identifiers[slot - 1];
        for (unsigned attempt = 0; identifier && attempt < MaximumReleaseAttempts; ++attempt) {
            bool released = false;
            try {
                released = m_platform && m_platform->release(identifier);
            } catch (...) {
                // Preserve the exact ID and retry within the same fixed bound.
            }
            if (released)
                identifier = 0;
        }
    }
    const bool verified = !m_identityUnknown && !m_identifiers[0] && !m_identifiers[1];
    return resultLocked(verified, verified ? PowerAssertionFailure::None : PowerAssertionFailure::Release);
}

PowerAssertionResult NativePowerAssertion::setActive(bool requested, Cancellation cancelled)
{
    std::lock_guard lock(m_mutex);
    if (!requested)
        return releaseLocked();
    auto interrupted = [&] { return cancelled && cancelled(); };
    auto rollback = [&](PowerAssertionFailure failure) {
        const auto cleanup = releaseLocked();
        auto result = resultLocked(false, cleanup.cleanupVerified ? failure : PowerAssertionFailure::Release);
        result.cancelled = failure == PowerAssertionFailure::Cancelled;
        return result;
    };
    try {
        if (interrupted())
            return rollback(PowerAssertionFailure::Cancelled);
        if (m_verified)
            return resultLocked(true);
        if (m_identityUnknown || m_identifiers[0] || m_identifiers[1]) {
            const auto previous = releaseLocked();
            if (!previous.cleanupVerified)
                return previous; // Never allocate replacements over unknown ownership.
        }
        if (!m_platform)
            return resultLocked(false, PowerAssertionFailure::Unavailable);
        for (unsigned slot = 0; slot < m_identifiers.size(); ++slot) {
            if (interrupted())
                return rollback(PowerAssertionFailure::Cancelled);
            m_identityUnknown = true; // An exception after an OS effect cannot invent absence.
            const auto created = m_platform->create(Kinds[slot]);
            m_identityUnknown = false;
            m_identifiers[slot] = created.identifier;
            if (created.success && !created.identifier)
                m_identityUnknown = true;
            if (!created.success || !created.identifier)
                return rollback(PowerAssertionFailure::Acquire);
            if (!m_platform->verify(created.identifier, Kinds[slot]))
                return rollback(PowerAssertionFailure::Verification);
        }
        if (interrupted())
            return rollback(PowerAssertionFailure::Cancelled);
        m_verified = true;
        return resultLocked(true);
    } catch (...) {
        return rollback(PowerAssertionFailure::Acquire);
    }
}

PowerAssertionResult NativePowerAssertion::release()
{
    std::lock_guard lock(m_mutex);
    return releaseLocked();
}

bool NativePowerAssertion::active() const
{
    std::lock_guard lock(m_mutex);
    return m_verified && m_identifiers[0] && m_identifiers[1];
}

bool NativePowerAssertion::cleanupVerified() const
{
    std::lock_guard lock(m_mutex);
    return !m_identityUnknown && !m_identifiers[0] && !m_identifiers[1];
}

} // namespace dockpipe::desktop
