#pragma once

#include "../ClipboardProtocol.h"
#include <chrono>
#include <functional>
#include <limits>
#include <stdexcept>

namespace dockpipe::desktop {
struct ClipboardRead {
    std::string text;
    std::string notice;
};

class ClipboardAdapter {
public:
    virtual ~ClipboardAdapter() = default;
    virtual std::int64_t changeCount() = 0;
    virtual ClipboardRead readText() = 0;
    virtual bool writeText(const std::string &text) = 0;
};

// Main-thread state. Admission is repeated immediately before every OS call.
// Changed revision assignment belongs to the serialized writer, after coalescing.
class WorkerClipboard final {
public:
    using Time = std::chrono::steady_clock::time_point;
    WorkerClipboard(ClipboardAdapter &adapter, std::function<bool()> admitted,
        std::function<void(ClipboardRecord)> emit, std::function<void(std::uint64_t)> resetOutput,
        std::function<void(std::uint64_t)> discardObservations, std::function<void()> wrote = {})
        : m_adapter(adapter), m_admitted(std::move(admitted)), m_emit(std::move(emit)),
          m_resetOutput(std::move(resetOutput)), m_discardObservations(std::move(discardObservations)), m_wrote(std::move(wrote)) {}

    void stop()
    {
        m_active = false;
        m_resetOutput(0);
    }

    void retireObservation()
    {
        if (!m_active)
            return;
        access();
        m_discardObservations(m_epoch);
        m_baseline = m_adapter.changeCount();
        check(m_baseline >= 0, "Clipboard generation is unavailable.");
    }

    void receive(const ClipboardRecord &record, Time now)
    {
        // Revalidate even direct callers, not only the channel decoder.
        encodeClipboardRecord(record);
        check(clipboardDirectionAllowed(record.operation, false), "Invalid clipboard direction.");
        check(m_admitted(), "Clipboard authority expired.");
        if (record.operation == ClipboardOperation::Watch) {
            check(record.epoch > m_epoch, "Clipboard epoch was replayed.");
            m_epoch = record.epoch;
            m_writeRevision = 0;
            m_active = false;
            m_resetOutput(m_epoch);
            access();
            m_baseline = m_adapter.changeCount();
            check(m_baseline >= 0, "Clipboard change counter unavailable.");
            m_nextPoll = now + std::chrono::milliseconds(100);
            m_active = true;
            return;
        }
        check(record.epoch == m_epoch, "Clipboard epoch changed.");
        if (record.operation == ClipboardOperation::Pause) {
            stop();
            return;
        }
        check(m_active && record.operation == ClipboardOperation::Write,
            "Clipboard is not armed.");
        check(m_writeRevision != std::numeric_limits<std::uint64_t>::max()
            && record.revision == m_writeRevision + 1, "Clipboard write revision was replayed.");
        m_writeRevision = record.revision;
        access();
        if (!m_adapter.writeText(record.text)) {
            notice("unavailable");
            m_active = false;
            return;
        }
        m_discardObservations(m_epoch);
        access();
        m_baseline = m_adapter.changeCount();
        check(m_baseline >= 0, "Clipboard write baseline unavailable.");
        access();
        if (m_wrote)
            m_wrote();
        m_emit({ClipboardOperation::Acknowledged, m_epoch, record.revision, {}});
    }

    void poll(Time now)
    {
        if (!m_active || now < m_nextPoll)
            return;
        m_nextPoll = now + std::chrono::milliseconds(100);
        access();
        const auto count = m_adapter.changeCount();
        check(count >= 0, "Clipboard change counter unavailable.");
        if (count == m_baseline)
            return;
        m_baseline = count;
        // A newer nontext/invalid value must retire queued older text too.
        m_discardObservations(m_epoch);
        access();
        auto value = m_adapter.readText();
        if (!value.notice.empty()) {
            notice(value.notice);
            return;
        }
        if (value.text.size() > ClipboardMaximumTextBytes) {
            notice("too_large");
            return;
        }
        ClipboardRecord changed{ClipboardOperation::Changed, m_epoch, 1, std::move(value.text)};
        try {
            encodeClipboardRecord(changed);
        } catch (const std::invalid_argument &) {
            notice("unsupported_text");
            return;
        }
        access();
        m_emit(std::move(changed));
    }

private:
    static void check(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
    void access() const { check(m_admitted(), "Clipboard authority expired."); }
    void notice(const std::string &reason)
    {
        access();
        ClipboardRecord record{ClipboardOperation::Notice, m_epoch, 0, reason};
        encodeClipboardRecord(record);
        m_emit(std::move(record));
    }
    ClipboardAdapter &m_adapter;
    std::function<bool()> m_admitted;
    std::function<void(ClipboardRecord)> m_emit;
    std::function<void(std::uint64_t)> m_resetOutput;
    std::function<void(std::uint64_t)> m_discardObservations;
    std::function<void()> m_wrote;
    bool m_active = false;
    std::uint64_t m_epoch = 0;
    std::uint64_t m_writeRevision = 0;
    std::int64_t m_baseline = 0;
    Time m_nextPoll{};
};
} // namespace dockpipe::desktop
