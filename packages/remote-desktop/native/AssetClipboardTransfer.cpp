#include "AssetClipboardTransfer.h"
#include <algorithm>
#include <limits>

namespace dockpipe::desktop {
namespace {
std::string hexDigest(const std::array<std::uint8_t, 32> &bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    for (const auto byte : bytes) {
        value += digits[byte >> 4];
        value += digits[byte & 15];
    }
    return value;
}
std::array<std::uint8_t, 32> binaryDigest(const std::string &text)
{
    if (text.size() != 64)
        throw std::runtime_error("Clipboard digest is unavailable.");
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        throw std::runtime_error("Clipboard digest is malformed.");
    };
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = (nibble(text[index * 2]) << 4) | nibble(text[index * 2 + 1]);
    return bytes;
}
}
AssetClipboardTransfer::AssetClipboardTransfer(AssetStore &store, AssetStore::Digests digests, Emit emit, Publish publish,
    std::function<bool()> admitted, std::function<std::int64_t()> generation,
    std::function<std::array<std::uint8_t, 16>()> random,
    std::function<PreparedClipboardPng(const std::vector<std::uint8_t> &)> png, bool images, bool files, AssetClipboardEndpointRole role)
    : m_store(store), m_digests(std::move(digests)), m_emit(std::move(emit)), m_publish(std::move(publish)),
      m_admitted(std::move(admitted)), m_generation(std::move(generation)), m_random(std::move(random)),
      m_png(std::move(png)), m_images(images), m_files(files), m_role(role) {}

void AssetClipboardTransfer::check(bool value, const char *reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
void AssetClipboardTransfer::access() { check(m_epoch && m_admitted(), "Clipboard asset authority expired."); }
AssetClipboardRecord AssetClipboardTransfer::response(const Transfer &transfer, AssetClipboardOperation operation) const
{
    AssetClipboardRecord result;
    result.operation = operation;
    result.epoch = transfer.offer.epoch;
    result.serial = transfer.offer.serial;
    result.transfer = transfer.offer.transfer;
    return result;
}
bool AssetClipboardTransfer::same(const Transfer &transfer, const AssetClipboardRecord &record) const
{
    return transfer.offer.epoch == record.epoch && transfer.offer.serial == record.serial
        && transfer.offer.transfer == record.transfer;
}
void AssetClipboardTransfer::dispose(Transfer &transfer)
{
    if (transfer.publication)
        transfer.publication->store(false);
    if (!transfer.snapshot.identifier.empty()) {
        m_store.cancel(transfer.snapshot.identifier);
        transfer.snapshot.identifier.clear();
    }
    transfer.sources.reset();
    transfer.png.clear();
    transfer.hash.reset();
}
void AssetClipboardTransfer::cancel(std::optional<Transfer> &transfer, const std::string &reason, bool notify)
{
    if (!transfer)
        return;
    dispose(*transfer);
    if (!notify) {
        transfer.reset();
        return;
    }
    if (transfer->cancelSent)
        return;
    transfer->cancelling = true;
    transfer->cancelSent = true;
    transfer->progress = std::chrono::steady_clock::now();
    auto record = response(*transfer, AssetClipboardOperation::Cancel);
    record.reason = reason;
    m_emit(std::move(record));
}
void AssetClipboardTransfer::stop()
{
    cancel(m_outgoing, "user_cancelled", false);
    cancel(m_incoming, "user_cancelled", false);
    m_pendingSelection.reset();
    m_pendingControl.reset();
    m_controlAcknowledgements.clear();
    m_epoch = 0;
}
bool AssetClipboardTransfer::drainCancellation(std::optional<Transfer> &slot, const AssetClipboardRecord &record)
{
    if (!slot || !same(*slot, record))
        return false;
    auto &transfer = *slot;
    if (record.operation == AssetClipboardOperation::Cancel) {
        dispose(transfer);
        auto acknowledged = response(transfer, AssetClipboardOperation::CancelAcknowledged);
        m_emit(std::move(acknowledged)); // Same FIFO as any already in-flight record.
        if (!transfer.cancelSent)
            slot.reset();
        return true;
    }
    if (record.operation == AssetClipboardOperation::CancelAcknowledged) {
        check(transfer.cancelSent, "Clipboard cancellation acknowledgment was unsolicited.");
        slot.reset();
        return true;
    }
    if (!transfer.cancelling)
        return false;
    if (record.operation == AssetClipboardOperation::Chunk) {
        const auto size = transfer.offer.items.at(transfer.item - 1).size;
        check(++transfer.drainedChunks <= 4 && record.item == transfer.item
            && record.offset == transfer.offset && transfer.offset < size
            && record.data.size() == std::min<std::uint64_t>(65536, size - transfer.offset),
            "Clipboard cancellation drain exceeded its known chunk window.");
        transfer.offset += record.data.size();
    } else {
        check(++transfer.drainedMetadata <= AssetClipboardWindowChunks + 4,
            "Clipboard cancellation drain exceeded its known metadata.");
        const bool outgoing = &slot == &m_outgoing;
        if (!outgoing) {
            const auto size = transfer.offer.items.at(transfer.item - 1).size;
            check(record.operation == AssetClipboardOperation::Finish && record.item == transfer.item
                && record.offset == size && transfer.offset == size,
                "Cancelled incoming assets changed their final item proof.");
        } else if (record.operation == AssetClipboardOperation::Accept) {
            check(!transfer.accepted && record.grantBytes == transfer.offer.totalBytes && record.window == 4,
                "Cancelled asset acceptance changed its original reservation.");
            transfer.accepted = true;
        } else if (record.operation == AssetClipboardOperation::Credit) {
            check(record.item == transfer.item, "Cancelled asset credit changed its item.");
            if (record.verified) {
                check(transfer.finishing && transfer.item < transfer.offer.items.size()
                    && record.offset == transfer.offset, "Cancelled verified credit was premature.");
            } else {
                const auto boundary = std::find(transfer.boundaries.begin(), transfer.boundaries.end(), record.offset);
                check(record.offset > transfer.credited && boundary != transfer.boundaries.end(),
                    "Cancelled asset credit did not acknowledge a sent chunk.");
                transfer.credited = record.offset;
                transfer.boundaries.erase(transfer.boundaries.begin(), std::next(boundary));
            }
        } else {
            check(record.operation == AssetClipboardOperation::Committed && transfer.finishing
                && transfer.item == transfer.offer.items.size(), "Cancelled asset commit was premature.");
        }
    }
    return true;
}
void AssetClipboardTransfer::advancePending(Time now)
{
    // Only Pause may cross its own outstanding Arm. A newer Arm must wait
    // for every earlier control ACK, matching the authenticated channel peer.
    const bool canPipelinePause = m_pendingControl
        && m_pendingControl->operation == AssetClipboardOperation::Pause
        && m_controlAcknowledgements.size() == 1
        && m_controlAcknowledgements.front().operation == AssetClipboardOperation::Arm
        && m_controlAcknowledgements.front().epoch == m_pendingControl->epoch;
    if (m_pendingControl && (m_controlAcknowledgements.empty() || canPipelinePause)) {
        auto control = *m_pendingControl;
        m_pendingControl.reset();
        m_controlAcknowledgements.push_back(control);
        m_emit(std::move(control));
    }
    if (!m_controlAcknowledgements.empty() || m_pendingControl)
        return;
    if ((m_outgoing && m_outgoing->cancelling) || (m_incoming && m_incoming->cancelling))
        return;
    if (m_pendingSelection && m_epoch && !m_disabled) {
        if (!m_admitted()) {
            m_pendingSelection.reset();
            return;
        }
        auto selection = std::move(*m_pendingSelection);
        m_pendingSelection.reset();
        localChanged(std::move(selection), now);
    }
}
void AssetClipboardTransfer::focusControl(AssetClipboardRecord control)
{
    // Focus is a stronger FIFO barrier than same-epoch clipboard cancellation.
    // The adapter drops queued old records before emitting the control or its ACK.
    cancel(m_outgoing, "stale_epoch", false);
    cancel(m_incoming, "stale_epoch", false);
    m_pendingSelection.reset();
    m_epoch = 0;
    m_pendingControl = std::move(control);
    advancePending(std::chrono::steady_clock::now());
}
void AssetClipboardTransfer::receive(const AssetClipboardRecord &record, Time now)
{
    encodeAssetClipboardRecord(record);
    if (drainCancellation(m_outgoing, record) || drainCancellation(m_incoming, record)) {
        advancePending(now);
        return;
    }
    if (m_disabled)
        return;
    if (record.operation == AssetClipboardOperation::EpochAcknowledged) {
        check(m_role == AssetClipboardEndpointRole::Viewer && !m_controlAcknowledgements.empty(),
            "Clipboard epoch acknowledgment was unsolicited.");
        const auto expected = m_controlAcknowledgements.front();
        check(expected.epoch == record.epoch && expected.operation == record.control,
            "Clipboard epoch acknowledgment changed its exact control.");
        m_controlAcknowledgements.pop_front();
        if (m_controlAcknowledgements.empty() && !m_pendingControl) {
            m_epoch = record.control == AssetClipboardOperation::Arm ? record.epoch : 0;
            m_originSerial = 0;
            m_receiverSerial = 0;
        }
        advancePending(now);
        return;
    }
    if (record.operation == AssetClipboardOperation::Arm || record.operation == AssetClipboardOperation::Pause) {
        check(m_role == AssetClipboardEndpointRole::Worker, "Only the viewer may change asset focus.");
        if (record.operation == AssetClipboardOperation::Arm) {
            check(record.epoch > m_lastEpoch, "Clipboard asset epoch was replayed.");
            m_lastEpoch = record.epoch;
        } else {
            check(record.epoch == m_lastEpoch, "Clipboard asset Pause changed its epoch.");
        }
        cancel(m_outgoing, "stale_epoch", false);
        cancel(m_incoming, "stale_epoch", false);
        m_pendingSelection.reset();
        m_epoch = record.operation == AssetClipboardOperation::Arm ? record.epoch : 0;
        m_originSerial = 0;
        m_receiverSerial = 0;
        AssetClipboardRecord acknowledged;
        acknowledged.operation = AssetClipboardOperation::EpochAcknowledged;
        acknowledged.epoch = record.epoch;
        acknowledged.control = record.operation;
        m_emit(std::move(acknowledged));
        return;
    }
    if (record.epoch < m_lastEpoch || (!m_epoch && record.epoch == m_lastEpoch))
        return; // Focus barrier drains old data without touching the OS.
    check(record.epoch == m_epoch, "Clipboard asset epoch changed.");
    access();
    if (record.operation == AssetClipboardOperation::Offer) {
        check(!m_incoming && (!m_outgoing || !same(*m_outgoing, record)) && record.serial == m_receiverSerial + 1,
            "Clipboard asset offer was replayed or already active.");
        m_receiverSerial = record.serial;
        Transfer transfer;
        transfer.offer = record;
        transfer.generation = m_generation();
        transfer.progress = now;
        transfer.deadline = now + std::chrono::minutes(10);
        m_incoming = std::move(transfer);
        if ((record.format == AssetClipboardFormat::Png && !m_images)
            || (record.format == AssetClipboardFormat::Files && !m_files)) {
            cancel(m_incoming, "unsupported");
            return;
        }
        try {
            std::vector<AssetEntry> entries;
            for (const auto &item : record.items)
                entries.push_back({item.name, item.size, {}});
            m_incoming->snapshot = m_store.prepare(entries);
            access();
            auto accepted = response(*m_incoming, AssetClipboardOperation::Accept);
            accepted.grantBytes = record.totalBytes;
            accepted.window = 4;
            m_incoming->accepted = true;
            m_emit(std::move(accepted));
        } catch (...) {
            cancel(m_incoming, "quota");
        }
        return;
    }
    if (m_incoming && same(*m_incoming, record)
        && (record.operation == AssetClipboardOperation::Chunk || record.operation == AssetClipboardOperation::Finish
            || record.operation == AssetClipboardOperation::Cancel)) {
        auto &transfer = *m_incoming;
        if (record.operation == AssetClipboardOperation::Cancel) {
            m_store.cancel(transfer.snapshot.identifier);
            m_incoming.reset();
            return;
        }
        check(transfer.accepted && !transfer.publishing && record.item == transfer.item,
            "Clipboard asset current item changed.");
        const auto size = transfer.offer.items.at(transfer.item - 1).size;
        if (record.operation == AssetClipboardOperation::Chunk) {
            check(record.offset == transfer.offset && transfer.offset < size
                && record.data.size() == std::min<std::uint64_t>(65536, size - transfer.offset),
                "Clipboard asset chunk changed its exact offset or length.");
            m_store.append(transfer.snapshot.identifier, transfer.item - 1, transfer.offset,
                record.data.data(), record.data.size());
            access();
            transfer.offset += record.data.size();
            transfer.progress = now;
            auto credit = response(transfer, AssetClipboardOperation::Credit);
            credit.item = transfer.item;
            credit.offset = transfer.offset;
            m_emit(std::move(credit));
        } else {
            check(record.offset == size && transfer.offset == size, "Clipboard asset Finish is premature.");
            try {
                m_store.finishEntry(transfer.snapshot.identifier, transfer.item - 1, hexDigest(record.sha256));
            } catch (...) {
                cancel(m_incoming, "hash_mismatch");
                return;
            }
            access();
            if (transfer.item < transfer.offer.items.size()) {
                auto credit = response(transfer, AssetClipboardOperation::Credit);
                credit.item = transfer.item;
                credit.offset = size;
                credit.verified = true;
                ++transfer.item;
                transfer.offset = 0;
                transfer.progress = now;
                m_emit(std::move(credit));
            } else {
                transfer.snapshot = m_store.finish(transfer.snapshot.identifier);
                check(m_generation() == transfer.generation, "A newer local clipboard generation superseded incoming assets.");
                // Durably reserve a possible OS clipboard owner BEFORE attempting publication.
                // A failed/crashed publication is uncertainty, never permission to unlink URLs.
                m_store.published(transfer.snapshot.identifier);
                transfer.publishing = true;
                transfer.publication = std::make_shared<std::atomic_bool>(true);
                try {
                    m_publish(transfer.offer, transfer.snapshot, transfer.generation, transfer.publication);
                } catch (...) {
                    cancel(m_incoming, "unsupported");
                }
            }
        }
        return;
    }
    check(m_outgoing && same(*m_outgoing, record), "Clipboard asset response changed its transfer.");
    auto &transfer = *m_outgoing;
    if (record.operation == AssetClipboardOperation::Cancel) {
        m_outgoing.reset();
        return;
    }
    if (record.operation == AssetClipboardOperation::Accept) {
        check(!transfer.accepted && record.grantBytes == transfer.offer.totalBytes && record.window == 4,
            "Clipboard asset acceptance changed its reservation.");
        transfer.accepted = true;
    } else if (record.operation == AssetClipboardOperation::Credit) {
        check(transfer.accepted && record.item == transfer.item, "Clipboard asset credit changed its item.");
        if (record.verified) {
            check(transfer.finishing && transfer.item < transfer.offer.items.size()
                && record.offset == transfer.offset, "Clipboard asset verified credit is premature.");
            ++transfer.item;
            transfer.offset = 0;
            transfer.credited = 0;
            transfer.finishing = false;
            transfer.hash = m_digests();
        } else {
            const auto boundary = std::find(transfer.boundaries.begin(), transfer.boundaries.end(), record.offset);
            check(record.offset > transfer.credited && boundary != transfer.boundaries.end(),
                "Clipboard asset credit does not acknowledge sent chunks.");
            transfer.credited = record.offset;
            transfer.boundaries.erase(transfer.boundaries.begin(), std::next(boundary));
        }
    } else if (record.operation == AssetClipboardOperation::Committed) {
        check(transfer.finishing && transfer.item == transfer.offer.items.size(), "Clipboard asset commit is premature.");
        m_outgoing.reset();
        return;
    } else {
        throw std::runtime_error("Invalid clipboard asset direction.");
    }
    transfer.progress = now;
    try {
        sendNext(now);
    } catch (...) {
        cancel(m_outgoing, "source_changed");
    }
}

void AssetClipboardTransfer::localChanged(LocalAssetSelection selection, Time now)
{
    if (m_epoch && m_admitted() && selection.generation == m_generation() && selection.ownershipKnown)
        m_store.observeReferencedPaths(selection.paths,
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    if (!m_controlAcknowledgements.empty() || m_pendingControl) {
        m_pendingSelection = std::move(selection);
        return;
    }
    access();
    cancel(m_outgoing, "source_changed");
    cancel(m_incoming, "source_changed");
    if ((m_outgoing && m_outgoing->cancelling) || (m_incoming && m_incoming->cancelling)) {
        m_pendingSelection = std::move(selection);
        return;
    }
    if (selection.generation < 0 || (selection.paths.empty() && selection.image.empty()))
        return;
    Transfer transfer;
    transfer.offer.operation = AssetClipboardOperation::Offer;
    transfer.offer.epoch = m_epoch;
    check(m_originSerial < std::numeric_limits<std::uint64_t>::max(), "Clipboard asset serial exhausted.");
    transfer.offer.serial = m_originSerial + 1;
    transfer.offer.transfer = m_random();
    transfer.offer.format = selection.format;
    transfer.generation = selection.generation;
    transfer.progress = now;
    transfer.deadline = now + std::chrono::minutes(10);
    if (selection.format == AssetClipboardFormat::Files) {
        check(m_files, "Clipboard file sharing was not negotiated.");
        transfer.sources = std::make_unique<AssetSourceBatch>(selection.paths);
        for (const auto &entry : transfer.sources->entries()) {
            transfer.offer.items.push_back({static_cast<std::uint32_t>(transfer.offer.items.size() + 1), entry.size, entry.name});
            transfer.offer.totalBytes += entry.size;
        }
    } else {
        check(m_images, "Clipboard image sharing was not negotiated.");
        auto image = m_png(selection.image);
        transfer.png = std::move(image.bytes);
        transfer.offer.width = image.width;
        transfer.offer.height = image.height;
        transfer.offer.totalBytes = transfer.png.size();
        transfer.offer.items.push_back({1, transfer.offer.totalBytes, "clipboard.png"});
    }
    transfer.hash = m_digests();
    encodeAssetClipboardRecord(transfer.offer);
    access();
    if (m_generation() != selection.generation)
        return;
    m_outgoing = std::move(transfer);
    ++m_originSerial;
    m_emit(m_outgoing->offer);
}

void AssetClipboardTransfer::sendNext(Time now)
{
    if (!m_outgoing || !m_outgoing->accepted || m_outgoing->finishing || m_outgoing->cancelling)
        return;
    auto &transfer = *m_outgoing;
    const auto size = transfer.offer.items.at(transfer.item - 1).size;
    while (transfer.offset < size && transfer.boundaries.size() < 4) {
        access();
        if (m_generation() != transfer.generation) {
            cancel(m_outgoing, "source_changed");
            return;
        }
        auto chunk = response(transfer, AssetClipboardOperation::Chunk);
        chunk.item = transfer.item;
        chunk.offset = transfer.offset;
        if (transfer.sources) {
            chunk.data = transfer.sources->read(transfer.item - 1, transfer.offset, 65536);
        } else {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(65536, size - transfer.offset));
            chunk.data.assign(transfer.png.begin() + transfer.offset, transfer.png.begin() + transfer.offset + count);
        }
        access();
        if (m_generation() != transfer.generation) {
            cancel(m_outgoing, "source_changed");
            return;
        }
        transfer.hash->update(chunk.data.data(), chunk.data.size());
        if (m_generation() != transfer.generation) {
            cancel(m_outgoing, "source_changed");
            return;
        }
        transfer.offset += chunk.data.size();
        transfer.boundaries.push_back(transfer.offset);
        m_emit(std::move(chunk));
    }
    if (transfer.offset == size && transfer.boundaries.empty()) {
        if (transfer.sources)
            transfer.sources->verify(transfer.item - 1);
        access();
        auto finish = response(transfer, AssetClipboardOperation::Finish);
        finish.item = transfer.item;
        finish.offset = size;
        finish.sha256 = binaryDigest(transfer.hash->finish());
        access();
        if (m_generation() != transfer.generation) {
            cancel(m_outgoing, "source_changed");
            return;
        }
        transfer.finishing = true;
        transfer.progress = now;
        m_emit(std::move(finish));
    }
}

void AssetClipboardTransfer::publicationCompleted(const AssetClipboardRecord &offer, bool success)
{
    if (!m_incoming || !m_incoming->publishing || m_incoming->cancelling || !same(*m_incoming, offer))
        return;
    if (!success || !m_admitted() || !m_incoming->publication->load()) {
        cancel(m_incoming, "unavailable");
        return;
    }
    m_store.published(m_incoming->snapshot.identifier);
    m_store.observeOwned(m_incoming->snapshot.identifier,
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    auto committed = response(*m_incoming, AssetClipboardOperation::Committed);
    m_incoming.reset();
    m_emit(std::move(committed));
}
void AssetClipboardTransfer::poll(Time now)
{
    if (m_disabled)
        return;
    for (auto *slot : {&m_incoming, &m_outgoing}) {
        if (*slot && (*slot)->cancelling && now - (*slot)->progress >= std::chrono::seconds(10)) {
            stop();
            m_disabled = true;
            return;
        }
    }
    if (!m_epoch)
        return;
    if (!m_admitted()) {
        cancel(m_outgoing, "user_cancelled");
        cancel(m_incoming, "user_cancelled");
        return;
    }
    for (auto *transfer : {&m_incoming, &m_outgoing}) {
        if (*transfer && !(*transfer)->cancelling
            && (now >= (*transfer)->deadline || now - (*transfer)->progress >= std::chrono::seconds(10)))
            cancel(*transfer, "timeout");
    }
    try {
        sendNext(now);
    } catch (...) {
        cancel(m_outgoing, "source_changed");
    }
}
}

namespace dockpipe::desktop {
void AssetClipboardTransfer::arm(std::uint64_t epoch)
{
    check(m_role == AssetClipboardEndpointRole::Viewer && !m_disabled
        && epoch > m_lastEpoch, "Viewer asset epoch was replayed.");
    m_lastEpoch = epoch;
    AssetClipboardRecord record;
    record.operation = AssetClipboardOperation::Arm;
    record.epoch = epoch;
    focusControl(std::move(record));
}
void AssetClipboardTransfer::pause()
{
    if (!m_lastEpoch)
        return;
    check(m_role == AssetClipboardEndpointRole::Viewer, "Only the viewer may pause asset sharing.");
    // A coalesced Arm which never reached the FIFO must not produce an orphan Pause.
    m_pendingControl.reset();
    std::uint64_t epoch = m_epoch;
    if (!m_controlAcknowledgements.empty()) {
        const auto &lastSent = m_controlAcknowledgements.back();
        if (lastSent.operation == AssetClipboardOperation::Pause)
            return;
        epoch = lastSent.epoch;
    }
    if (!epoch)
        return;
    AssetClipboardRecord record;
    record.operation = AssetClipboardOperation::Pause;
    record.epoch = epoch;
    focusControl(std::move(record));
}
}

namespace dockpipe::desktop {
std::optional<std::int64_t> AssetClipboardTransfer::outgoingGeneration(const AssetClipboardRecord &record) const
{
    if (!m_outgoing || m_outgoing->cancelling || !same(*m_outgoing, record))
        return std::nullopt;
    return m_outgoing->generation;
}
}

namespace dockpipe::desktop {
void AssetClipboardTransfer::maintainOwnership(const std::vector<std::string> &paths,
    std::int64_t generation, std::int64_t now)
{
    if (!active() || !m_admitted() || generation != m_generation())
        return;
    m_store.observeReferencedPaths(paths, now);
    m_store.reap(now, true);
}
}
