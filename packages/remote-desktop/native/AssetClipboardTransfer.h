#pragma once
#include "worker/AssetStore.h"
#include "AssetClipboardProtocol.h"
#include <chrono>
#include <atomic>
#include <deque>
#include <functional>
#include <optional>

namespace dockpipe::desktop {
struct LocalAssetSelection {
    AssetClipboardFormat format = AssetClipboardFormat::Files;
    std::int64_t generation = -1;
    std::vector<std::string> paths;
    std::vector<std::uint8_t> image;
    bool ownershipKnown = true;
};
struct PreparedClipboardPng {
    std::vector<std::uint8_t> bytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Single I/O-executor state. OS publication is requested asynchronously and must
// revalidate the local clipboard generation and authority on the native thread.
enum class AssetClipboardEndpointRole { Viewer, Worker };

class AssetClipboardTransfer final {
public:
    using Time = std::chrono::steady_clock::time_point;
    using Emit = std::function<void(AssetClipboardRecord)>;
    using Publish = std::function<void(const AssetClipboardRecord &, const AssetSnapshot &, std::int64_t, std::shared_ptr<std::atomic_bool>)>;
    AssetClipboardTransfer(AssetStore &store, AssetStore::Digests digests, Emit emit, Publish publish,
        std::function<bool()> admitted, std::function<std::int64_t()> generation,
        std::function<std::array<std::uint8_t, 16>()> random,
        std::function<PreparedClipboardPng(const std::vector<std::uint8_t> &)> png,
        bool images, bool files, AssetClipboardEndpointRole role = AssetClipboardEndpointRole::Worker);
    void receive(const AssetClipboardRecord &record, Time now);
    bool active() const { return m_epoch && !m_disabled; }
    bool disabled() const { return m_disabled; }
    std::uint64_t epoch() const { return m_epoch; }
    std::optional<std::int64_t> outgoingGeneration(const AssetClipboardRecord &record) const;
    void localChanged(LocalAssetSelection selection, Time now);
    void publicationCompleted(const AssetClipboardRecord &offer, bool success);
    void arm(std::uint64_t epoch);
    void pause();
    void poll(Time now);
    void maintainOwnership(const std::vector<std::string> &paths, std::int64_t generation, std::int64_t unixSeconds);
    void stop();
private:
    struct Transfer {
        AssetClipboardRecord offer;
        AssetSnapshot snapshot;
        std::unique_ptr<AssetSourceBatch> sources;
        std::vector<std::uint8_t> png;
        std::unique_ptr<AssetDigest> hash;
        std::uint32_t item = 1;
        std::uint64_t offset = 0;
        std::uint64_t credited = 0;
        std::deque<std::uint64_t> boundaries;
        bool accepted = false;
        bool finishing = false;
        bool publishing = false;
        bool cancelling = false;
        bool cancelSent = false;
        unsigned drainedChunks = 0;
        unsigned drainedMetadata = 0;
        std::shared_ptr<std::atomic_bool> publication;
        std::int64_t generation = -1;
        Time progress;
        Time deadline;
    };
    void check(bool value, const char *reason);
    void access();
    void cancel(std::optional<Transfer> &transfer, const std::string &reason, bool notify = true);
    AssetClipboardRecord response(const Transfer &transfer, AssetClipboardOperation operation) const;
    bool same(const Transfer &transfer, const AssetClipboardRecord &record) const;
    void sendNext(Time now);
    bool drainCancellation(std::optional<Transfer> &transfer, const AssetClipboardRecord &record);
    void advancePending(Time now);
    void dispose(Transfer &transfer);
    void focusControl(AssetClipboardRecord control);
    AssetStore &m_store;
    AssetStore::Digests m_digests;
    Emit m_emit;
    Publish m_publish;
    std::function<bool()> m_admitted;
    std::function<std::int64_t()> m_generation;
    std::function<std::array<std::uint8_t, 16>()> m_random;
    std::function<PreparedClipboardPng(const std::vector<std::uint8_t> &)> m_png;
    bool m_images;
    bool m_files;
    AssetClipboardEndpointRole m_role;
    std::uint64_t m_lastEpoch = 0;
    std::uint64_t m_epoch = 0;
    std::uint64_t m_originSerial = 0;
    std::uint64_t m_receiverSerial = 0;
    bool m_disabled = false;
    std::optional<AssetClipboardRecord> m_pendingControl;
    std::deque<AssetClipboardRecord> m_controlAcknowledgements;
    std::optional<LocalAssetSelection> m_pendingSelection;
    std::optional<Transfer> m_outgoing;
    std::optional<Transfer> m_incoming;
};
}
