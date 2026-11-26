#pragma once

#include "NativeDesktopBackend.h"

namespace dockpipe::desktop {
struct WorkerReadiness {
    bool view = false;
    bool input = false;

    bool operator==(const WorkerReadiness &) const = default;
};

enum class ReadinessAction { Continue, RefreshIdle, CloseOwnedSession };

inline WorkerReadiness workerReadiness(bool ownerView, bool ownerInput, NativePermissionStatus permissions)
{
    const bool view = ownerView && permissions.screenCapture;
    return {view, view && ownerInput && permissions.accessibility && permissions.eventPosting};
}

inline ReadinessAction readinessAction(WorkerReadiness advertised, WorkerReadiness current, bool sessionOwned)
{
    if (advertised == current)
        return ReadinessAction::Continue;
    return sessionOwned ? ReadinessAction::CloseOwnedSession : ReadinessAction::RefreshIdle;
}
} // namespace dockpipe::desktop
