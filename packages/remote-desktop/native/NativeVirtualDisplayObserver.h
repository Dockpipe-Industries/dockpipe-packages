#pragma once

#include "NativeVirtualDisplayOwner.h"
#include "NativeVirtualMode.h"
#include <memory>
#include <optional>

namespace dockpipe::desktop {
struct VirtualDisplayObserverRequest {
    NativeVirtualDisplaySpec spec;
    std::uint32_t display = 0;
    std::string nonce;
    int parentProcess = 0;
    std::int64_t deadlineUnixMilliseconds = 0;
};

VirtualDisplayObserverRequest parseVirtualDisplayObserverRequest(const nlohmann::json &profile);
nlohmann::json virtualDisplayObserverProfile(const VirtualDisplayObserverRequest &request);
nlohmann::json virtualDisplayObserverMessage(const VirtualDisplayObserverRequest &request, const char *type);
void validateVirtualDisplayObserverMessage(const nlohmann::json &message,
    const VirtualDisplayObserverRequest &request, const char *type, std::size_t fields);
NativeVirtualMode parseVirtualDisplayObservation(const nlohmann::json &message,
    const VirtualDisplayObserverRequest &request, int observerProcess);
NativeVirtualDisplayResult runVirtualDisplayObserverChild(const VirtualDisplayObserverRequest &request,
    const std::function<bool()> &stopRequested, const std::string &loadedModulePath);

// Fresh-process mode evidence stays separate from the parent's cached mode.
// No process/filesystem effect occurs in construction. A positive observation
// includes natural exit0, exact reap and private snapshot cleanup.
class VirtualDisplayModeObserver final {
public:
    explicit VirtualDisplayModeObserver(VirtualDisplayOwnerContext context);
    ~VirtualDisplayModeObserver();
    VirtualDisplayModeObserver(const VirtualDisplayModeObserver &) = delete;
    VirtualDisplayModeObserver &operator=(const VirtualDisplayModeObserver &) = delete;
    bool observe(const VirtualDisplayObserverRequest &request, NativeVirtualDisplay::Clock::time_point deadline,
        const NativeVirtualDisplay::SelectionAdmission &admission);
    bool close(NativeVirtualDisplay::Clock::time_point deadline);
    const std::optional<NativeVirtualMode> &mode() const;
private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace dockpipe::desktop
