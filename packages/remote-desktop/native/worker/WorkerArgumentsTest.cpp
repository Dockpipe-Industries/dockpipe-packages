#include "WorkerArguments.h"
#include <cassert>
#include <iostream>
using namespace dockpipe::desktop;
template<typename Action> bool rejected(Action action)
{
    try {
        action();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}
int main()
{
    assert(parseWorkerArguments({"--permissions", "screen"}).permission == PermissionMode::Screen);
    assert(parseWorkerArguments({"--permissions", "input"}).permission == PermissionMode::Input);
    assert(parseWorkerArguments({"--permissions", "status"}).permission == PermissionMode::Status);
    assert(rejected([] { parseWorkerArguments({}); }));
    assert(rejected([] { parseWorkerArguments({"--permissions", "all"}); }));
    assert(rejected([] { parseWorkerArguments({"--permissions", "screen", "--state", "/state"}); }));
    assert(rejected([] { parseWorkerArguments({"--permissions"}); }));
    const auto worker = parseWorkerArguments({"--cli", "/cli", "--state", "/state", "--channel-helper", "/helper", "--manifest", "/manifest"});
    assert(worker.permission == PermissionMode::None && worker.paths.size() == 4);
    assert(rejected([] { parseWorkerArguments({"--cli", "cli", "--state", "/state", "--channel-helper", "/helper", "--manifest", "/manifest"}); }));
    assert(rejected([] { parseWorkerArguments({"--cli", "/cli", "--state", "/state", "--state", "/helper", "--manifest", "/manifest"}); }));
    std::cout << "PASS explicit permission operations, no mixed worker mode, trusted path arguments, malformed/duplicate rejection\n";
}
