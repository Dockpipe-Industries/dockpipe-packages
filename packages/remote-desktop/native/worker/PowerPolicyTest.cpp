#include "PowerPolicyFile.h"
#include <cassert>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace dockpipe::desktop;

namespace {
template <typename Function> bool rejects(Function function)
{
    try {
        function();
        return false;
    } catch (...) {
        return true;
    }
}
}

int main()
{
    const Json off{{"schema", "dockpipe.desktop.power/v1"}, {"mode", "off"}};
    assert(parsePowerPolicyRecord(off) == PowerPolicy::Off);
    for (const auto policy : {PowerPolicy::Off, PowerPolicy::Connected, PowerPolicy::Enabled}) {
        assert(parsePowerPolicy(powerPolicyName(policy)) == policy);
        for (const bool running : {false, true})
            assert(connectedPowerAssertion(policy, running) == (policy == PowerPolicy::Connected && running));
        for (const bool enabled : {false, true}) {
            for (const bool registered : {false, true})
                assert(enabledPowerAssertion(policy, enabled, registered)
                    == (policy == PowerPolicy::Enabled && enabled && registered));
        }
    }
    for (const auto mode : {"", "while-connected", "ENABLED", "unknown"})
        assert(rejects([&] { parsePowerPolicy(mode); }));
    auto invalid = off;
    invalid["permission"] = true;
    assert(rejects([&] { parsePowerPolicyRecord(invalid); }));
    invalid = off;
    invalid["mode"] = true;
    assert(rejects([&] { parsePowerPolicyRecord(invalid); }));
    invalid = off;
    invalid["schema"] = "foreign";
    assert(rejects([&] { parsePowerPolicyRecord(invalid); }));

    char path[] = "/tmp/dockpipe-power-policy-test-XXXXXX";
    assert(mkdtemp(path));
    {
        PowerPolicyDirectory directory(path);
        assert(!directory.read("desktop-power.json"));
        directory.write("desktop-power.json", off);
        assert(directory.read("desktop-power.json") == off);
        const std::string file = std::string(path) + "/desktop-power.json";
        struct stat metadata{};
        assert(lstat(file.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0600);
        auto enabled = off;
        enabled["mode"] = "enabled";
        directory.write("desktop-power.json", enabled);
        assert(directory.read("desktop-power.json") == enabled);
        assert(chmod(file.c_str(), 0644) == 0);
        assert(rejects([&] { directory.read("desktop-power.json"); }));
        assert(chmod(file.c_str(), 0600) == 0);
        assert(unlink(file.c_str()) == 0);
        assert(symlink("missing", file.c_str()) == 0);
        assert(rejects([&] { directory.read("desktop-power.json"); }));
        assert(rejects([&] { directory.write("desktop-power.json", off); }));
        assert(unlink(file.c_str()) == 0);
        assert(mkfifo(file.c_str(), 0600) == 0);
        assert(rejects([&] { directory.read("desktop-power.json"); }));
        assert(unlink(file.c_str()) == 0);
    }
    assert(chmod(path, 0755) == 0);
    assert(rejects([&] { PowerPolicyDirectory directory(path); }));
    assert(rmdir(path) == 0);
    std::cout << "PASS strict saved policy, independent lifetime decisions, private atomic metadata and special-file rejection\n";
}
