#include "NativeVirtualDisplayOwnerClient.h"
#include "NativeVirtualDisplayOwnerProtocol.h"
#include "NativeVirtualDisplayObserver.h"
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
Json readFile(const std::string &path)
{
    std::ifstream input(path);
    Json value;
    input >> value;
    return value;
}
void writeFile(const std::string &path, const Json &value)
{
    std::ofstream output(path);
    output << value.dump();
    output.close();
    assert(chmod(path.c_str(), 0600) == 0);
}

int fakeObserver(const Json &definition, const std::string &descriptor)
{
    const auto request = parseVirtualDisplayObserverRequest(definition.at("profile"));
    const auto directory = descriptor.substr(0, descriptor.find_last_of('/'));
    const auto snapshotDirectory = directory + "/host-tmp/qt-private";
    assert(mkdir(snapshotDirectory.c_str(), 0700) == 0);
    const auto snapshotPath = snapshotDirectory + "/native-worker-module";
    {
        std::ofstream output(snapshotPath);
        output << "fixture module";
    }
    assert(chmod(snapshotPath.c_str(), 0500) == 0);
    auto admitted = virtualDisplayObserverMessage(request, "admitted");
    admitted["pid"] = getpid();
    admitted["snapshot_path"] = snapshotPath;
    std::cout << admitted.dump() << std::endl;
    std::string line;
    if (!std::getline(std::cin, line))
        return 0;
    validateVirtualDisplayObserverMessage(parseVirtualDisplayOwnerJson(line), request, "observe", 6);
    auto observed = virtualDisplayObserverMessage(request, "observed");
    observed["pid"] = getpid();
    observed["observation"] = {{"online_complete", true}, {"online", true}, {"active", true},
        {"mirrored", false}, {"mirror_target", 0}, {"vendor", 0x4450}, {"product", 1}, {"serial", request.spec.serial},
        {"pixels_width", request.spec.width}, {"pixels_height", request.spec.height},
        {"bounds_width", request.spec.width}, {"bounds_height", request.spec.height}, {"mode_present", true},
        {"mode_width", request.spec.width}, {"mode_height", request.spec.height},
        {"mode_pixel_width", request.spec.width}, {"mode_pixel_height", request.spec.height}, {"refresh_rate", 60}};
    if (definition.at("fixture") == "observer-wrong-mode")
        observed["observation"]["mode_width"] = 1920;
    std::cout << observed.dump() << std::endl;
    return 0;
}

int fakeChild(const std::string &descriptor)
{
    const auto definition = readFile(descriptor);
    const auto profile = definition.at("profile");
    if (profile.at("operation") == "virtual-display-observe")
        return fakeObserver(definition, descriptor);
    const auto request = parseVirtualDisplayOwnerRequest(profile);
    const auto mode = definition.at("fixture").get<std::string>();
    const auto directory = descriptor.substr(0, descriptor.find_last_of('/'));
    auto admitted = virtualDisplayOwnerMessage(request, "admitted");
    admitted["pid"] = getpid();
    const auto hostTemporary = directory + "/host-tmp";
    assert(std::string(getenv("TMPDIR")) == hostTemporary);
    const auto snapshotDirectory = hostTemporary + "/qt-private";
    assert(mkdir(snapshotDirectory.c_str(), 0700) == 0);
    const auto snapshotPath = snapshotDirectory + "/native-worker-module";
    {
        std::ofstream snapshot(snapshotPath);
        snapshot << "fixture module";
    }
    assert(chmod(snapshotPath.c_str(), 0500) == 0);
    admitted["snapshot_path"] = snapshotPath;
    if (mode == "foreign-admitted")
        admitted["nonce"] = std::string(64, 'f');
    std::cout << admitted.dump() << std::endl;
    std::string line;
    if (!std::getline(std::cin, line))
        return 0;
    const auto create = parseVirtualDisplayOwnerJson(line);
    if (create.value("type", "") == "stop")
        return 0;
    validateVirtualDisplayOwnerMessage(create, request, "create", 5);
    if (mode == "changed-snapshot") {
        assert(chmod(snapshotPath.c_str(), 0600) == 0);
        std::ofstream changed(snapshotPath);
        changed << "changed fixture module";
        changed.close();
        assert(chmod(snapshotPath.c_str(), 0500) == 0);
    }
    writeFile(directory + "/display-state.json", {{"pid", getpid()}, {"serial", request.spec.serial},
        {"width", request.spec.width}, {"height", request.spec.height}});
    if (mode != "lost-ready") {
        auto ready = virtualDisplayOwnerMessage(request, "ready");
        ready["pid"] = getpid();
        ready["display"] = 42;
        ready["width"] = request.spec.width;
        ready["height"] = request.spec.height;
        ready["refresh_rate"] = 60;
        if (mode == "foreign-ready")
            ready["serial"] = request.spec.serial + 1;
        std::cout << ready.dump() << std::endl;
    }
    while (std::getline(std::cin, line)) {
        if (parseVirtualDisplayOwnerJson(line).value("type", "") == "stop")
            return 0;
    }
    return 0;
}

struct Fixture {
    std::string executable;
    std::string mode = "healthy";
    std::string directory;
    std::string observerDirectory;
    unsigned materializations = 0;
    unsigned snapshots = 0;
    unsigned modeQueries = 0;
    unsigned prematureModeQueries = 0;
    bool lastInventoryRequestedModes = false;
    bool throwMaterializer = false;
    bool forceUnknownSnapshot = false;
    bool retainAfterExit = false;
    bool modeKnown = false;
    bool reusedFallback = false;
    bool cancelled = false;
    bool cancelDuringObserver = false;
    bool driftDuringObserver = false;
    bool geometryDrifted = false;
    bool cancelAfterSnapshot = false;
    unsigned pendingModeSnapshots = 0;
    VirtualDisplayOwnerContext context()
    {
        return {executable, [this](const std::string &path, const Json &profile) {
            ++materializations;
            if (profile.at("operation") == "virtual-display-owner")
                directory = path;
            else {
                observerDirectory = path;
                if (cancelDuringObserver)
                    cancelled = true;
            }
            writeFile(path + "/child-host.json", {{"profile", profile}, {"fixture", mode}});
            if (throwMaterializer)
                throw std::runtime_error("fixture materialization failed after private file write");
            return path + "/child-host.json";
        }, [this](const std::string &path) {
            if (cancelAfterSnapshot)
                cancelled = true;
            if (driftDuringObserver && !observerDirectory.empty() && path.starts_with(observerDirectory + "/"))
                geometryDrifted = true;
            std::ifstream input(path);
            std::string bytes((std::istreambuf_iterator<char>(input)), {});
            return bytes == "fixture module";
        }};
    }
    VirtualDisplayOwnerInventory snapshot(bool includeModes = false)
    {
        ++snapshots;
        lastInventoryRequestedModes = includeModes;
        if (includeModes) {
            ++modeQueries;
            if (directory.empty() || access((directory + "/display-state.json").c_str(), F_OK) != 0) {
                ++prematureModeQueries;
                throw std::runtime_error("fixture forbids mode queries before display creation");
            }
        }
        if (forceUnknownSnapshot)
            return {};
        VirtualDisplayOwnerDisplay fallback;
        fallback.id = reusedFallback ? 42 : 1;
        fallback.vendor = 0x756e6b6e;
        fallback.product = 0x76697274;
        fallback.width = 1920;
        fallback.height = 1080;
        fallback.boundsWidth = 1920;
        fallback.boundsHeight = 1080;
        fallback.main = true;
        fallback.active = true;
        VirtualDisplayOwnerInventory result{true, {fallback}};
        if (directory.empty() || access((directory + "/display-state.json").c_str(), F_OK) != 0)
            return result;
        const auto state = readFile(directory + "/display-state.json");
        if (!retainAfterExit && kill(state.at("pid").get<int>(), 0) < 0)
            return result;
        VirtualDisplayOwnerDisplay display;
        display.id = 42;
        display.vendor = 0x4450;
        display.product = 1;
        display.serial = state.at("serial").get<std::uint32_t>();
        display.width = state.at("width").get<unsigned>();
        display.height = state.at("height").get<unsigned>();
        display.boundsWidth = geometryDrifted ? 1920 : display.width;
        display.boundsHeight = display.height;
        display.active = true;
        display.main = true;
        display.modeKnown = includeModes && modeKnown;
        if (pendingModeSnapshots) {
            --pendingModeSnapshots;
            display.active = false;
        }
        display.mode = {display.width, display.height, display.width, display.height, 60};
        result.displays = {display};
        return result;
    }
    std::shared_ptr<NativeVirtualDisplay::Platform> platform()
    {
        return makeVirtualDisplayOwnerPlatformForSystem(context(), [this](bool includeModes) { return snapshot(includeModes); },
            [this](std::uint32_t id, std::uint32_t serial) {
                const auto value = snapshot();
                for (const auto &display : value.displays) {
                    if (display.id == id && display.vendor == 0x4450 && display.product == 1 && display.serial == serial)
                        return true;
                }
                return false;
            });
    }
    ~Fixture()
    {
        if (!directory.empty()) {
            if (!observerDirectory.empty()) {
                unlink((observerDirectory + "/host-tmp/qt-private/native-worker-module").c_str());
                rmdir((observerDirectory + "/host-tmp/qt-private").c_str());
                rmdir((observerDirectory + "/host-tmp").c_str());
                unlink((observerDirectory + "/snapshot-cleanup-unknown.json").c_str());
                unlink((observerDirectory + "/child-host.json").c_str());
                rmdir(observerDirectory.c_str());
            }
            unlink((directory + "/host-tmp/qt-private/native-worker-module").c_str());
            rmdir((directory + "/host-tmp/qt-private").c_str());
            rmdir((directory + "/host-tmp").c_str());
            unlink((directory + "/snapshot-cleanup-unknown.json").c_str());
            unlink((directory + "/display-state.json").c_str());
            unlink((directory + "/child-host.json").c_str());
            rmdir(directory.c_str());
        }
    }
};

void normalAndLostReady(const std::string &self)
{
    for (const std::string mode : {"healthy", "lost-ready", "foreign-admitted", "foreign-ready"}) {
        Fixture fixture;
        fixture.executable = self;
        fixture.mode = mode;
        auto platform = fixture.platform();
        assert(fixture.materializations == 0 && fixture.snapshots == 0 && fixture.directory.empty());
        const auto created = platform->createWithAdmission({3840, 2160}, Clock::now() + 250ms, [] { return true; });
        assert(created.effectsPossible);
        assert((created.display == 42) == (mode == "healthy"));
        if (mode == "healthy")
            assert(platform->ownerAlive());
        platform->releaseBefore(Clock::now() + 800ms);
        const auto closed = platform->observe(created.display, {3840, 2160});
        assert(closed.querySucceeded && !closed.online && !platform->ownerAlive());
    }
}
void strictReadbackAndAbsence(const std::string &self)
{
    for (const bool reused : {false, true}) {
        Fixture fixture;
        fixture.executable = self;
        fixture.modeKnown = false;
        fixture.reusedFallback = reused;
        if (!reused)
            fixture.mode = "observer-wrong-mode";
        auto platform = fixture.platform();
        const auto created = platform->createWithAdmission({3840, 2160}, Clock::now() + 300ms, [] { return true; });
        assert(created.effectsPossible && !created.display); // NULL mode or reused baseline ID cannot admit.
        platform->releaseBefore(Clock::now() + 800ms);
        const auto closed = platform->observe(0, {3840, 2160});
        assert(closed.online == reused); // A known reused ID cannot be declared absent.

    }
    Fixture fixture;
    fixture.executable = self;
    auto platform = fixture.platform();
    const auto created = platform->createWithAdmission({3840, 2160}, Clock::now() + 300ms, [] { return true; });
    assert(created.display == 42);
    fixture.retainAfterExit = true;
    platform->releaseBefore(Clock::now() + 800ms);
    assert(platform->observe(42, {3840, 2160}).online); // Reaping alone cannot prove display absence.
    fixture.retainAfterExit = false;
    fixture.forceUnknownSnapshot = true;
    assert(!platform->observe(42, {3840, 2160}).querySucceeded);
}
void ownerCrashAndSharedDeadline(const std::string &self)
{
    Fixture fixture;
    fixture.executable = self;
    auto platform = fixture.platform();
    const auto created = platform->createWithAdmission({3840, 2160}, Clock::now() + 300ms, [] { return true; });
    assert(created.display == 42);
    std::this_thread::sleep_for(350ms);
    assert(platform->ownerAlive()); // Creation expiry never ends a healthy owner.
    fixture.retainAfterExit = true;
    const int child = readFile(fixture.directory + "/display-state.json").at("pid").get<int>();
    assert(kill(child, SIGKILL) == 0); // Only this fixture's exact owned child.
    const auto observeBy = Clock::now() + 1s;
    while (platform->ownerAlive() && Clock::now() < observeBy)
        std::this_thread::sleep_for(1ms);
    assert(!platform->ownerAlive()); // Public geometry alone cannot hide child death.
    platform->releaseBefore(Clock::now() + 500ms);
    assert(platform->observe(42, {3840, 2160}).online);
    fixture.retainAfterExit = false;
    assert(!platform->observe(42, {3840, 2160}).online);

    Fixture expiry;
    expiry.executable = self;
    auto bounded = expiry.platform();
    assert(bounded->createWithAdmission({3840, 2160}, Clock::now() + 300ms, [] { return true; }).display == 42);
    const auto before = Clock::now();
    bounded->releaseBefore(before + 400ms);
    assert(Clock::now() - before < 450ms);
    assert(!bounded->observe(42, {3840, 2160}).online);
}


void delayedReadinessAndCancellation(const std::string &self)
{
    Fixture delayed;
    delayed.executable = self;
    delayed.pendingModeSnapshots = 5;
    auto platform = delayed.platform();
    const auto created = platform->createWithAdmission({3840, 2160}, Clock::now() + 500ms, [] { return true; });
    assert(created.display == 42 && delayed.pendingModeSnapshots == 0);
    platform->releaseBefore(Clock::now() + 800ms);
    assert(!platform->observe(42, {3840, 2160}).online);
    assert(access((delayed.directory + "/host-tmp").c_str(), F_OK) != 0);

    Fixture denied;
    denied.executable = self;
    denied.cancelAfterSnapshot = true;
    auto cancelled = denied.platform();
    const auto rejected = cancelled->createWithAdmission({3840, 2160}, Clock::now() + 500ms,
        [&denied] { return !denied.cancelled; });
    assert(rejected.effectsPossible && !rejected.display);
    assert(access((denied.directory + "/display-state.json").c_str(), F_OK) != 0);
    cancelled->releaseBefore(Clock::now() + 800ms);
    assert(!cancelled->observe(0, {3840, 2160}).online);
    assert(access((denied.directory + "/host-tmp").c_str(), F_OK) != 0);
}


void inertFileFailurePreservesResourceTruth(const std::string &self)
{
    Fixture fixture;
    fixture.executable = self;
    fixture.mode = "changed-snapshot";
    auto platform = fixture.platform();
    const auto created = platform->createWithAdmission({3840, 2160}, Clock::now() + 500ms, [] { return true; });
    assert(created.display == 42);
    platform->releaseBefore(Clock::now() + 800ms);
    const auto closed = platform->observe(42, {3840, 2160});
    assert(closed.querySucceeded && !closed.online);
    assert(access((fixture.directory + "/host-tmp/qt-private/native-worker-module").c_str(), F_OK) == 0);
    assert(access((fixture.directory + "/snapshot-cleanup-unknown.json").c_str(), F_OK) == 0);
}


void baselineAndCleanupNeverQueryModes(const std::string &self)
{
    Fixture fixture;
    fixture.executable = self;
    auto platform = fixture.platform();
    const auto created = platform->createWithAdmission({3840, 2160}, Clock::now() + 500ms, [] { return true; });
    assert(created.display == 42);
    assert(fixture.prematureModeQueries == 0 && fixture.modeQueries == 0);
    const auto admittedModeQueries = fixture.modeQueries;
    platform->releaseBefore(Clock::now() + 800ms);
    const auto stopped = platform->observe(created.display, {3840, 2160});
    assert(stopped.querySucceeded && !stopped.online);
    assert(!fixture.lastInventoryRequestedModes && fixture.modeQueries == admittedModeQueries);

    Fixture rejected;
    rejected.executable = self;
    rejected.throwMaterializer = true;
    auto noChild = rejected.platform();
    assert(!noChild->createWithAdmission({3840, 2160}, Clock::now() + 500ms, [] { return true; }).effectsPossible);
    assert(rejected.snapshots == 1 && rejected.modeQueries == 0 && !rejected.lastInventoryRequestedModes);
}


void observerCannotOverrideCancellationOrCurrentGeometry(const std::string &self)
{
    for (const bool cancel : {false, true}) {
        Fixture fixture;
        fixture.executable = self;
        fixture.cancelDuringObserver = cancel;
        fixture.driftDuringObserver = !cancel;
        auto platform = fixture.platform();
        const auto started = platform->createWithAdmission({3840, 2160}, Clock::now() + 600ms,
            [&fixture] { return !fixture.cancelled; });
        assert(started.effectsPossible && !started.display);
        assert(!platform->ownerAlive());
        platform->releaseBefore(Clock::now() + 800ms);
        const auto removed = platform->observe(0, {3840, 2160});
        assert(removed.querySucceeded && !removed.online);
    }
}

void noEffectFailures(const std::string &self)
{
    Fixture fixture;
    fixture.executable = self;
    fixture.throwMaterializer = true;
    auto platform = fixture.platform();
    const auto failed = platform->createWithAdmission({3840, 2160}, Clock::now() + 300ms, [] { return true; });
    assert(!failed.effectsPossible && !fixture.directory.empty());
    assert(access(fixture.directory.c_str(), F_OK) != 0);
    Fixture denied;
    denied.executable = self;
    auto cancelled = denied.platform();
    assert(!cancelled->createWithAdmission({3840, 2160}, Clock::now() + 300ms, [] { return false; }).effectsPossible);
    assert(denied.materializations == 0 && denied.snapshots == 0);
    Fixture spawn;
    spawn.executable = "/nonexistent/dockpipe-native-owner-fixture";
    auto missing = spawn.platform();
    assert(missing->createWithAdmission({3840, 2160}, Clock::now() + 300ms, [] { return true; }).effectsPossible);
    assert(access(spawn.directory.c_str(), F_OK) != 0);
    missing->releaseBefore(Clock::now() + 100ms);
    assert(!missing->observe(0, {3840, 2160}).querySucceeded); // Unknown launch fate is never invented clean.
}
}

int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--native-worker-host" && std::string(argv[2]) == "--request")
        return fakeChild(argv[3]);
    assert(argc == 1);
    char executable[4096];
    assert(realpath(argv[0], executable));
    normalAndLostReady(executable);
    strictReadbackAndAbsence(executable);
    noEffectFailures(executable);
    ownerCrashAndSharedDeadline(executable);
    delayedReadinessAndCancellation(executable);
    inertFileFailurePreservesResourceTruth(executable);
    baselineAndCleanupNeverQueryModes(executable);
    observerCannotOverrideCancellationOrCurrentGeometry(executable);
}
