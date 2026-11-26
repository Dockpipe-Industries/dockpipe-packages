#include "WorkerClipboard.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace dockpipe::desktop;

namespace {
template<typename Action>
bool rejected(Action action)
{
    try {
        action();
    } catch (const std::exception &) {
        return true;
    }
    return false;
}
class FakeClipboard final : public ClipboardAdapter {
public:
    std::int64_t count = 7;
    unsigned counts = 0;
    unsigned reads = 0;
    unsigned writes = 0;
    bool writeSucceeds = true;
    std::function<void()> afterWrite = [] {};
    ClipboardRead value{"previous remote clipboard", {}};
    std::int64_t changeCount() override { ++counts; return count; }
    ClipboardRead readText() override { ++reads; return value; }
    bool writeText(const std::string &text) override
    {
        ++writes;
        if (!writeSucceeds)
            return false;
        value = {text, {}};
        ++count;
        afterWrite();
        return true;
    }
};
}

int main()
{
    FakeClipboard adapter;
    bool allowed = true;
    std::vector<ClipboardRecord> emitted;
    std::vector<std::uint64_t> resets;
    unsigned discards = 0;
    WorkerClipboard clipboard(adapter, [&] { return allowed; },
        [&](ClipboardRecord record) { emitted.push_back(std::move(record)); },
        [&](std::uint64_t epoch) { resets.push_back(epoch); },
        [&](std::uint64_t epoch) { assert(epoch != 0); ++discards; });
    const auto now = std::chrono::steady_clock::now();
    clipboard.poll(now);
    assert(adapter.counts == 0 && adapter.reads == 0 && adapter.writes == 0);
    clipboard.receive({ClipboardOperation::Watch, 1, 0, {}}, now);
    assert(adapter.counts == 1 && adapter.reads == 0 && emitted.empty());
    ++adapter.count;
    clipboard.poll(now + std::chrono::milliseconds(99));
    assert(adapter.counts == 1);
    clipboard.poll(now + std::chrono::milliseconds(100));
    assert(adapter.counts == 2 && adapter.reads == 1 && emitted.size() == 1);
    assert(emitted.back().operation == ClipboardOperation::Changed);
    clipboard.receive({ClipboardOperation::Write, 1, 1, "fresh text"}, now);
    assert(discards == 2 && adapter.writes == 1 && adapter.value.text == "fresh text");
    assert(emitted.back().operation == ClipboardOperation::Acknowledged && emitted.back().revision == 1);
    const auto afterAck = emitted.size();
    clipboard.poll(now + std::chrono::milliseconds(200));
    assert(emitted.size() == afterAck); // The successful native write is not echoed.
    assert(rejected([&] { clipboard.receive({ClipboardOperation::Write, 1, 1, "replay"}, now); }));
    assert(rejected([&] { clipboard.receive({ClipboardOperation::Write, 1, 3, "gap"}, now); }));
    assert(rejected([&] { clipboard.receive({ClipboardOperation::Write, 2, 2, "stale epoch"}, now); }));
    assert(adapter.writes == 1);
    for (const auto &reason : {"too_large", "not_text", "unsupported_text", "unavailable"}) {
        ++adapter.count;
        adapter.value = {{}, reason};
        clipboard.poll(now + std::chrono::seconds(adapter.count));
        assert(emitted.back().operation == ClipboardOperation::Notice && emitted.back().text == reason);
    }
    for (const auto &text : {std::string(ClipboardMaximumTextBytes + 1, 'x'), std::string("x\0y", 3), std::string("\xff", 1)}) {
        ++adapter.count;
        adapter.value = {text, {}};
        clipboard.poll(now + std::chrono::seconds(adapter.count));
        assert(emitted.back().operation == ClipboardOperation::Notice);
    }
    ++adapter.count;
    adapter.value = {std::string(ClipboardMaximumTextBytes, 'x'), {}};
    clipboard.poll(now + std::chrono::seconds(adapter.count));
    assert(emitted.back().operation == ClipboardOperation::Changed
        && emitted.back().text.size() == ClipboardMaximumTextBytes);
    ++adapter.count;
    adapter.value = {"Unicode: \xf0\x9f\x8c\x8d", {}};
    clipboard.poll(now + std::chrono::seconds(adapter.count));
    assert(emitted.back().operation == ClipboardOperation::Changed);
    const auto readsBeforeWatch = adapter.reads;
    clipboard.receive({ClipboardOperation::Pause, 1, 0, {}}, now);
    const auto pausedCounts = adapter.counts;
    clipboard.poll(now + std::chrono::hours(1));
    assert(adapter.counts == pausedCounts && resets.back() == 0);
    assert(rejected([&] { clipboard.receive({ClipboardOperation::Write, 1, 2, "paused"}, now); }));
    assert(rejected([&] { clipboard.receive({ClipboardOperation::Watch, 1, 0, {}}, now); }));
    clipboard.receive({ClipboardOperation::Watch, 2, 0, {}}, now);
    assert(adapter.reads == readsBeforeWatch); // Watch never reads previously present contents.
    adapter.writeSucceeds = false;
    const auto beforeFailure = emitted.size();
    clipboard.receive({ClipboardOperation::Write, 2, 1, {}}, now);
    assert(emitted.size() == beforeFailure + 1 && emitted.back().operation == ClipboardOperation::Notice);
    assert(rejected([&] { clipboard.receive({ClipboardOperation::Write, 2, 2, "after failure"}, now); }));
    clipboard.receive({ClipboardOperation::Watch, 3, 0, {}}, now);
    allowed = false;
    const auto deniedCounts = adapter.counts;
    const auto deniedWrites = adapter.writes;
    assert(rejected([&] { clipboard.poll(now + std::chrono::seconds(1)); }));
    assert(rejected([&] { clipboard.receive({ClipboardOperation::Write, 3, 1, "expired"}, now); }));
    assert(adapter.counts == deniedCounts && adapter.writes == deniedWrites);
    clipboard.stop();
    clipboard.poll(now + std::chrono::hours(2));
    assert(adapter.counts == deniedCounts);

    // A lease/policy loss between initial admission and the OS call rejects the
    // operation without touching the adapter or acknowledging any write.
    FakeClipboard crossing;
    unsigned admissions = 0;
    WorkerClipboard expired(crossing, [&] { return ++admissions == 1; },
        [&](ClipboardRecord) { assert(false); }, [](std::uint64_t) {}, [](std::uint64_t) {});
    assert(rejected([&] { expired.receive({ClipboardOperation::Watch, 1, 0, {}}, now); }));
    assert(crossing.counts == 0 && crossing.writes == 0 && crossing.reads == 0);
    FakeClipboard revoked;
    bool live = true;
    unsigned acknowledgements = 0;
    WorkerClipboard noLateAck(revoked, [&] { return live; },
        [&](ClipboardRecord record) { acknowledgements += record.operation == ClipboardOperation::Acknowledged; },
        [](std::uint64_t) {}, [](std::uint64_t) {});
    noLateAck.receive({ClipboardOperation::Watch, 1, 0, {}}, now);
    revoked.afterWrite = [&] { live = false; };
    assert(rejected([&] { noLateAck.receive({ClipboardOperation::Write, 1, 1, "written before revocation"}, now); }));
    assert(revoked.writes == 1 && acknowledgements == 0);
    noLateAck.stop();
    std::cout << "PASS baseline-only Watch,100ms gate,epochs,exact writes,ACK after success,no echo,notices,Pause,stop,authority loss\n";
}
