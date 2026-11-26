#include "WorkerPackageContract.h"
#include "WorkerRuntime.h"
#include <cassert>
#include <iostream>

using namespace dockpipe::desktop;

namespace {
int stop(void *) { return 0; }

bool accepts(const Json &profile, std::uint32_t abi = DOCKPIPE_NATIVE_WORKER_ABI_V1)
{
    const auto encoded = profile.dump();
    DockpipeNativeWorkerHostV1 host{abi, sizeof(DockpipeNativeWorkerHostV1), "/trusted/cli", "/private/state",
        encoded.data(), encoded.size(), nullptr, stop};
    try {
        parseWorkerPackageProfile(host);
        return true;
    } catch (...) {
        return false;
    }
}
}

int main()
{
    const Json base{{"schema", "dockpipe.desktop.native.host/v1"}, {"operation", "status"}};
    assert(accepts(base));
    for (const auto &operation : {"request-screen", "request-input"}) {
        auto profile = base;
        profile["operation"] = operation;
        assert(accepts(profile));
    }
    auto power = base;
    power["operation"] = "power-settings";
    assert(accepts(power));
    power["mode"] = "enabled";
    assert(!accepts(power));
    power["operation"] = "configure-power";
    assert(accepts(power));
    power["mode"] = "unknown";
    assert(!accepts(power));
    power["mode"] = "off";
    assert(accepts(power));
    power = base;
    power["operation"] = "power-agent";
    power["channel_helper"] = "/trusted/helper";
    power["manifest"] = "/private/manifest";
    assert(!accepts(power));
    power["desktop_provider"] = Json{{"id", "dockpipe.desktop.native"}, {"version", "0.1.0"}, {"artifact", std::string(64, 'a')}};
    power["desktop_service_label"] = "com.example.desktop";
    assert(accepts(power));
    power["desktop_service_label"] = "other/service";
    assert(!accepts(power));
    power["desktop_service_label"] = "com.example.desktop";
    power["desktop_provider"]["artifact"] = "bad";
    assert(!accepts(power));
    auto child = base;
    child["operation"] = "virtual-display-owner";
    assert(!accepts(child));
    child["request"] = Json{
        {"schema", "dockpipe.desktop.virtual-owner/v1"},
        {"nonce", std::string(64, 'a')},
        {"serial", 7},
        {"width", 1280},
        {"height", 720},
        {"parent_pid", 1},
        {"create_deadline_unix_ms", std::int64_t(2000000000000)},
    };
    assert(accepts(child));
    child["channel_helper"] = "/not/permitted";
    assert(!accepts(child));
    child.erase("channel_helper");
    child["request"]["width"] = 1281;
    assert(!accepts(child));
    child["request"]["width"] = 1280;
    child["request"]["parent_pid"] = 0;
    assert(!accepts(child));
    child["request"]["parent_pid"] = 1;
    child["virtual_display_host_template"] = Json{{"path", "/private/template.json"}, {"sha256", std::string(64, 'a')}};
    assert(!accepts(child));
    auto observer = base;
    observer["operation"] = "virtual-display-observe";
    observer["request"] = Json{
        {"schema", "dockpipe.desktop.virtual-observer/v1"},
        {"nonce", std::string(64, 'b')},
        {"parent_pid", 1},
        {"display", 9},
        {"serial", 7},
        {"width", 1280},
        {"height", 720},
        {"deadline_unix_ms", std::int64_t(2000000000000)},
    };
    assert(accepts(observer));
    observer["request"]["display"] = 0;
    assert(!accepts(observer));
    observer["request"]["display"] = 9;
    observer["request"]["capture"] = true;
    assert(!accepts(observer));
    observer["request"].erase("capture");
    observer["channel_helper"] = "/not/permitted";
    assert(!accepts(observer));
    observer.erase("channel_helper");
    observer["virtual_display_host_template"] = Json{{"path", "/private/template.json"}, {"sha256", std::string(64, 'a')}};
    assert(!accepts(observer));
    auto profile = base;
    profile["operation"] = "worker";
    assert(!accepts(profile));
    profile["channel_helper"] = "/trusted/helper";
    profile["manifest"] = "/private/manifest";
    assert(accepts(profile));
    profile["virtual_display_host_template"] = Json{{"path", "/private/template.json"}, {"sha256", std::string(64, 'a')}};
    assert(accepts(profile));
    profile["virtual_display_host_template"]["sha256"] = "bad";
    assert(!accepts(profile));
    profile["virtual_display_host_template"]["sha256"] = std::string(64, 'a');
    profile["virtual_display_host_template"]["other"] = true;
    assert(!accepts(profile));
    profile["virtual_display_host_template"] = nullptr;
    assert(!accepts(profile));
    profile.erase("virtual_display_host_template");
    profile["channel_helper"] = std::string("/trusted/helper\0other", 21);
    assert(!accepts(profile));
    profile["channel_helper"] = "relative";
    assert(!accepts(profile));
    profile = base;
    profile["channel_helper"] = "/not/allowed/in/setup";
    assert(!accepts(profile));
    profile = base;
    profile["cli"] = "/wire/override";
    assert(!accepts(profile));
    assert(!accepts(base, 2));
    profile = base;
    profile["operation"] = "capture";
    assert(!accepts(profile));
    const auto encoded = base.dump();
    DockpipeNativeWorkerHostV1 host{1, sizeof(DockpipeNativeWorkerHostV1), "/trusted/cli", "/private/state",
        encoded.data(), DOCKPIPE_NATIVE_WORKER_PROFILE_LIMIT_V1 + 1, nullptr, stop};
    bool rejected = false;
    try { parseWorkerPackageProfile(host); } catch (...) { rejected = true; }
    assert(rejected); // Reject length before reading borrowed bytes.
    host.profile_json_bytes = encoded.size();
    host.stop_requested = nullptr;
    rejected = false;
    try { parseWorkerPackageProfile(host); } catch (...) { rejected = true; }
    assert(rejected);

    WorkerRuntime runtime;
    int polls = 0;
    runtime.stopSource = [&] { return ++polls >= 2; };
    assert(!runtime.cancelled());
    assert(runtime.cancelled());
    runtime.stopSource = [] { return false; };
    assert(runtime.cancelled()); // Host stop is revocation, never renewable.
    runtime.cleanupVerified = false;
    assert(!runtime.cleanupVerified); // Cancellation cannot assert cleanup.
    std::cout << "PASS strict operation profiles, borrowed bounds, ABI, path authority and sticky cancellation\n";
}
