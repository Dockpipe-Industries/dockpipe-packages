#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

namespace dockpipe::desktop {
class ChildProcess final {
public:
    // An optional private canonical TMPDIR is applied only to this spawn.
    // The parent's environment and the default spawn path remain unchanged.
    static std::unique_ptr<ChildProcess> start(const std::vector<std::string> &arguments,
        bool channelDescriptors = false, const std::string &temporaryDirectory = {});
    ~ChildProcess();
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;
    int input() const { return m_input; }
    int output() const { return m_output; }
    int control() const { return m_control; }
    int status() const { return m_status; }
    bool stop();
    // An explicit deadline cannot be renewed by a later call or destructor.
    bool stop(std::chrono::steady_clock::time_point deadline);
    bool exited();
    // Admission evidence only. Never use this number to signal after reaping.
    pid_t pid() const { return m_pid; }
private:
    ChildProcess() = default;
    bool stopWithin(std::chrono::steady_clock::time_point deadline);
    bool childIdentityPinned();
    pid_t m_pid = -1;
    int m_input = -1;
    int m_output = -1;
    int m_control = -1;
    int m_status = -1;
    bool m_reaped = false;
    bool m_groupVerified = false;
    std::optional<std::chrono::steady_clock::time_point> m_explicitStopDeadline;
};

// All writes are finite; their deadline is checked at most every50ms.
bool writeBytes(int descriptor, const std::uint8_t *bytes, std::size_t length, std::chrono::steady_clock::time_point deadline);
bool writeRecord(int descriptor, const std::vector<std::uint8_t> &value, std::size_t maximum, std::chrono::steady_clock::time_point deadline);
class RecordReader final {
public:
    enum class Outcome { Pending, Complete, Closed, Invalid };
    Outcome poll(int descriptor, std::size_t maximum, bool lines = false);
    std::vector<std::uint8_t> take();
private:
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_expected = 0;
};
} // namespace dockpipe::desktop
