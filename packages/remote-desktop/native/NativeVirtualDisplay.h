#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dockpipe::desktop {

struct NativeVirtualDisplaySpec {
    unsigned width = 0;
    unsigned height = 0;
    std::uint32_t serial = 0; // Zero keeps direct/probe random-serial behavior.
};

struct NativeVirtualDisplayResult {
    bool success = false;
    bool cleanupVerified = false;
    std::uint32_t display = 0;
    std::string error;
};

class NativeVirtualDisplay final {
public:
    struct Observation {
        bool querySucceeded = false;
        bool online = false;
        bool matches = false;
    };
    struct Creation {
        bool effectsPossible = false;
        std::uint32_t display = 0;
    };
    using Clock = std::chrono::steady_clock;
    using SelectionAdmission = std::function<bool()>;
    // OS boundary also used by no-display fixtures. No remotely selected adapter.
    class Platform {
    public:
        virtual ~Platform() = default;
        virtual bool available() = 0;
        virtual bool ownerAlive()
        {
            return true;
        }
        virtual Creation create(NativeVirtualDisplaySpec spec) = 0;
        virtual Creation createWithAdmission(NativeVirtualDisplaySpec spec, Clock::time_point deadline,
            const SelectionAdmission &admission)
        {
            (void)deadline;
            return admission && admission() ? create(spec) : Creation{};
        }
        virtual Observation observe(std::uint32_t display, NativeVirtualDisplaySpec spec) = 0;
        // Creation-only configuration. Recheck admission immediately before a
        // mode setter; observe() stays a pure query during cleanup.
        virtual Observation configureAndObserve(std::uint32_t display, NativeVirtualDisplaySpec spec,
            const SelectionAdmission &)
        {
            return observe(display, spec);
        }
        virtual void release() = 0;
        virtual void releaseBefore(Clock::time_point)
        {
            release();
        }
    };
    using Completion = std::function<void(NativeVirtualDisplayResult)>;

    NativeVirtualDisplay();
    explicit NativeVirtualDisplay(std::shared_ptr<Platform> platform);
    ~NativeVirtualDisplay();
    NativeVirtualDisplay(const NativeVirtualDisplay &) = delete;
    NativeVirtualDisplay &operator=(const NativeVirtualDisplay &) = delete;

    static bool available();
    bool ownerAlive() const;
    void create(NativeVirtualDisplaySpec spec, Clock::time_point deadline, Completion completion);
    // Caller must first verify capture, encoder and held-input release. The
    // supplied deadline is the caller's existing overall cleanup deadline.
    void close(Clock::time_point deadline, Completion completion);

private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace dockpipe::desktop
