#include "WorkerChannel.h"
#include <sys/ioctl.h>
#include "WorkerCleanup.h"
#include <cassert>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

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

std::pair<RecordReader::Outcome, std::vector<std::uint8_t>> pipeRecord(const std::vector<std::uint8_t> &bytes)
{
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    if (!bytes.empty())
        assert(write(descriptors[1], bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    close(descriptors[1]);
    RecordReader reader;
    const auto outcome = reader.poll(descriptors[0], 4097);
    auto record = outcome == RecordReader::Outcome::Complete ? reader.take() : std::vector<std::uint8_t>{};
    close(descriptors[0]);
    return {outcome, record};
}

std::vector<std::uint8_t> envelope(const std::vector<std::uint8_t> &payload)
{
    assert(payload.size() < 256);
    std::vector<std::uint8_t> result{0, 0, 0, static_cast<std::uint8_t>(payload.size())};
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
}

int main()
{
    const auto encodedClipboard = encodeClipboardRecord({ClipboardOperation::Watch, 1, 0, {}});
    std::vector<std::uint8_t> clipboardBytes{4};
    clipboardBytes.insert(clipboardBytes.end(), encodedClipboard.begin(), encodedClipboard.end());
    assert(rejected([&] { workerChannelMessage(true, false, RecordReader::Outcome::Complete, clipboardBytes, false, true); }));
    assert(rejected([&] { workerChannelMessage(true, false, RecordReader::Outcome::Complete, clipboardBytes, true, false); }));
    assert(workerChannelMessage(true, false, RecordReader::Outcome::Complete, clipboardBytes, true, true).kind == WorkerChannelMessage::Kind::Clipboard);
    assert(workerChannelMessage(true, true, RecordReader::Outcome::Complete, clipboardBytes, true, true).kind == WorkerChannelMessage::Kind::Stop);
    auto wrongDirection = encodeClipboardRecord({ClipboardOperation::Acknowledged, 1, 1, {}});
    wrongDirection.insert(wrongDirection.begin(), 4);
    assert(rejected([&] { workerChannelMessage(true, false, RecordReader::Outcome::Complete, wrongDirection, true, true); }));
    clipboardBytes.pop_back();
    assert(rejected([&] { workerChannelMessage(true, true, RecordReader::Outcome::Complete, clipboardBytes, true, true); }));
    const auto eof = pipeRecord({});
    assert(eof.first == RecordReader::Outcome::Closed);
    assert(workerChannelMessage(true, false, eof.first, eof.second, false).kind == WorkerChannelMessage::Kind::Stop);
    assert(rejected([&] { workerChannelMessage(false, true, eof.first, eof.second, false); }));
    for (const auto &bytes : std::vector<std::vector<std::uint8_t>>{{0, 0, 0}, {0, 0, 0, 2, 3}, {0, 0, 32, 0}}) {
        const auto malformed = pipeRecord(bytes);
        assert(malformed.first == RecordReader::Outcome::Invalid);
        assert(rejected([&] { workerChannelMessage(true, true, malformed.first, malformed.second, true); }));
    }
    const auto keyframe = pipeRecord(envelope({3, 1}));
    assert(workerChannelMessage(true, false, keyframe.first, keyframe.second, false).kind == WorkerChannelMessage::Kind::Keyframe);
    assert(workerChannelMessage(true, true, keyframe.first, keyframe.second, false).kind == WorkerChannelMessage::Kind::Stop);
    const auto invalid = pipeRecord(envelope({9, 1}));
    assert(rejected([&] { workerChannelMessage(true, true, invalid.first, invalid.second, true); }));
    const auto input = Json{{"sequence", 1}, {"kind", "key"}, {"hid_usage", 4}, {"down", false}}.dump();
    std::vector<std::uint8_t> payload{2};
    payload.insert(payload.end(), input.begin(), input.end());
    const auto complete = pipeRecord(envelope(payload));
    assert(rejected([&] { workerChannelMessage(true, true, complete.first, complete.second, false); }));
    assert(workerChannelMessage(true, true, complete.first, complete.second, true).kind == WorkerChannelMessage::Kind::Stop);
    assert(workerChannelMessage(true, false, complete.first, complete.second, true).kind == WorkerChannelMessage::Kind::Input);

    const Json provider{{"id", "dockpipe.desktop.native"}, {"version", "0.1.0"}, {"artifact", std::string(64, 'a')}};
    const Json catalog{{"schema", "dockpipe.remote.desktop/v1"}, {"provider", provider}, {"epoch", std::string(64, 'd')},
        {"target", Json{{"node", "fixture"}, {"fingerprint", std::string(64, 'b')}, {"generation", std::string(64, 'c')}}},
        {"view", true}, {"input", false}};
    const Json open{{"schema", "dockpipe.remote.desktop/v1"}, {"provider", provider}, {"epoch", catalog["epoch"]},
        {"target", catalog["target"]}, {"id", std::string(64, '1')}, {"nonce", std::string(64, '2')},
        {"deadline", "2030-01-01T00:00:00Z"}, {"mode", "view"}};
    for (const bool nativeStartSucceeds : {false, true}) {
        auto controlCatalog = catalog;
        controlCatalog["input"] = true;
        auto controlOpen = open;
        controlOpen["mode"] = "control";
        WorkerProtocol protocol(controlCatalog, provider);
        Json directive{{"open", controlOpen}, {"lease_millis", 10000}, {"close", false}, {"ack", 0}};
        protocol.directive(directive, Clock::now());
        protocol.localPin(std::string(64, '3'));
        directive["ack"] = 1;
        directive["signals"] = Json::array({Json{{"reference", protocol.reference()}, {"sequence", 1},
            {"type", "binding"}, {"fingerprint", std::string(64, '4')}}});
        protocol.directive(directive, Clock::now());
        protocol.connecting();
        protocol.ready(Json{{"type", "ready"}, {"reference", protocol.reference()}, {"role", "worker"},
            {"mode", "control"}, {"capture", Json{{"quality", "high"}, {"frame_rate", 30}, {"clipboard_text", true}}}});
        int descriptors[2];
        assert(pipe(descriptors) == 0);
        assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
        auto earlyWatch = encodeClipboardRecord({ClipboardOperation::Watch, 1, 0, {}});
        earlyWatch.insert(earlyWatch.begin(), 4);
        const auto bytes = envelope(earlyWatch);
        assert(write(descriptors[1], bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
        RecordReader reader;
        for (unsigned waiting = 0; waiting < 3; ++waiting) {
            assert(pollWorkerChannel(reader, descriptors[0], protocol.phase(), false, true, true).kind == WorkerChannelMessage::Kind::None);
            int buffered = 0;
            assert(ioctl(descriptors[0], FIONREAD, &buffered) == 0 && buffered == static_cast<int>(bytes.size()));
        }
        if (nativeStartSucceeds) {
            protocol.running();
            const auto message = pollWorkerChannel(reader, descriptors[0], protocol.phase(), false, true, true);
            assert(message.kind == WorkerChannelMessage::Kind::Clipboard && message.clipboard.operation == ClipboardOperation::Watch);
        } else {
            // Failed start follows ordinary native/child cleanup without ever
            // interpreting the early clipboard record or touching the OS.
            protocol.stop();
            assert(pollWorkerChannel(reader, descriptors[0], protocol.phase(), false, true, true).kind == WorkerChannelMessage::Kind::None);
            assert(pollWorkerChannel(reader, descriptors[0], protocol.phase(), true, true, true).kind == WorkerChannelMessage::Kind::Stop);
            int buffered = 0;
            assert(ioctl(descriptors[0], FIONREAD, &buffered) == 0 && buffered == static_cast<int>(bytes.size()));
            protocol.cleanup(true, true);
            assert(protocol.phase() == WorkerProtocol::Phase::Closed);
        }
        close(descriptors[0]);
        close(descriptors[1]);
    }
    for (const bool nativeVerified : {false, true}) {
        for (const bool childVerified : {false, true}) {
            WorkerProtocol protocol(catalog, provider);
            assert(protocol.directive(Json{{"open", open}, {"lease_millis", 1000}, {"close", false}, {"ack", 0}}, Clock::now()));
            if (workerChannelMessage(true, true, eof.first, eof.second, false).kind == WorkerChannelMessage::Kind::Stop) {
                protocol.stop();
                protocol.cleanup(nativeVerified, childVerified);
            }
            const bool verified = nativeVerified && childVerified;
            assert(protocol.report()["report"]["state"] == (verified ? "closed" : "unknown"));
            Json forget{{"type", "forget"}, {"reference", protocol.reference()}};
            assert(exactCleanupForget(forget, protocol.reference(), verified) == verified);
            assert(!exactCleanupForget(Json{}, protocol.reference(), verified));
            forget["reference"]["epoch"] = std::string(64, 'e');
            assert(!exactCleanupForget(forget, protocol.reference(), verified));
        }
    }
    AssetClipboardRecord arm;
    arm.operation = AssetClipboardOperation::Arm;
    arm.epoch = 1;
    auto assetBytes = encodeAssetClipboardRecord(arm);
    assetBytes.insert(assetBytes.begin(), 5);
    assert(rejected([&] {
        workerChannelMessage(true, false, RecordReader::Outcome::Complete, assetBytes, false, false, true, true);
    }));
    assert(rejected([&] {
        workerChannelMessage(true, false, RecordReader::Outcome::Complete, assetBytes, true, false, false, false);
    }));
    const auto asset = workerChannelMessage(true, false, RecordReader::Outcome::Complete, assetBytes, true, false, true, false);
    assert(asset.kind == WorkerChannelMessage::Kind::Asset && asset.asset.epoch == 1);
    std::cout << "PASS real-pipe EOF close, preReady failure, exited malformed/truncated rejection, no late input, and unchanged native/child/exact-Forget proof,early Watch pipe backpressure until Running,start-failure cleanup\n";
}
