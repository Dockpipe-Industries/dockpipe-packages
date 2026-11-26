#include "WorkerReadiness.h"
#include <cassert>
#include <iostream>

using namespace dockpipe::desktop;

int main()
{
    const NativePermissionStatus denied{false, false, false};
    const NativePermissionStatus screenOnly{true, false, false};
    const NativePermissionStatus all{true, true, true};
    const auto unavailable = workerReadiness(true, true, denied);
    assert(!unavailable.view && !unavailable.input);
    for (unsigned poll = 0; poll != 1000; ++poll)
        assert(readinessAction(unavailable, workerReadiness(true, true, denied), false) == ReadinessAction::Continue);

    const auto view = workerReadiness(true, true, screenOnly);
    const auto control = workerReadiness(true, true, all);
    assert(view.view && !view.input && control.view && control.input);
    assert(readinessAction(unavailable, view, false) == ReadinessAction::RefreshIdle);
    assert(readinessAction(view, unavailable, false) == ReadinessAction::RefreshIdle);
    assert(readinessAction(view, control, false) == ReadinessAction::RefreshIdle);
    assert(readinessAction(control, view, false) == ReadinessAction::RefreshIdle);
    for (const auto advertised : {unavailable, view, control}) {
        for (const auto current : {unavailable, view, control}) {
            const auto action = readinessAction(advertised, current, true);
            assert(action == (advertised == current ? ReadinessAction::Continue : ReadinessAction::CloseOwnedSession));
        }
    }
    // OS capabilities cannot widen independent owner policy. Irrelevant input
    // permission changes do not churn a view-only catalog.
    assert(workerReadiness(false, true, all) == unavailable);
    const auto ownerViewOnly = workerReadiness(true, false, screenOnly);
    assert(ownerViewOnly == workerReadiness(true, false, all));
    assert(readinessAction(ownerViewOnly, workerReadiness(true, false, all), false) == ReadinessAction::Continue);
    assert(readinessAction(control, workerReadiness(true, true, {true, true, false}), true)
        == ReadinessAction::CloseOwnedSession);
    assert(readinessAction(control, workerReadiness(true, true, {true, false, true}), true)
        == ReadinessAction::CloseOwnedSession);
    std::cout << "PASS denied idle stability, clean catalog refresh, active gain/loss close, and independent owner bounds\n";
}
