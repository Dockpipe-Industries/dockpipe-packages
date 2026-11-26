#include "NativeAssets.h"
#include "NativeAssetClipboard.h"
#include <condition_variable>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>

namespace dockpipe::desktop {
struct NativeAssets::State {
    struct Publication {
        AssetClipboardRecord offer;
        AssetSnapshot snapshot;
        std::vector<std::uint8_t> png;
        std::int64_t generation;
        std::shared_ptr<std::atomic_bool> ticket;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void(AssetClipboardTransfer &)>> jobs;
    std::optional<Publication> publication;
    std::atomic_bool allowed{false};
    std::atomic<std::int64_t> generation{-1};
    std::atomic<std::uint64_t> epoch{0};
    std::uint64_t laneEpoch = 0; // I/O lane only; a newer main-thread Arm cannot revive old work.
    std::deque<AssetClipboardRecord> cancelledPublications;
    std::atomic<std::int64_t> allowedUntil{0};
    std::optional<LocalAssetSelection> pendingSelection;
    bool selectionQueued = false;
    bool maintenanceQueued = false;
    bool stopping = false;
    bool done = false;
    bool unavailable = false;
    std::thread thread;

    bool admitted() const
    {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return allowed.load() && now < allowedUntil.load();
    }
    bool operationAdmitted() const
    {
        return admitted() && laneEpoch != 0 && laneEpoch == epoch.load();
    }
    bool publicationCancelled(const AssetClipboardRecord &offer) const
    {
        return std::any_of(cancelledPublications.begin(), cancelledPublications.end(), [&](const auto &cancelled) {
            return cancelled.epoch == offer.epoch && cancelled.serial == offer.serial && cancelled.transfer == offer.transfer;
        });
    }
    void refresh(bool value)
    {
        allowed.store(value);
        const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        allowedUntil.store(std::chrono::duration_cast<std::chrono::nanoseconds>(limit.time_since_epoch()).count());
    }
    bool select(LocalAssetSelection selection)
    {
        std::lock_guard lock(mutex);
        if (stopping || unavailable)
            return false;
        pendingSelection = std::move(selection);
        if (selectionQueued)
            return true;
        if (jobs.size() >= 12) {
            allowed.store(false);
            pendingSelection.reset();
            if (publication)
                publication->ticket->store(false);
            return false;
        }
        selectionQueued = true;
        jobs.push_back([this](AssetClipboardTransfer &transfer) {
            std::optional<LocalAssetSelection> selection;
            {
                std::lock_guard guard(mutex);
                selection = std::move(pendingSelection);
                pendingSelection.reset();
                selectionQueued = false;
            }
            if (selection) {
                try {
                    transfer.localChanged(std::move(*selection), std::chrono::steady_clock::now());
                } catch (...) {
                    std::fputs("{\"clipboard_assets\":\"unsupported\"}\n", stderr);
                }
            }
        });
        changed.notify_one();
        return true;
    }

    void fencePublication(const AssetClipboardRecord *record = nullptr)
    {
        std::lock_guard lock(mutex);
        if (publication && (!record || (publication->offer.epoch == record->epoch
            && publication->offer.serial == record->serial && publication->offer.transfer == record->transfer)))
            publication->ticket->store(false);
    }
    bool post(std::function<void(AssetClipboardTransfer &)> job)
    {
        std::lock_guard lock(mutex);
        if (stopping || unavailable || jobs.size() >= 12) {
            allowed.store(false);
            if (publication)
                publication->ticket->store(false);
            return false;
        }
        jobs.push_back(std::move(job));
        changed.notify_one();
        return true;
    }
};

NativeAssets::NativeAssets(std::string root, bool images, bool files,
    std::function<void(AssetClipboardRecord)> emit, std::function<bool()> admitted,
    std::function<void()> clipboardPublished)
    : m_state(std::make_shared<State>()), m_admitted(std::move(admitted)),
      m_clipboardPublished(std::move(clipboardPublished)), m_images(images), m_files(files)
{
    const auto state = m_state;
    state->thread = std::thread([state, root = std::move(root), images, files, emit = std::move(emit)] {
        try {
            const auto digests = [] { return makeNativeAssetDigest(); };
            AssetStore store(root, digests, validateNativeAssetNames);
            AssetClipboardTransfer transfer(store, digests, emit,
                [state, &store](const auto &offer, const auto &snapshot, auto generation, auto ticket) {
                    State::Publication publication{offer, snapshot, {}, generation, ticket};
                    if (offer.format == AssetClipboardFormat::Png) {
                        std::vector<std::uint8_t> bytes;
                        bytes.reserve(static_cast<std::size_t>(offer.totalBytes));
                        for (std::uint64_t offset = 0; offset < offer.totalBytes;) {
                            if (!state->operationAdmitted() || !ticket->load())
                                return;
                            auto part = store.read(snapshot, 0, offset, 65536);
                            offset += part.size();
                            bytes.insert(bytes.end(), part.begin(), part.end());
                        }
                        const std::vector<std::uint8_t> signature{137, 80, 78, 71, 13, 10, 26, 10};
                        if (bytes.size() < signature.size() || !std::equal(signature.begin(), signature.end(), bytes.begin()))
                            throw std::runtime_error("Clipboard PNG signature changed.");
                        auto image = normalizeNativeClipboardPng(bytes);
                        if (image.width != offer.width || image.height != offer.height)
                            throw std::runtime_error("Clipboard image dimensions changed.");
                        publication.png = std::move(image.bytes);
                    }
                    std::lock_guard lock(state->mutex);
                    if (state->stopping || !state->operationAdmitted() || !ticket->load() || state->publicationCancelled(offer))
                        return;
                    if (state->publication)
                        throw std::runtime_error("Clipboard publication is already pending.");
                    state->publication = std::move(publication);
                }, [state] { return state->operationAdmitted(); },
                [state] { return state->generation.load(); }, nativeAssetTransferIdentifier,
                normalizeNativeClipboardPng, images, files);
            for (;;) {
                std::function<void(AssetClipboardTransfer &)> job;
                {
                    std::unique_lock lock(state->mutex);
                    state->changed.wait_for(lock, std::chrono::milliseconds(25), [&] {
                        return state->stopping || !state->jobs.empty();
                    });
                    if (state->stopping)
                        break;
                    if (!state->jobs.empty()) {
                        job = std::move(state->jobs.front());
                        state->jobs.pop_front();
                    }
                }
                if (job) {
                    try {
                        job(transfer);
                    } catch (...) {
                        if (state->laneEpoch != state->epoch.load())
                            continue; // Obsolete I/O lost its fence; queued controls still need their ACKs.
                        throw;
                    }
                }
                if (state->laneEpoch != state->epoch.load())
                    continue;
                transfer.poll(std::chrono::steady_clock::now());
                if (transfer.disabled()) {
                    std::lock_guard lock(state->mutex);
                    state->unavailable = true;
                    state->allowed.store(false);
                }
                store.reap(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
            }
            transfer.stop();
        } catch (...) {
            // Fixed diagnostic only: never include source names, paths, bytes or content hashes.
            std::fputs("{\"clipboard_assets\":\"unavailable\"}\n", stderr);
            std::lock_guard lock(state->mutex);
            state->unavailable = true;
        }
        std::lock_guard lock(state->mutex);
        state->done = true;
        state->changed.notify_all();
    });
}
NativeAssets::~NativeAssets() { stop(); }

void NativeAssets::receive(AssetClipboardRecord record)
{
    encodeAssetClipboardRecord(record);
    const auto state = m_state;
    {
        std::lock_guard lock(state->mutex);
        if (state->unavailable)
            return; // Rich sharing stays disabled until a new channel; media/input remain independent.
    }
    if (record.operation == AssetClipboardOperation::Arm) {
        if (!m_admitted() || record.epoch <= m_lastEpoch)
            throw std::runtime_error("Native clipboard asset authority expired or epoch was replayed.");
        m_lastEpoch = record.epoch;
        state->fencePublication();
        {
            std::lock_guard lock(state->mutex);
            state->cancelledPublications.clear();
        }
        state->refresh(true);
        state->epoch.store(record.epoch);
        m_baseline = nativeAssetClipboardGeneration(); // Baseline only; no initial content export.
        m_ownershipKnown = false;
        m_referencedPaths.clear();
        state->generation.store(m_baseline);
    } else if (record.operation == AssetClipboardOperation::Pause) {
        if (record.epoch != m_lastEpoch)
            throw std::runtime_error("Native clipboard asset Pause changed its epoch.");
        state->allowed.store(false);
        state->epoch.store(0);
        state->fencePublication();
    } else if (record.operation == AssetClipboardOperation::Cancel) {
        state->fencePublication(&record);
        if (record.epoch == state->epoch.load()) {
            std::lock_guard lock(state->mutex);
            if (!state->publicationCancelled(record)) {
                if (state->cancelledPublications.size() == 2)
                    state->cancelledPublications.pop_front();
                state->cancelledPublications.push_back(record);
            }
        }
    }
    if (!state->post([state, record = std::move(record)](auto &transfer) {
        if (record.operation == AssetClipboardOperation::Arm)
            state->laneEpoch = record.epoch;
        else if (record.operation == AssetClipboardOperation::Pause)
            state->laneEpoch = 0;
        transfer.receive(record, std::chrono::steady_clock::now());
    }))
        throw std::runtime_error("Native clipboard asset job capacity is unavailable.");
}
void NativeAssets::poll()
{
    const auto state = m_state;
    {
        std::lock_guard lock(state->mutex);
        if (state->unavailable) {
            state->allowed.store(false);
            state->epoch.store(0);
            return;
        }
    }
    const bool allowed = state->epoch.load() && m_admitted();
    state->refresh(allowed);
    if (!allowed) {
        state->fencePublication();
        return;
    }
    std::optional<State::Publication> publication;
    {
        std::lock_guard lock(state->mutex);
        if (state->publication) {
            publication = std::move(state->publication);
            state->publication.reset();
        }
    }
    if (publication) {
        const auto &item = *publication;
        const auto admitted = [&] {
            return item.ticket->load() && state->admitted() && state->epoch.load() == item.offer.epoch
                && m_admitted();
        };
        const bool success = publishNativeAssetClipboard(item.offer, item.snapshot, item.png,
            item.generation, admitted);
        const auto observed = nativeAssetClipboardGeneration();
        if (success || observed != item.generation) {
            // An OS publication failure may have cleared or partially changed the pasteboard.
            // Retire its echo without claiming the prior clipboard survived or sending Committed.
            m_baseline = observed;
            m_ownershipKnown = success;
            m_referencedPaths = success ? item.snapshot.paths : std::vector<std::string>{};
            state->generation.store(m_baseline);
            m_clipboardPublished();
        }
        if (!state->post([offer = item.offer, success](auto &transfer) {
            transfer.publicationCompleted(offer, success);
        }))
            throw std::runtime_error("Native clipboard publication completion could not be queued.");
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextPoll)
        return;
    m_nextPoll = now + std::chrono::milliseconds(100);
    const auto count = nativeAssetClipboardGeneration();
    state->generation.store(count);
    if (count == m_baseline) {
        if (m_ownershipKnown && now >= m_nextMaintenance) {
            m_nextMaintenance = now + std::chrono::seconds(1);
            bool queued = false;
            {
                std::lock_guard lock(state->mutex);
                queued = state->maintenanceQueued;
                state->maintenanceQueued = true;
            }
            if (!queued && !state->post([state, paths = m_referencedPaths, count](auto &transfer) {
                {
                    std::lock_guard lock(state->mutex);
                    state->maintenanceQueued = false;
                }
                transfer.maintainOwnership(paths, count,
                    std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
            }))
                throw std::runtime_error("Native clipboard ownership maintenance could not be queued.");
        }
        return;
    }
    m_baseline = count;
    state->fencePublication();
    LocalAssetSelection selection;
    try {
        selection = readNativeAssetClipboard(m_images, m_files);
    } catch (...) {
        selection.generation = count;
        selection.ownershipKnown = false;
        std::fputs("{\"clipboard_assets\":\"unsupported\"}\n", stderr);
    }
    m_ownershipKnown = selection.ownershipKnown;
    m_referencedPaths = selection.paths;
    if (!state->select(std::move(selection)))
        throw std::runtime_error("Native clipboard observation capacity is unavailable.");
}
void NativeAssets::externalClipboardWrite()
{
    if (!m_state->epoch.load())
        return;
    m_baseline = nativeAssetClipboardGeneration();
    m_state->generation.store(m_baseline);
    m_ownershipKnown = true;
    m_referencedPaths.clear();
    m_state->fencePublication();
    LocalAssetSelection retired;
    retired.generation = m_baseline;
    if (!m_state->select(std::move(retired)))
        throw std::runtime_error("Native clipboard asset retirement could not be queued.");
}

bool NativeAssets::stop()
{
    const auto state = m_state;
    state->allowed.store(false);
    state->epoch.store(0);
    std::unique_lock lock(state->mutex);
    if (state->publication)
        state->publication->ticket->store(false);
    state->stopping = true;
    state->jobs.clear();
    state->pendingSelection.reset();
    state->changed.notify_all();
    const bool stopped = state->changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return state->done; });
    lock.unlock();
    if (state->thread.joinable()) {
        if (stopped)
            state->thread.join();
        else
            state->thread.detach(); // Caller must retain the module and report unknown cleanup.
    }
    if (!stopped)
        m_stopUnknown = true;
    return stopped && !m_stopUnknown;
}
}
