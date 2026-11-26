#include "WorkerHostTemplate.h"
#include <cassert>
#include <iostream>

using namespace dockpipe::desktop;

namespace {
WorkerHostIdentity identity()
{
    return {std::string(64, 'a'), "/Applications/DockPipe.app/Contents/Helpers/dockpipe", std::string(64, 'b'),
        "/private/state", "/Applications/DockPipe.app/Contents/MacOS/DockPipe", std::string(64, 'c'), {}};
}

Json descriptor()
{
    const auto expected = identity();
    return Json{{"schema", "dockpipe.native-worker-host/v1"},
        {"module", {{"path", "/private/provider/module.so"}, {"sha256", expected.moduleSha256}}},
        {"cli", {{"path", expected.cliPath}, {"sha256", expected.cliSha256}}},
        {"state_directory", expected.stateDirectory},
        {"profile", {{"schema", "dockpipe.desktop.native.host/v1"}, {"operation", "virtual-display-owner"}}},
        {"trust", {{"mode", "review-adhoc"}, {"host_sha256", expected.hostSha256}}}};
}

bool accepts(const Json &value, const WorkerHostIdentity &expected = identity())
{
    try {
        validateWorkerHostTemplate(value, expected);
        return true;
    } catch (...) {
        return false;
    }
}
}

int main()
{
    const auto original = descriptor();
    assert(accepts(original));
    assert(parseWorkerHostTemplate(original.dump()) == original);
    for (const auto &key : {"module", "cli", "state_directory", "trust", "schema"}) {
        auto changed = original;
        changed.erase(key);
        assert(!accepts(changed));
    }
    auto changed = original;
    changed["module"]["sha256"] = std::string(64, 'd');
    assert(!accepts(changed));
    changed = original;
    changed["cli"]["path"] = "/private/other-cli";
    assert(!accepts(changed));
    changed = original;
    changed["state_directory"] = "/private/other-state";
    assert(!accepts(changed));
    changed = original;
    changed["trust"]["host_sha256"] = std::string(64, 'd');
    assert(!accepts(changed));
    changed = original;
    changed["module"]["path"] = "/private/../provider/module.so";
    assert(!accepts(changed));
    changed = original;
    changed["trust"]["team_identifier"] = "ABCDEFGHIJ";
    assert(!accepts(changed));
    changed = original;
    changed["profile"]["operation"] = "request-screen";
    assert(!accepts(changed));
    auto wrongHost = identity();
    wrongHost.hostExecutable = "/private/other-host";
    assert(!accepts(original, wrongHost));
    auto production = identity();
    production.productionTeamIdentifier = "ABCDEFGHIJ";
    assert(!accepts(original, production));
    changed = original;
    changed["trust"] = Json{{"mode", "production"}, {"team_identifier", "ABCDEFGHIJ"}};
    assert(accepts(changed, production));
    changed["trust"]["team_identifier"] = "OTHERTEAM1";
    assert(!accepts(changed, production));
    for (const auto &bytes : {std::string("{\"profile\":{},\"profile\":{}}"),
                             std::string("{\"module\":{\"path\":\"a\",\"p\\u0061th\":\"b\"}}")}) {
        bool rejected = false;
        try {
            parseWorkerHostTemplate(bytes);
        } catch (...) {
            rejected = true;
        }
        assert(rejected);
    }
    const auto deeplyNested = std::string(18, '[') + "0" + std::string(18, ']');
    bool nestingRejected = false;
    try {
        parseWorkerHostTemplate(deeplyNested);
    } catch (...) {
        nestingRejected = true;
    }
    assert(nestingRejected);
    Json child{
        {"schema", "dockpipe.desktop.native.host/v1"},
        {"operation", "virtual-display-owner"},
        {"request", {{"nonce", std::string(64, 'e')}}},
    };
    auto cloned = cloneVirtualDisplayHostDescriptor(original, child);
    assert(cloned["profile"] == child);
    cloned["profile"] = original["profile"];
    assert(cloned == original);
    assert(descriptor() == original);
    auto productionDescriptor = original;
    productionDescriptor["trust"] = Json{
        {"mode", "production"},
        {"team_identifier", "ABCDEFGHIJ"},
    };
    const auto productionClone = cloneVirtualDisplayHostDescriptor(productionDescriptor, child);
    assert(productionClone["trust"] == productionDescriptor["trust"]);
    assert(productionClone["module"] == productionDescriptor["module"]);
    assert(productionClone["cli"] == productionDescriptor["cli"]);
    assert(productionClone["state_directory"] == productionDescriptor["state_directory"]);
    auto observer = child;
    observer["operation"] = "virtual-display-observe";
    observer["request"] = Json{
        {"schema", "dockpipe.desktop.virtual-observer/v1"},
        {"nonce", std::string(64, 'e')},
        {"parent_pid", 1},
        {"display", 9},
        {"serial", 7},
        {"width", 1280},
        {"height", 720},
        {"deadline_unix_ms", std::int64_t(2000000000000)},
    };
    auto observerClone = cloneVirtualDisplayHostDescriptor(original, observer);
    assert(observerClone["profile"] == observer);
    observerClone["profile"] = original["profile"];
    assert(observerClone == original);
    auto productionObserver = cloneVirtualDisplayHostDescriptor(productionDescriptor, observer);
    assert(productionObserver["trust"] == productionDescriptor["trust"]);
    productionObserver["profile"] = productionDescriptor["profile"];
    assert(productionObserver == productionDescriptor);
    for (const auto operation : {"worker", "request-input", "configure-power"}) {
        child["operation"] = operation;
        bool rejected = false;
        try {
            cloneVirtualDisplayHostDescriptor(original, child);
        } catch (...) {
            rejected = true;
        }
        assert(rejected);
    }
    std::cout << "PASS exact current host/module/CLI/state/trust binding, duplicate rejection and profile-only cloning\n";
}
