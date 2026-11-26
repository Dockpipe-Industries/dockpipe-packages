#pragma once

#include "WorkerArguments.h"
#include "WorkerDiagnostics.h"
#include "PowerPolicy.h"
#include "WorkerHostTemplate.h"
#include <optional>
#include <atomic>
#include <memory>
#include <functional>
#include <string>

namespace dockpipe::desktop {
// The package instance retains this state when cleanup cannot be verified.
// A stop request never upgrades unknown native ownership to a safe unload.
struct WorkerRuntime final {
    std::shared_ptr<std::atomic_bool> stopRequested = std::make_shared<std::atomic_bool>(false);
    std::atomic_bool cleanupVerified{true};
    std::function<bool()> stopSource;
    std::shared_ptr<void> retainedResources;
    std::shared_ptr<WorkerDiagnostics> diagnostics = std::make_shared<WorkerDiagnostics>();

    bool cancelled()
    {
        if (stopSource && stopSource())
            *stopRequested = true;
        return *stopRequested;
    }
};

int runPermissionMode(PermissionMode mode);
int runPowerAgent(const std::string &cli, const std::string &state, const std::string &helper,
    const std::string &manifestPath, const std::string &providerImage, const Json &desktopProvider,
    const std::string &desktopServiceLabel, WorkerRuntime &runtime);

int runWorker(const std::string &cli, const std::string &state, const std::string &helper,
    const std::string &manifestPath, const std::string &providerImage, WorkerRuntime &runtime,
    std::optional<VirtualDisplayHostTemplateReference> virtualDisplayHostTemplate = {});
} // namespace dockpipe::desktop
