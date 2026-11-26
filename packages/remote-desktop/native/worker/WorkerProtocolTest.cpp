#include "WorkerProtocol.h"
#include <cassert>
#include <iostream>

using namespace dockpipe::desktop;
namespace {
Json provider{{"id", "dockpipe.desktop.native"}, {"version", "0.1.0"}, {"artifact", std::string(64, 'a')}};
Json catalog{{"schema", "dockpipe.remote.desktop/v1"}, {"target", Json{{"node", "fixture"}, {"fingerprint", std::string(64, 'b')}, {"generation", std::string(64, 'c')}}},
    {"provider", provider}, {"epoch", std::string(64, 'd')}, {"view", true}, {"input", false}};
Json directive()
{
    Json open{{"schema", "dockpipe.remote.desktop/v1"}, {"id", std::string(64, '1')}, {"target", catalog["target"]}, {"epoch", catalog["epoch"]},
        {"provider", provider}, {"mode", "view"}, {"nonce", std::string(64, '2')}, {"deadline", "2030-01-01T00:00:00Z"}};
    return Json{{"open", open}, {"lease_millis", 10000}, {"close", false}, {"ack", 0}};
}
template<typename Action> bool rejected(Action action)
{
    try { action(); } catch (const std::exception &) { return true; }
    return false;
}
}
int main()
{
    const auto now = Clock::now();
    WorkerProtocol state(catalog, provider);
    auto value = directive();
    assert(state.directive(value, now));
    state.localPin(std::string(64, '3'));
    assert(!state.canConnect());
    value["ack"] = 1;
    value["signals"] = Json::array({Json{{"reference", state.reference()}, {"sequence", 1}, {"type", "binding"}, {"fingerprint", std::string(64, '4')}}});
    assert(!state.directive(value, now));
    assert(state.canConnect());
    state.connecting();
    assert(rejected([&] { state.running(); }));
    const auto ready = Json{{"type", "ready"}, {"reference", state.reference()}, {"role", "worker"}, {"mode", "view"}, {"capture", Json{{"quality", "high"}, {"frame_rate", 30}}}};
    auto wrong = ready;
    wrong["role"] = "viewer";
    assert(rejected([&] { state.ready(wrong); }));
    for (const auto *capability : {"clipboard_text", "clipboard_images", "clipboard_files"}) {
        auto richView = ready;
        richView["capture"][capability] = true;
        assert(rejected([&] { state.ready(richView); }));
    }
    auto richCapture = ready["capture"];
    richCapture["clipboard_images"] = true;
    richCapture["clipboard_files"] = true;
    const auto richProfile = parseCaptureProfile(richCapture);
    assert(richProfile.clipboardImages && richProfile.clipboardFiles && !richProfile.clipboardText);
    for (const auto *capability : {"clipboard_images", "clipboard_files"}) {
        for (const auto &invalid : {Json(false), Json(nullptr), Json("true")}) {
            auto changed = richCapture;
            changed[capability] = invalid;
            assert(rejected([&] { parseCaptureProfile(changed); }));
        }
    }
    state.ready(ready);
    state.running();
    assert(state.report()["report"]["state"] == "running");
    value["open"]["target"]["generation"] = std::string(64, '5');
    assert(rejected([&] { state.directive(value, now); }));
    state.stop();
    state.cleanup(true, false);
    assert(state.phase() == WorkerProtocol::Phase::Unknown);
    assert(state.report()["report"]["state"] == "unknown");
    value = directive();
    assert(rejected([&] { state.directive(value, now); }));
    WorkerProtocol closed(catalog, provider);
    value["close"] = true;
    value["lease_millis"] = 0;
    assert(closed.directive(value, now));
    closed.cleanup(true, true);
    assert(closed.report()["report"]["state"] == "closed");
    WorkerProtocol viewOnly(catalog, provider);
    value = directive();
    value["open"]["mode"] = "control";
    assert(rejected([&] { viewOnly.directive(value, now); }));
    auto input = parseInput(Json{{"sequence", std::uint64_t(1)}, {"kind", "key"}, {"hid_usage", 4}, {"down", true}});
    assert(input.hidUsage == 4 && input.down);
    assert(rejected([&] { parseInput(Json{{"sequence", std::uint64_t(1)}, {"kind", "key"}, {"hid_usage", 65536}, {"down", true}}); }));
    std::cout << "PASS frozen target/mode, both binding acknowledgments, exact ready, unknown retention, closed proof, input parsing\n";
}
