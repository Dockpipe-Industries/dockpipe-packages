#include "NativeAssets.h"
#include "NativeAssetClipboard.h"
#include <cassert>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
using namespace dockpipe::desktop;
namespace {
std::atomic<std::int64_t> generation{1};
std::atomic_uint reads{0};
std::atomic_bool entered{false};
std::atomic_bool released{false};
std::atomic_bool completed{false};
std::mutex gateMutex;
std::condition_variable gateChanged;
class FakeDigest final : public AssetDigest {
public:
    void update(const std::uint8_t *, std::size_t) override {}
    std::string finish() override { return std::string(64, '0'); }
};
}
namespace dockpipe::desktop {
void validateNativeAssetNames(const std::vector<AssetEntry> &) {}
std::unique_ptr<AssetDigest> makeNativeAssetDigest() { return std::make_unique<FakeDigest>(); }
std::array<std::uint8_t, 16> nativeAssetTransferIdentifier()
{
    std::array<std::uint8_t, 16> value{};
    value[0] = 1;
    return value;
}
PreparedClipboardPng normalizeNativeClipboardPng(const std::vector<std::uint8_t> &bytes)
{
    entered = true;
    gateChanged.notify_all();
    std::unique_lock lock(gateMutex);
    gateChanged.wait(lock, [] { return released.load(); });
    completed = true;
    return {bytes, 1, 1};
}
std::int64_t nativeAssetClipboardGeneration() { return generation; }
LocalAssetSelection readNativeAssetClipboard(bool, bool)
{
    ++reads;
    LocalAssetSelection value;
    value.generation = generation;
    value.format = AssetClipboardFormat::Png;
    value.image = {1};
    return value;
}
bool publishNativeAssetClipboard(const AssetClipboardRecord &, const AssetSnapshot &,
    const std::vector<std::uint8_t> &, std::int64_t, const std::function<bool()> &)
{
    assert(false && "No publication is allowed in this cancellation fixture");
    return false;
}
}
int main()
{
    char pattern[] = "/tmp/dockpipe-native-assets-test-XXXXXX";
    const auto temporary = mkdtemp(pattern);
    assert(temporary);
    const std::filesystem::path root(temporary);
    std::atomic_uint emitted{0};
    NativeAssets assets((root / "cache").string(), true, true,
        [&](auto) { ++emitted; }, [] { return true; }, [] { assert(false); });
    assets.poll();
    assert(reads == 0); // No OS content read before Arm.
    AssetClipboardRecord arm;
    arm.operation = AssetClipboardOperation::Arm;
    arm.epoch = 1;
    assets.receive(arm);
    for (unsigned turn = 0; turn < 100 && emitted == 0; ++turn)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(emitted == 1 && reads == 0); // Arm ACK, baseline only.
    ++generation;
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    assets.poll();
    {
        std::unique_lock lock(gateMutex);
        assert(gateChanged.wait_for(lock, std::chrono::seconds(1), [] { return entered.load(); }));
    }
    for (unsigned copy = 0; copy < 20; ++copy) {
        ++generation;
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        assets.poll(); // One replaceable latest image, not twenty queued image payloads.
    }
    const auto start = std::chrono::steady_clock::now();
    assert(!assets.stop()); // A blocked job must never become verified cleanup.
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    assert(emitted == 1); // No Offer or publication while the blocked job is fenced.
    released = true;
    gateChanged.notify_all();
    for (unsigned turn = 0; turn < 200 && !completed; ++turn)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(completed);
    assert(!assets.stop()); // An earlier unknown stop cannot be upgraded later.
    generation = 100;
    reads = 0;
    entered = false;
    released = false;
    completed = false;
    std::atomic_uint acknowledgments{0};
    std::atomic_uint offers{0};
    std::atomic<std::uint64_t> offeredEpoch{0};
    NativeAssets refocused((root / "refocus-cache").string(), true, true, [&](const auto &record) {
        if (record.operation == AssetClipboardOperation::EpochAcknowledged)
            ++acknowledgments;
        if (record.operation == AssetClipboardOperation::Offer) {
            ++offers;
            offeredEpoch = record.epoch;
        }
    }, [] { return true; }, [] { assert(false); });
    arm.epoch = 10;
    refocused.receive(arm);
    for (unsigned turn = 0; turn < 200 && acknowledgments == 0; ++turn)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(acknowledgments == 1);
    ++generation;
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    refocused.poll();
    {
        std::unique_lock lock(gateMutex);
        assert(gateChanged.wait_for(lock, std::chrono::seconds(1), [] { return entered.load(); }));
    }
    auto pause = arm;
    pause.operation = AssetClipboardOperation::Pause;
    refocused.receive(pause);
    arm.epoch = 11;
    refocused.receive(arm);
    released = true;
    gateChanged.notify_all();
    for (unsigned turn = 0; turn < 200 && acknowledgments < 3; ++turn)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(acknowledgments == 3 && offers == 0); // New Arm cannot revive the old blocked image.
    ++generation;
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    refocused.poll();
    for (unsigned turn = 0; turn < 200 && offers == 0; ++turn)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(offers == 1 && offeredEpoch == 11);
    assert(refocused.stop());
    generation = 200;
    entered = false;
    released = false;
    completed = false;
    std::atomic_uint accepts{0};
    std::atomic_uint credits{0};
    std::atomic_uint cancelled{0};
    std::atomic_uint committed{0};
    NativeAssets incoming((root / "incoming-cache").string(), true, true, [&](const auto &record) {
        if (record.operation == AssetClipboardOperation::Accept)
            ++accepts;
        if (record.operation == AssetClipboardOperation::Credit)
            ++credits;
        if (record.operation == AssetClipboardOperation::CancelAcknowledged)
            ++cancelled;
        if (record.operation == AssetClipboardOperation::Committed)
            ++committed;
    }, [] { return true; }, [] { assert(false); });
    arm.epoch = 20;
    incoming.receive(arm);
    AssetClipboardRecord offer;
    offer.operation = AssetClipboardOperation::Offer;
    offer.epoch = 20;
    offer.serial = 1;
    offer.transfer[0] = 2;
    offer.format = AssetClipboardFormat::Png;
    offer.width = 1;
    offer.height = 1;
    offer.totalBytes = 9;
    offer.items = {{1, 9, "clipboard.png"}};
    incoming.receive(offer);
    for (unsigned turn = 0; turn < 200 && accepts == 0; ++turn)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(accepts == 1);
    auto chunk = offer;
    chunk.operation = AssetClipboardOperation::Chunk;
    chunk.item = 1;
    chunk.data = {137, 80, 78, 71, 13, 10, 26, 10, 1};
    incoming.receive(chunk);
    for (unsigned turn = 0; turn < 200 && credits == 0; ++turn)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(credits == 1);
    auto finish = offer;
    finish.operation = AssetClipboardOperation::Finish;
    finish.item = 1;
    finish.offset = 9; // Fake digest returns all zero bytes; this is a state-machine fixture, not a PNG decoder.
    incoming.receive(finish);
    {
        std::unique_lock lock(gateMutex);
        assert(gateChanged.wait_for(lock, std::chrono::seconds(1), [] { return entered.load(); }));
    }
    auto cancel = offer;
    cancel.operation = AssetClipboardOperation::Cancel;
    cancel.reason = "user_cancelled";
    incoming.receive(cancel); // Cancel arrives while no main-thread publication task exists yet.
    released = true;
    gateChanged.notify_all();
    for (unsigned turn = 0; turn < 200 && cancelled == 0; ++turn)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    incoming.poll();
    assert(cancelled == 1 && committed == 0);
    assert(incoming.stop());
    std::filesystem::remove_all(root);
    std::cout << "PASS no pre-Arm OS read/no initial export, off-main blocked image job, coalesced image backlog, responsive cancellation/sticky unknown unload, and blocked-normalizer Pause/refocus and pre-publication peer-Cancel fencing\n";
}
