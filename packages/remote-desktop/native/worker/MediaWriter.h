#pragma once
#include "NativeDesktopBackend.h"
#include "FrameCodec.h"
#include "ChildProcess.h"
#include "../ClipboardProtocol.h"
#include "../AssetClipboardProtocol.h"
#include <deque>
#include <limits>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace dockpipe::desktop {
class MediaWriter final {
public:
    MediaWriter(int descriptor, std::function<void()> keyframeRequest, H264ProfileBounds bounds = {})
        : m_descriptor(descriptor), m_keyframeRequest(std::move(keyframeRequest)), m_bounds(bounds),
          m_thread([this] { run(); })
    {
    }
    ~MediaWriter() { stop(); }
    void submit(EncodedFrame frame)
    {
        if (frame.annexB.size() > FrameCodec::MaximumBytes - FrameCodec::HeaderBytes) {
            {
                std::lock_guard lock(m_mutex);
                if (m_stopping)
                    return;
                m_pending.reset();
                m_needKeyframe = true;
            }
            m_keyframeRequest();
            return;
        }
        bool request = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping)
                return;
            if (m_pending) {
                m_needKeyframe = true;
                request = true;
                m_pending.reset();
            }
            if (!m_needKeyframe || frame.keyframe) {
                if (frame.keyframe)
                    m_needKeyframe = false;
                m_pending = std::move(frame);
            }
        }
        if (request)
            m_keyframeRequest();
        m_changed.notify_one();
    }
    // Watch/Pause retire all pending clipboard output. In-flight records remain
    // ordered on this same writer and are rejected by the peer's epoch gate.
    void clipboardEpoch(std::uint64_t epoch)
    {
        std::lock_guard lock(m_mutex);
        m_clipboardEpoch = epoch;
        m_clipboardRevision = 0;
        m_acknowledgements.clear();
        m_clipboardChanged.reset();
        m_clipboardNotice.reset();
    }
    void discardClipboardObservations(std::uint64_t epoch)
    {
        std::lock_guard lock(m_mutex);
        if (m_clipboardEpoch == epoch) {
            m_clipboardChanged.reset();
            m_clipboardNotice.reset();
        }
    }
    bool submitClipboard(ClipboardRecord record)
    {
        // Changed's positive revision is a placeholder until dequeue. Validate
        // all other structure and text before retaining any caller memory.
        encodeClipboardRecord(record);
        if (!clipboardDirectionAllowed(record.operation, true))
            return false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping || m_failed || !m_clipboardEpoch || record.epoch != m_clipboardEpoch)
                return false;
            if (record.operation == ClipboardOperation::Acknowledged) {
                if (m_acknowledgements.size() >= 8)
                    return false;
                m_acknowledgements.push_back(std::move(record));
            } else if (record.operation == ClipboardOperation::Changed) {
                m_clipboardChanged = std::move(record);
            } else {
                m_clipboardNotice = std::move(record);
            }
        }
        m_changed.notify_one();
        return true;
    }
    bool submitAsset(AssetClipboardRecord record)
    {
        encodeAssetClipboardRecord(record);
        std::lock_guard lock(m_mutex);
        if (m_stopping || m_failed)
            return false;
        const auto operation = record.operation;
        if (operation == AssetClipboardOperation::EpochAcknowledged) {
            std::erase_if(m_assetUrgent, [](const AssetClipboardRecord &queued) {
                return queued.operation != AssetClipboardOperation::EpochAcknowledged
                    && queued.operation != AssetClipboardOperation::CancelAcknowledged;
            });
            m_assetNormal.clear();
        } else if (operation == AssetClipboardOperation::Cancel
            || operation == AssetClipboardOperation::CancelAcknowledged) {
            const auto discard = [&](const AssetClipboardRecord &queued) {
                return queued.epoch == record.epoch && queued.serial == record.serial
                    && queued.transfer == record.transfer && queued.operation != AssetClipboardOperation::Offer;
            };
            std::erase_if(m_assetNormal, discard);
            std::erase_if(m_assetUrgent, discard);
        }
        const bool urgent = operation == AssetClipboardOperation::EpochAcknowledged
            || operation == AssetClipboardOperation::Cancel || operation == AssetClipboardOperation::CancelAcknowledged
            || operation == AssetClipboardOperation::Committed || operation == AssetClipboardOperation::Accept;
        auto &queue = urgent ? m_assetUrgent : m_assetNormal;
        if (queue.size() >= (urgent ? 8U : 16U))
            return false;
        // A Cancel cannot overtake a queued Offer which introduced this tuple.
        if (operation == AssetClipboardOperation::Cancel || operation == AssetClipboardOperation::CancelAcknowledged) {
            for (auto iterator = m_assetNormal.begin(); iterator != m_assetNormal.end();) {
                if (iterator->operation == AssetClipboardOperation::Offer && iterator->epoch == record.epoch
                    && iterator->serial == record.serial && iterator->transfer == record.transfer) {
                    m_assetUrgent.push_back(std::move(*iterator));
                    iterator = m_assetNormal.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            if (m_assetUrgent.size() >= 8)
                return false;
        }
        queue.push_back(std::move(record));
        m_changed.notify_one();
        return true;
    }
    bool failed() const { return m_failed; }
    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_pending.reset();
            m_acknowledgements.clear();
            m_clipboardChanged.reset();
            m_clipboardNotice.reset();
            m_assetUrgent.clear();
            m_assetNormal.clear();
        }
        m_changed.notify_one();
        if (m_thread.joinable())
            m_thread.join();
    }
private:
    void run()
    {
        std::uint64_t sequence = 0;
        for (;;) {
            EncodedFrame frame;
            std::optional<ClipboardRecord> clipboard;
            std::optional<AssetClipboardRecord> asset;
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock, [this] {
                    return m_stopping || m_pending || !m_acknowledgements.empty()
                        || m_clipboardChanged || m_clipboardNotice || !m_assetUrgent.empty() || !m_assetNormal.empty();
                });
                if (m_stopping)
                    return;
                if (!m_acknowledgements.empty()) {
                    clipboard = std::move(m_acknowledgements.front());
                    m_acknowledgements.pop_front();
                } else if (!m_assetUrgent.empty()) {
                    asset = std::move(m_assetUrgent.front());
                    m_assetUrgent.pop_front();
                } else if (m_pending && m_nonAckClipboardSinceVideo) {
                    frame = std::move(*m_pending);
                    m_pending.reset();
                    m_nonAckClipboardSinceVideo = false;
                } else if (m_clipboardNotice) {
                    clipboard = std::move(m_clipboardNotice);
                    m_clipboardNotice.reset();
                    m_nonAckClipboardSinceVideo = true;
                } else if (m_clipboardChanged) {
                    if (m_clipboardRevision == std::numeric_limits<std::uint64_t>::max()) {
                        m_failed = true;
                        return;
                    }
                    clipboard = std::move(m_clipboardChanged);
                    m_clipboardChanged.reset();
                    clipboard->revision = ++m_clipboardRevision;
                    m_nonAckClipboardSinceVideo = true;
                } else if (!m_assetNormal.empty()) {
                    asset = std::move(m_assetNormal.front());
                    m_assetNormal.pop_front();
                    m_nonAckClipboardSinceVideo = true;
                } else {
                    frame = std::move(*m_pending);
                    m_pending.reset();
                    m_nonAckClipboardSinceVideo = false;
                }
            }
            try {
                if (clipboard) {
                    auto record = encodeClipboardRecord(*clipboard);
                    record.insert(record.begin(), 4);
                    if (!writeRecord(m_descriptor, record, ClipboardHeaderBytes + ClipboardMaximumTextBytes + 1,
                            Clock::now() + std::chrono::milliseconds(250))) {
                        m_failed = true;
                        return;
                    }
                    continue;
                }
                if (asset) {
                    auto record = encodeAssetClipboardRecord(*asset);
                    record.insert(record.begin(), 5);
                    if (!writeRecord(m_descriptor, record, AssetClipboardMaximumRecordBytes + 1,
                            Clock::now() + std::chrono::milliseconds(250))) {
                        m_failed = true;
                        return;
                    }
                    continue;
                }
                VideoFrame envelope;
                envelope.sequence = ++sequence;
                envelope.microseconds = static_cast<std::uint64_t>(frame.presentationMicroseconds);
                envelope.width = frame.width;
                envelope.height = frame.height;
                envelope.keyframe = frame.keyframe;
                envelope.annexB = std::move(frame.annexB);
                auto record = FrameCodec::encode(envelope, m_bounds);
                record.insert(record.begin(), 1);
                if (!writeRecord(m_descriptor, record, FrameCodec::MaximumBytes + 1, Clock::now() + std::chrono::milliseconds(250))) {
                    m_failed = true;
                    return; // A partial record is fatal; never continue a damaged stream.
                }
            } catch (...) {
                m_failed = true;
                return;
            }
        }
    }
    int m_descriptor;
    std::function<void()> m_keyframeRequest;
    const H264ProfileBounds m_bounds;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::optional<EncodedFrame> m_pending;
    std::uint64_t m_clipboardEpoch = 0;
    std::uint64_t m_clipboardRevision = 0;
    std::deque<ClipboardRecord> m_acknowledgements;
    std::deque<AssetClipboardRecord> m_assetUrgent;
    std::deque<AssetClipboardRecord> m_assetNormal;
    std::optional<ClipboardRecord> m_clipboardChanged;
    std::optional<ClipboardRecord> m_clipboardNotice;
    // ACKs always have priority. Observation traffic gets at most one turn
    // before a pending video frame, even when changes keep being replenished.
    bool m_nonAckClipboardSinceVideo = false;
    bool m_stopping = false;
    bool m_needKeyframe = true;
    std::atomic_bool m_failed{false};
    std::thread m_thread;
};
} // namespace dockpipe::desktop
