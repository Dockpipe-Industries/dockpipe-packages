#pragma once

#include "WorkerProtocol.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>

namespace dockpipe::desktop {

// One stop request owns one result. Timeout/failure is terminal even if a late
// native callback arrives; global stopped notifications cannot complete it.
class StopCompletion final {
public:
    enum class State { Pending, Verified, Unknown };

    explicit StopCompletion(Clock::time_point deadline) : m_deadline(deadline) {}

    void complete(bool verified, Clock::time_point now = Clock::now())
    {
        auto expected = State::Pending;
        m_state.compare_exchange_strong(expected, verified && now < m_deadline ? State::Verified : State::Unknown);
    }

    bool pending() const { return m_state.load() == State::Pending; }
    bool verified() const { return m_state.load() == State::Verified; }

private:
    const Clock::time_point m_deadline;
    std::atomic<State> m_state{State::Pending};
};

inline bool awaitStopCompletion(StopCompletion &completion, Clock::time_point deadline,
    const std::function<Clock::time_point()> &now, const std::function<void(Clock::time_point)> &pump)
{
    while (completion.pending()) {
        const auto current = now();
        if (current >= deadline) {
            completion.complete(false);
            break;
        }
        pump(std::min(deadline, current + std::chrono::milliseconds(5)));
    }
    return completion.verified();
}

// The future owns and joins the relay stop even when native waiting throws.
// No caller may read/stop that child again until this function has returned.
inline bool finishCleanupConcurrently(const std::function<bool()> &waitNative, const std::function<bool()> &stopChild)
{
    auto child = std::async(std::launch::async, stopChild);
    const bool nativeVerified = waitNative();
    return child.get() && nativeVerified;
}

inline bool exactCleanupForget(const Json &message, const Json &reference, bool cleanupVerified)
{
    return cleanupVerified && message.is_object() && message.size() == 2
        && message.contains("type") && message["type"] == "forget" && message.contains("reference")
        && message["reference"] == reference && validReference(reference);
}

// An already queued directive may precede Forget after local cleanup. It is
// discarded without acting on its signals/lease; only the exact original open
// can be ignored this way, and it can never turn into a fresh session.
inline bool queuedOriginalDirective(const Json &message, const Json &open)
{
    return message.is_object() && message.size() == 2 && message.contains("type") && message["type"] == "directive"
        && message.contains("directive") && message["directive"].is_object()
        && message["directive"].contains("open") && message["directive"]["open"] == open;
}

} // namespace dockpipe::desktop
