#pragma once

namespace dockpipe::desktop {
class CaptureLifecycle final {
public:
    enum class Phase { Idle, Starting, Running, Stopping, Closed, Unknown };
    enum class StopAction { Complete, AwaitStart, StopNative, AwaitStop };

    bool begin(bool permitted)
    {
        if (!permitted || m_phase != Phase::Idle)
            return false;
        m_phase = Phase::Starting;
        return true;
    }
    // A late successful native start after cancellation must immediately stop;
    // it must never publish frames or turn cancellation into success.
    bool started(bool success)
    {
        if (!success) {
            m_phase = Phase::Closed;
            return false;
        }
        if (m_phase == Phase::Stopping || m_phase == Phase::Unknown || m_phase == Phase::Closed)
            return true;
        if (m_phase == Phase::Starting)
            m_phase = Phase::Running;
        return false;
    }
    StopAction stop()
    {
        if (m_phase == Phase::Idle || m_phase == Phase::Closed)
            return StopAction::Complete;
        if (m_phase == Phase::Starting) {
            m_phase = Phase::Stopping;
            return StopAction::AwaitStart;
        }
        if (m_phase == Phase::Running) {
            m_phase = Phase::Stopping;
            return StopAction::StopNative;
        }
        return StopAction::AwaitStop;
    }
    void stopped(bool verified) { m_phase = verified ? Phase::Closed : Phase::Unknown; }
    bool acceptsFrames() const { return m_phase == Phase::Running; }
    Phase phase() const { return m_phase; }

private:
    Phase m_phase = Phase::Idle;
};
} // namespace dockpipe::desktop
