#include <DockpipeNativeWorkerPackage.h>
#include <atomic>
#include <cassert>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <thread>

namespace {
int cancelled(void *context)
{
    ++*static_cast<std::atomic_uint *>(context);
    return 1;
}

DockpipeNativeWorkerResultV1 invoke(DockpipeNativeWorkerEntryV1 entry, const DockpipeNativeWorkerHostV1 &host)
{
    DockpipeNativeWorkerResultV1 result{sizeof(DockpipeNativeWorkerResultV1),
        DOCKPIPE_NATIVE_WORKER_FAILURE_V1, 0};
    entry(&host, &result);
    return result;
}
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        std::cerr << "Native worker module could not load.\n";
        return 1;
    }
    const auto entry = reinterpret_cast<DockpipeNativeWorkerEntryV1>(dlsym(module, DOCKPIPE_NATIVE_WORKER_ENTRY_V1));
    assert(entry);
    Dl_info image{};
    assert(dladdr(reinterpret_cast<const void *>(entry), &image) && image.dli_fname && image.dli_fbase);
    // The entrypoint belongs to this module image, never the test/host executable.
    assert(std::strcmp(image.dli_fname, argv[1]) == 0);
    const char profile[] = "{\"schema\":\"dockpipe.desktop.native.host/v1\",\"operation\":\"worker\","
        "\"channel_helper\":\"/not/executed/helper\",\"manifest\":\"/not/read/manifest\"}";
    std::atomic_uint polls{0};
    DockpipeNativeWorkerHostV1 host{1, sizeof(DockpipeNativeWorkerHostV1), "/not/executed/cli", "/not/read/state",
        profile, sizeof(profile) - 1, &polls, cancelled};
    auto wrong = host;
    wrong.abi_version = 2;
    auto result = invoke(entry, wrong);
    assert(result.outcome == DOCKPIPE_NATIVE_WORKER_FAILURE_V1 && result.cleanup_verified == 0 && polls == 0);
    wrong = host;
    wrong.profile_json = "{}";
    wrong.profile_json_bytes = 2;
    result = invoke(entry, wrong);
    assert(result.outcome == DOCKPIPE_NATIVE_WORKER_FAILURE_V1 && result.cleanup_verified == 0 && polls == 0);
    std::thread foreign([&] {
        const auto rejected = invoke(entry, host);
        assert(rejected.outcome == DOCKPIPE_NATIVE_WORKER_FAILURE_V1 && rejected.cleanup_verified == 0);
    });
    foreign.join();
    assert(polls == 0);
    struct sigaction before{};
    assert(sigaction(SIGPIPE, nullptr, &before) == 0);
    result = invoke(entry, host);
    assert(result.outcome == DOCKPIPE_NATIVE_WORKER_CANCELLED_V1 && result.cleanup_verified == 1 && polls == 1);
    struct sigaction after{};
    assert(sigaction(SIGPIPE, nullptr, &after) == 0 && before.sa_handler == after.sa_handler);
    result = invoke(entry, host);
    assert(result.outcome == DOCKPIPE_NATIVE_WORKER_FAILURE_V1 && result.cleanup_verified == 0 && polls == 1);
    // No unload: the ABI retains its image and instance for process lifetime.
    std::cout << "PASS module load/image binding, ABI/profile/thread rejection, pre-cancel and one-shot entry; no OS permission/capture/input API\n";
}
