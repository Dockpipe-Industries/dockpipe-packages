#include "AssetClipboardTransfer.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unistd.h>
using namespace dockpipe::desktop;
namespace {
std::function<void()> afterDigestUpdate;
std::function<void()> beforeDigestFinish;
class Digest final : public AssetDigest {
public:
    void update(const std::uint8_t *bytes, std::size_t size) override
    {
        for (std::size_t index = 0; index < size; ++index)
            value = value * 131 + bytes[index];
        if (afterDigestUpdate)
            afterDigestUpdate();
    }
    std::string finish() override
    {
        if (beforeDigestFinish)
            beforeDigestFinish();
        std::ostringstream result;
        result << std::hex << std::setw(64) << std::setfill('0') << value;
        return result.str();
    }
    std::uint64_t value = 0;
};
template<class Action> void refuses(Action action)
{
    bool rejected = false;
    try {
        action();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
struct Publication {
    AssetClipboardRecord offer;
    AssetSnapshot snapshot;
    std::shared_ptr<std::atomic_bool> ticket;
};
}
int main()
{
    char pattern[] = "/tmp/dockpipe-asset-transfer-XXXXXX";
    const auto temporary = mkdtemp(pattern);
    assert(temporary);
    const auto root = std::filesystem::canonical(temporary);
    const auto hashes = [] { return std::make_unique<Digest>(); };
    AssetStore workerStore((root / "worker").string(), hashes);
    AssetStore viewerStore((root / "viewer").string(), hashes);
    std::deque<AssetClipboardRecord> fromWorker;
    std::deque<AssetClipboardRecord> fromViewer;
    std::optional<Publication> workerPublish;
    std::optional<Publication> viewerPublish;
    std::int64_t generation = 1;
    bool admitted = true;
    const auto random = [] {
        static std::uint8_t counter = 0;
        std::array<std::uint8_t, 16> value{};
        value[0] = ++counter;
        return value;
    };
    const auto png = [](const std::vector<std::uint8_t> &bytes) {
        return PreparedClipboardPng{bytes, 1, 1};
    };
    AssetClipboardTransfer worker(workerStore, hashes,
        [&](auto record) { fromWorker.push_back(std::move(record)); },
        [&](const auto &offer, const auto &snapshot, auto count, auto ticket) {
            assert(count == generation);
            workerPublish = Publication{offer, snapshot, ticket};
        }, [&] { return admitted; }, [&] { return generation; }, random, png, true, true);
    AssetClipboardTransfer viewer(viewerStore, hashes,
        [&](auto record) { fromViewer.push_back(std::move(record)); },
        [&](const auto &offer, const auto &snapshot, auto count, auto ticket) {
            assert(count == generation);
            viewerPublish = Publication{offer, snapshot, ticket};
        }, [&] { return admitted; }, [&] { return generation; }, random, png, true, true,
        AssetClipboardEndpointRole::Viewer);
    const auto now = std::chrono::steady_clock::now();
    const auto pump = [&] {
        unsigned turns = 0;
        while (!fromViewer.empty() || !fromWorker.empty()) {
            assert(++turns < 1000);
            if (!fromViewer.empty()) {
                auto record = std::move(fromViewer.front());
                fromViewer.pop_front();
                worker.receive(record, now);
            }
            if (!fromWorker.empty()) {
                auto record = std::move(fromWorker.front());
                fromWorker.pop_front();
                viewer.receive(record, now);
            }
        }
    };
    viewer.arm(1);
    pump();
    const auto source = root / "selected.txt";
    const auto empty = root / "empty.txt";
    std::ofstream(source) << std::string(5 * 65536 + 3, 'x');
    std::ofstream(empty).close();
    LocalAssetSelection selection;
    selection.generation = generation;
    selection.paths = {source.string(), empty.string()};
    viewer.localChanged(selection, now);
    assert(fromViewer.size() == 1 && fromViewer.front().operation == AssetClipboardOperation::Offer);
    worker.receive(fromViewer.front(), now);
    fromViewer.pop_front();
    viewer.receive(fromWorker.front(), now);
    fromWorker.pop_front();
    assert(fromViewer.size() == 4); // No fifth uncredited chunk is produced.
    pump();
    assert(workerPublish && workerPublish->ticket->load());
    assert(workerPublish->snapshot.paths[0].ends_with("selected.txt"));
    assert(std::filesystem::file_size(workerPublish->snapshot.paths[0]) == 5 * 65536 + 3);
    worker.publicationCompleted(workerPublish->offer, true);
    pump();
    assert(std::filesystem::exists(workerPublish->snapshot.paths[0]));
    viewer.pause();
    pump();
    auto replay = AssetClipboardRecord{};
    replay.operation = AssetClipboardOperation::Arm;
    replay.epoch = 1;
    refuses([&] { worker.receive(replay, now); });
    viewer.arm(2);
    pump();
    auto stale = workerPublish->offer;
    stale.operation = AssetClipboardOperation::Committed;
    worker.receive(stale, now); // Late old lifetime cannot end or reopen epoch2.
    viewer.localChanged(selection, now);
    pump();
    assert(workerPublish->offer.epoch == 2);
    worker.publicationCompleted(workerPublish->offer, false);
    assert(fromWorker.size() == 1 && fromWorker.front().operation == AssetClipboardOperation::Cancel);
    pump();
    viewer.localChanged(selection, now);
    const auto pendingOffer = fromViewer.front();
    std::ofstream(source) << "changed";
    worker.receive(fromViewer.front(), now);
    fromViewer.pop_front();
    viewer.receive(fromWorker.front(), now);
    fromWorker.pop_front();
    assert(fromViewer.size() == 1 && fromViewer.front().operation == AssetClipboardOperation::Cancel);
    pump();
    // Simultaneous opposite transfers and crossed cancellations share no slot.
    std::ofstream(source) << "restored";
    ++generation;
    selection.generation = generation;
    worker.localChanged(selection, now);
    viewer.localChanged(selection, now);
    ++generation;
    selection.generation = generation;
    worker.localChanged(selection, now); // Cancel crosses the peer's already queued Offer/Accept.
    viewer.localChanged(selection, now);
    pump();
    assert(workerPublish && viewerPublish && workerPublish->ticket->load() && viewerPublish->ticket->load());
    const auto workerPending = workerPublish->offer;
    const auto viewerPending = viewerPublish->offer;
    ++generation;
    LocalAssetSelection unsupported;
    unsupported.generation = generation;
    worker.localChanged(unsupported, now);
    viewer.localChanged(unsupported, now);
    assert(!workerPublish->ticket->load() && !viewerPublish->ticket->load());
    pump(); // Both peers acknowledge both crossed Cancel requests before reuse.
    worker.publicationCompleted(workerPending, true);
    viewer.publicationCompleted(viewerPending, true);
    assert(fromWorker.empty() && fromViewer.empty()); // Late completed I/O cannot publish/commit.
    // Focus may Pause before the corresponding Arm acknowledgment arrives.
    viewer.arm(3);
    viewer.pause();
    viewer.arm(4); // Coalesced behind two outstanding epoch controls.
    viewer.pause(); // No orphan Pause4 may be emitted when Arm4 never left.
    assert(fromViewer.size() == 2);
    pump();
    assert(!viewer.active());
    viewer.arm(5);
    pump();
    assert(viewer.active() && viewer.epoch() == 5);
    // A newer copy during post-read hashing must fence Chunk and Finish too.
    selection.generation = generation;
    selection.paths = {source.string()};
    viewer.localChanged(selection, now);
    worker.receive(fromViewer.front(), now);
    fromViewer.pop_front();
    bool changeOnce = true;
    afterDigestUpdate = [&] {
        if (changeOnce) {
            changeOnce = false;
            ++generation;
        }
    };
    viewer.receive(fromWorker.front(), now);
    fromWorker.pop_front();
    afterDigestUpdate = {};
    assert(fromViewer.size() == 1 && fromViewer.front().operation == AssetClipboardOperation::Cancel);
    pump();
    selection.generation = generation;
    selection.paths = {empty.string()};
    viewer.localChanged(selection, now);
    worker.receive(fromViewer.front(), now);
    fromViewer.pop_front();
    changeOnce = true;
    beforeDigestFinish = [&] {
        if (changeOnce) {
            changeOnce = false;
            ++generation;
        }
    };
    viewer.receive(fromWorker.front(), now);
    fromWorker.pop_front();
    beforeDigestFinish = {};
    assert(fromViewer.size() == 1 && fromViewer.front().operation == AssetClipboardOperation::Cancel);
    pump();
    // A settled Arm followed by rapid Pause/rearm must not send a new Arm
    // before the peer acknowledges Pause. The Go peer rejects that ordering.
    assert(viewer.active() && viewer.epoch() == 5);
    viewer.pause();
    viewer.arm(6);
    assert(fromViewer.size() == 1);
    assert(fromViewer.front().operation == AssetClipboardOperation::Pause);
    assert(fromViewer.front().epoch == 5);
    worker.receive(fromViewer.front(), now);
    fromViewer.pop_front();
    assert(fromViewer.empty() && fromWorker.size() == 1);
    assert(fromWorker.front().operation == AssetClipboardOperation::EpochAcknowledged);
    assert(fromWorker.front().control == AssetClipboardOperation::Pause);
    assert(!viewer.active());
    viewer.receive(fromWorker.front(), now);
    fromWorker.pop_front();
    assert(fromViewer.size() == 1);
    assert(fromViewer.front().operation == AssetClipboardOperation::Arm);
    assert(fromViewer.front().epoch == 6);
    pump();
    assert(viewer.active() && viewer.epoch() == 6);
    admitted = false;
    worker.poll(now);
    viewer.poll(now);
    std::filesystem::remove_all(root);
    std::cout << "PASS sequential/empty files, four-window, manual publication ACK, failed publication, source mutation, crossed cancellation/full duplex, publication tickets, Arm/Pause ACK race, pause highwater, stale epoch and post-I/O generation fencing\n";
}
