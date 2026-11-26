#include "WorkerPackageContract.h"
#include "WorkerRuntime.h"
#include "ChildProcess.h"
#include "WorkerApplication.h"
#include "../NativeVirtualDisplayOwner.h"
#include "../NativeVirtualDisplayObserver.h"
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>
#include <unistd.h>

extern "C" __attribute__((visibility("default")))
void dockpipe_native_worker_package_v1(const DockpipeNativeWorkerHostV1 *host,
    DockpipeNativeWorkerResultV1 *result);

namespace {
std::atomic_bool entered{false};
// Retained until this dedicated host process exits, including unknown cleanup.
// The host must never unload this image or invoke it a second time.
dockpipe::desktop::WorkerRuntime *retainedRuntime = nullptr;

std::string loadedProviderImage()
{
    Dl_info image{};
    dockpipe::desktop::require(dladdr(reinterpret_cast<const void *>(&dockpipe_native_worker_package_v1), &image)
        && image.dli_fname && image.dli_fbase, "Cannot identify the loaded native provider image.");
    char resolved[4096];
    dockpipe::desktop::require(realpath(image.dli_fname, resolved), "Cannot resolve the loaded native provider image.");
    return resolved;
}
}

extern "C" __attribute__((visibility("default")))
void dockpipe_native_worker_package_v1(const DockpipeNativeWorkerHostV1 *host,
    DockpipeNativeWorkerResultV1 *result)
{
    if (!result || result->struct_size != sizeof(*result))
        return;
    result->outcome = DOCKPIPE_NATIVE_WORKER_FAILURE_V1;
    result->cleanup_verified = 0;
    dockpipe::desktop::WorkerRuntime *ownedRuntime = nullptr;
    dockpipe::desktop::WorkerDiagnostics boundaryDiagnostics;
    auto *diagnostics = &boundaryDiagnostics;
    @autoreleasepool {
        try {
            dockpipe::desktop::require(host && [NSThread isMainThread], "Native worker requires the host main thread.");
            diagnostics->stage(dockpipe::desktop::WorkerStage::Profile);
            const auto profile = dockpipe::desktop::parseWorkerPackageProfile(*host);
            dockpipe::desktop::require(!entered.exchange(true), "Native worker host mode may run only once.");
            diagnostics->stage(dockpipe::desktop::WorkerStage::Instance);
            retainedRuntime = new dockpipe::desktop::WorkerRuntime;
            ownedRuntime = retainedRuntime;
            auto &runtime = *ownedRuntime;
            diagnostics = runtime.diagnostics.get();
            runtime.stopSource = [host] { return host->stop_requested(host->host_context) != 0; };
            if (runtime.cancelled()) {
                result->outcome = DOCKPIPE_NATIVE_WORKER_CANCELLED_V1;
                result->cleanup_verified = 1;
                runtime.stopSource = {};
                return;
            }
            if (profile.permission != dockpipe::desktop::PermissionMode::None) {
                diagnostics->stage(dockpipe::desktop::WorkerStage::Permissions);
                dockpipe::desktop::runPermissionMode(profile.permission);
            } else if (profile.operation == dockpipe::desktop::WorkerOperation::PowerSettings) {
                dockpipe::desktop::runPowerSettings(host->state_directory);
            } else if (profile.operation == dockpipe::desktop::WorkerOperation::ConfigurePower) {
                dockpipe::desktop::configurePowerPolicy(host->state_directory, profile.powerPolicy);
            } else if (profile.operation == dockpipe::desktop::WorkerOperation::VirtualDisplayObserver) {
                // This role performs only synchronous public display queries.
                // No AppKit initialization, native creation, child or bridge.
                // Query failure is an operation failure with known no-effects cleanup.
                runtime.cleanupVerified = true;
                const auto observation = dockpipe::desktop::runVirtualDisplayObserverChild(*profile.virtualDisplayObserverRequest,
                    [&runtime] { return runtime.cancelled(); }, loadedProviderImage());
                dockpipe::desktop::require(observation.success, "Fresh virtual display observation was not verified.");
            } else if (profile.operation == dockpipe::desktop::WorkerOperation::VirtualDisplayOwner) {
                runtime.cleanupVerified = false;
                const auto child = dockpipe::desktop::runVirtualDisplayOwnerChild(*profile.virtualDisplayOwnerRequest,
                    [&runtime] { return runtime.cancelled(); }, loadedProviderImage());
                runtime.cleanupVerified = child.cleanupVerified;
                dockpipe::desktop::require(child.success, "Dedicated virtual display owner did not complete successfully.");
            } else if (profile.operation == dockpipe::desktop::WorkerOperation::PowerAgent) {
                diagnostics->stage(dockpipe::desktop::WorkerStage::ModuleImage);
                dockpipe::desktop::runPowerAgent(host->cli_path, host->state_directory, profile.helper,
                    profile.manifest, loadedProviderImage(), profile.desktopProvider,
                    profile.desktopServiceLabel, runtime);
            } else {
                diagnostics->stage(dockpipe::desktop::WorkerStage::ModuleImage);
                dockpipe::desktop::runWorker(host->cli_path, host->state_directory, profile.helper,
                    profile.manifest, loadedProviderImage(), runtime, profile.virtualDisplayHostTemplate);
            }
            result->outcome = runtime.cancelled() ? DOCKPIPE_NATIVE_WORKER_CANCELLED_V1 : DOCKPIPE_NATIVE_WORKER_SUCCESS_V1;
        } catch (const std::exception &error) {
            diagnostics->failure();
            if (diagnostics->unexpected()) {
                try {
                    diagnostics->unexpectedHash(dockpipe::desktop::diagnosticMessageHash(error.what()));
                } catch (...) {
                    // The optional digest cannot prevent cleanup result delivery.
                }
            }
        } catch (...) {
            diagnostics->failure();
        }
        if (ownedRuntime) {
            if (*ownedRuntime->stopRequested)
                result->outcome = DOCKPIPE_NATIVE_WORKER_CANCELLED_V1;
            result->cleanup_verified = ownedRuntime->cleanupVerified ? 1 : 0;
            ownedRuntime->stopSource = {}; // Host pointers are borrowed only until return.
            if (result->cleanup_verified)
                ownedRuntime->retainedResources.reset();
        }
        if (result->outcome == DOCKPIPE_NATIVE_WORKER_FAILURE_V1 || !result->cleanup_verified) {
            try {
                const auto line = std::string("Native worker diagnostic ")
                    + diagnostics->snapshot(result->cleanup_verified == 1).dump() + "\n";
                if (line.size() <= 2048)
                    dockpipe::desktop::writeBytes(STDERR_FILENO,
                        reinterpret_cast<const std::uint8_t *>(line.data()), line.size(),
                        dockpipe::desktop::Clock::now() + std::chrono::milliseconds(250));
            } catch (...) {
                // Diagnostic I/O cannot change the operation/cleanup result.
            }
        }
    }
}
