#pragma once

#include "WorkerProtocol.h"
#include <string_view>

namespace dockpipe::desktop {
// Local IPC is reliable and Core retains/retries its latest received report.
// Repeating unchanged state on every lease directive only queues stale reports
// ahead of terminal cleanup. Retain one bounded, fully written JSON line; never
// commit a failed write or suppress a changed state/ACK/binding/reference.
class WorkerReports final {
public:
    static constexpr std::size_t MaximumBytes = 1U << 20;

    template<typename Writer>
    bool publish(const Json &report, Writer write)
    {
        auto line = report.dump() + "\n";
        require(line.size() <= MaximumBytes, "Native report exceeds the metadata limit.");
        if (line == m_published)
            return false;
        require(write(std::string_view(line)), "Native control report write failed.");
        m_published = std::move(line);
        return true;
    }

    std::size_t retainedBytes() const
    {
        return m_published.size();
    }

private:
    std::string m_published;
};
} // namespace dockpipe::desktop
