#include "WorkerReports.h"
#include "ChildProcess.h"
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
}

int main()
{
    const Json provider{{"id", "dockpipe.desktop.native"}, {"version", "0.1.0"}, {"artifact", std::string(64, 'a')}};
    const Json catalog{{"schema", "dockpipe.remote.desktop/v1"}, {"provider", provider}, {"epoch", std::string(64, 'd')},
        {"target", Json{{"node", "fixture"}, {"fingerprint", std::string(64, 'b')}, {"generation", std::string(64, 'c')}}},
        {"view", true}, {"input", true}};
    const Json open{{"schema", "dockpipe.remote.desktop/v1"}, {"provider", provider}, {"epoch", catalog["epoch"]},
        {"target", catalog["target"]}, {"id", std::string(64, '1')}, {"nonce", std::string(64, '2')},
        {"deadline", "2030-01-01T00:00:00Z"}, {"mode", "control"}};
    auto directive = Json{{"open", open}, {"lease_millis", 10000}, {"close", false}, {"ack", 0}};
    WorkerProtocol protocol(catalog, provider);
    WorkerReports reports;
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    unsigned writes = 0;
    const auto write = [&](std::string_view line) {
        ++writes;
        return writeBytes(descriptors[1], reinterpret_cast<const std::uint8_t *>(line.data()),
            line.size(), Clock::now() + std::chrono::seconds(1));
    };
    const auto now = Clock::now();
    assert(protocol.directive(directive, now));
    protocol.localPin(std::string(64, '3'));
    assert(reports.publish(protocol.report(), write)); // Binding must reach Core.
    assert(!reports.publish(protocol.report(), write));
    directive["ack"] = 1;
    directive["signals"] = Json::array({Json{{"reference", protocol.reference()}, {"sequence", 1},
        {"type", "binding"}, {"fingerprint", std::string(64, '4')}}});
    assert(!protocol.directive(directive, now));
    assert(reports.publish(protocol.report(), write)); // ACK/signal changes must reach Core.
    protocol.connecting();
    protocol.ready(Json{{"type", "ready"}, {"reference", protocol.reference()}, {"role", "worker"}, {"mode", "control"}, {"capture", Json{{"quality", "high"}, {"frame_rate", 30}}}});
    protocol.running();
    assert(reports.publish(protocol.report(), write));
    for (unsigned iteration = 1; iteration <= 350; ++iteration) {
        const auto receivedAt = now + std::chrono::milliseconds(iteration * 100);
        assert(!protocol.directive(directive, receivedAt));
        assert(protocol.lease() == receivedAt + std::chrono::seconds(10));
        assert(!reports.publish(protocol.report(), write));
    }
    auto invalid = directive;
    invalid["open"]["target"]["generation"] = std::string(64, 'e');
    assert(rejected([&] { protocol.directive(invalid, now); })); // Dedupe never skips validation.
    protocol.stop();
    protocol.cleanup(true, true);
    const auto closed = protocol.report();
    assert(rejected([&] { reports.publish(closed, [](std::string_view) { return false; }); }));
    assert(reports.publish(closed, write)); // Failed writes cannot commit/suppress terminal state.
    assert(!reports.publish(closed, write));
    assert(writes == 4); // 350 renewals do not place stale Running records before Closed.
    const auto retained = reports.retainedBytes();
    unsigned oversizedWrites = 0;
    Json oversized{{"type", "report"}, {"report", std::string(WorkerReports::MaximumBytes + 1, 'x')}};
    assert(rejected([&] { reports.publish(oversized, [&](std::string_view) { ++oversizedWrites; return true; }); }));
    assert(oversizedWrites == 0 && reports.retainedBytes() == retained && retained <= WorkerReports::MaximumBytes);
    close(descriptors[1]);
    RecordReader reader;
    std::vector<Json> observed;
    for (;;) {
        const auto outcome = reader.poll(descriptors[0], WorkerReports::MaximumBytes, true);
        if (outcome == RecordReader::Outcome::Closed)
            break;
        assert(outcome == RecordReader::Outcome::Complete);
        observed.push_back(Json::parse(reader.take()));
    }
    close(descriptors[0]);
    assert(observed.size() == 4);
    assert(observed[0]["report"]["signals"].size() == 1);
    assert(observed[1]["report"]["ack"] == 1 && observed[1]["report"]["signals"].empty());
    assert(observed[2]["report"]["state"] == "running");
    assert(observed[3] == closed && observed[3]["report"]["reference"] == protocol.reference());

    WorkerReports unknownReports;
    Json unknown = closed;
    unknown["report"]["state"] = "unknown";
    assert(unknownReports.publish(closed, [](std::string_view) { return true; }));
    assert(unknownReports.publish(unknown, [](std::string_view) { return true; }));
    unknown["report"]["reference"]["id"] = std::string(64, '5');
    assert(unknownReports.publish(unknown, [](std::string_view) { return true; }));
    std::cout << "PASS real-pipe binding/ACK/Running/Closed transitions, 350 validated renewals without duplicates, failed-write retry and bounded single-report cache\n";
}
