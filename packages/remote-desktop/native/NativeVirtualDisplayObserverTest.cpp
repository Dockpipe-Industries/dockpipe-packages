#include "NativeVirtualDisplayObserver.h"
#include "NativeVirtualDisplayOwnerProtocol.h"
#include <cassert>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

using namespace dockpipe::desktop;
using Json = nlohmann::json;
using Clock = NativeVirtualDisplay::Clock;
using namespace std::chrono_literals;
namespace {
Json observation(const VirtualDisplayObserverRequest &request, int pid)
{
    auto result = virtualDisplayObserverMessage(request, "observed");
    result["pid"] = pid;
    result["observation"] = {{"online_complete", true}, {"online", true}, {"active", true},
        {"mirrored", false}, {"mirror_target", 0}, {"vendor", 0x4450}, {"product", 1},
        {"serial", request.spec.serial}, {"pixels_width", request.spec.width}, {"pixels_height", request.spec.height},
        {"bounds_width", request.spec.width}, {"bounds_height", request.spec.height}, {"mode_present", true},
        {"mode_width", request.spec.width}, {"mode_height", request.spec.height},
        {"mode_pixel_width", request.spec.width}, {"mode_pixel_height", request.spec.height}, {"refresh_rate", 60}};
    return result;
}
Json readFile(const std::string &path)
{
    std::ifstream input(path);
    Json value;
    input >> value;
    return value;
}
void writeFile(const std::string &path, const std::string &bytes, mode_t mode)
{
    std::ofstream output(path);
    output << bytes;
    output.close();
    assert(chmod(path.c_str(), mode) == 0);
}
int child(const std::string &descriptor)
{
    const auto definition = readFile(descriptor);
    const auto request = parseVirtualDisplayObserverRequest(definition.at("profile"));
    const auto fixture = definition.at("fixture").get<std::string>();
    const auto directory = descriptor.substr(0, descriptor.find_last_of('/'));
    const auto snapshotDirectory = directory + "/host-tmp/qt-private";
    assert(mkdir(snapshotDirectory.c_str(), 0700) == 0);
    const auto snapshot = snapshotDirectory + "/native-worker-module";
    writeFile(snapshot, "observer fixture", 0500);
    auto admitted = virtualDisplayObserverMessage(request, "admitted");
    admitted["pid"] = getpid();
    admitted["snapshot_path"] = snapshot;
    std::cout << admitted.dump() << std::endl;
    std::string line;
    if (!std::getline(std::cin, line))
        return 0;
    validateVirtualDisplayObserverMessage(parseVirtualDisplayOwnerJson(line), request, "observe", 6);
    if (fixture == "eof")
        return 0;
    auto result = observation(request, getpid());
    if (fixture == "wrong-mode")
        result["observation"]["mode_pixel_width"] = 1920;
    if (fixture == "wrong-pid")
        result["pid"] = getpid() + 1;
    if (fixture == "changed-snapshot") {
        assert(chmod(snapshot.c_str(), 0600) == 0);
        writeFile(snapshot, "changed observer fixture", 0500);
    }
    std::cout << result.dump() << std::endl;
    if (fixture == "immediate-exit")
        _exit(0);
    if (fixture == "hang") {
        signal(SIGTERM, SIG_IGN);
        for (;;)
            pause();
    }
    return fixture == "nonzero" ? 7 : 0;
}
struct Fixture {
    std::string self;
    std::string mode = "healthy";
    std::string directory;
    unsigned writes = 0;
    bool cancelled = false;
    bool cancelAfterSnapshot = false;
    VirtualDisplayOwnerContext context()
    {
        return {self, [this](const std::string &path, const Json &profile) {
            ++writes;
            directory = path;
            writeFile(path + "/child-host.json", Json{{"profile", profile}, {"fixture", mode}}.dump(), 0600);
            return path + "/child-host.json";
        }, [this](const std::string &path) {
            if (cancelAfterSnapshot)
                cancelled = true;
            std::ifstream input(path);
            return std::string(std::istreambuf_iterator<char>(input), {}) == "observer fixture";
        }};
    }
    ~Fixture()
    {
        if (directory.empty())
            return;
        unlink((directory + "/host-tmp/qt-private/native-worker-module").c_str());
        rmdir((directory + "/host-tmp/qt-private").c_str());
        rmdir((directory + "/host-tmp").c_str());
        unlink((directory + "/snapshot-cleanup-unknown.json").c_str());
        unlink((directory + "/child-host.json").c_str());
        rmdir(directory.c_str());
    }
};
VirtualDisplayObserverRequest request()
{
    return {{3840, 2160, 12345}, 42, std::string(64, 'a'), getpid(),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::system_clock::now() + 1s).time_since_epoch()).count()};
}
template<class Function> bool rejects(Function action)
{
    try {
        action();
        return false;
    } catch (...) {
        return true;
    }
}
void protocol()
{
    const auto expected = request();
    const auto decoded = parseVirtualDisplayObserverRequest(parseVirtualDisplayOwnerJson(virtualDisplayObserverProfile(expected).dump()));
    assert(decoded.display == expected.display && decoded.spec.serial == expected.spec.serial);
    const auto valid = observation(expected, 123);
    assert(exactVirtualMode(parseVirtualDisplayObservation(valid, expected, 123), expected.spec));
    for (const auto field : {"nonce", "serial", "display", "parent_pid", "pid"}) {
        auto invalid = valid;
        if (std::string(field) == "nonce")
            invalid[field] = std::string(64, 'b');
        else
            invalid[field] = 0;
        assert(rejects([&] { parseVirtualDisplayObservation(invalid, expected, 123); }));
    }
    for (const auto field : {"online_complete", "online", "active", "mode_present"}) {
        auto invalid = valid;
        invalid["observation"][field] = false;
        assert(rejects([&] { parseVirtualDisplayObservation(invalid, expected, 123); }));
    }
    for (const auto field : {"pixels_width", "bounds_width", "mode_width", "mode_pixel_width", "serial", "refresh_rate"}) {
        auto invalid = valid;
        invalid["observation"][field] = 0;
        assert(rejects([&] { parseVirtualDisplayObservation(invalid, expected, 123); }));
    }
    auto invalid = valid;
    invalid["observation"]["refresh_rate"] = nullptr;
    assert(rejects([&] { parseVirtualDisplayObservation(invalid, expected, 123); }));
    auto badProfile = virtualDisplayObserverProfile(expected);
    badProfile["request"]["height"] = 999;
    assert(rejects([&] { parseVirtualDisplayObserverRequest(badProfile); }));
}
void processes(const std::string &self)
{
    for (const std::string mode : {"healthy", "immediate-exit", "wrong-mode", "wrong-pid", "eof", "nonzero", "hang", "changed-snapshot"}) {
        Fixture fixture;
        fixture.self = self;
        fixture.mode = mode;
        VirtualDisplayModeObserver observer(fixture.context());
        assert(fixture.writes == 0);
        const auto started = Clock::now();
        const bool accepted = observer.observe(request(), started + 250ms, [] { return true; });
        assert(accepted == (mode == "healthy" || mode == "immediate-exit"));
        assert(bool(observer.mode()) == accepted);
        assert(Clock::now() - started < 400ms);
        assert(observer.close(Clock::now() + 600ms));
        if (accepted)
            assert(access(fixture.directory.c_str(), F_OK) != 0);
        if (mode == "changed-snapshot")
            assert(access((fixture.directory + "/host-tmp/qt-private/native-worker-module").c_str(), F_OK) == 0);
    }
    Fixture cancelled;
    cancelled.self = self;
    cancelled.cancelAfterSnapshot = true;
    VirtualDisplayModeObserver observer(cancelled.context());
    assert(!observer.observe(request(), Clock::now() + 300ms, [&] { return !cancelled.cancelled; }));
    assert(!observer.mode());
    assert(observer.close(Clock::now() + 500ms));
}
}
int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--native-worker-host" && std::string(argv[2]) == "--request")
        return child(argv[3]);
    assert(argc == 1);
    char executable[4096];
    assert(realpath(argv[0], executable));
    protocol();
    processes(executable);
}
