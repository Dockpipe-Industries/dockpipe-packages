#include "WorkerCleanup.h"
#include <cassert>
#include <iostream>

using namespace dockpipe::desktop;
using namespace std::chrono_literals;

namespace {
Json reference()
{
    return Json{{"schema", "dockpipe.remote.desktop/v1"}, {"id", std::string(64, 'a')}, {"epoch", std::string(64, 'b')},
        {"target", Json{{"node", "fixture"}, {"fingerprint", std::string(64, 'c')}, {"generation", std::string(64, 'd')}}}};
}
}

int main()
{
    const auto start = Clock::time_point{};
    auto now = start;
    const auto deadline = start + 5500ms;
    StopCompletion completion(deadline);
    int pumps = 0;
    const bool verified = awaitStopCompletion(completion, deadline, [&] { return now; }, [&](auto until) {
        assert(until > now && until - now <= 5ms);
        now = until;
        if (++pumps == 3)
            completion.complete(true, now);
    });
    assert(verified && pumps == 3); // Only this request's callback establishes completion.

    StopCompletion noCallback(deadline);
    now = start;
    assert(!awaitStopCompletion(noCallback, deadline, [&] { return now; }, [&](auto until) { now = until; }));
    assert(now == deadline);
    noCallback.complete(true, deadline + 1ms);
    assert(!noCallback.verified()); // Timeout cannot become late success.

    StopCompletion lateCallback(deadline);
    now = start;
    assert(!awaitStopCompletion(lateCallback, deadline, [&] { return now; }, [&](auto) {
        now = deadline + 1ms;
        lateCallback.complete(true, now);
    }));

    StopCompletion watchdog(deadline);
    watchdog.complete(false, start + 5s);
    watchdog.complete(true, start + 5100ms);
    assert(!watchdog.verified()); // An earlier unknown result is terminal.

    StopCompletion independent(deadline);
    assert(independent.pending()); // Another request cannot inherit a prior global notification.
    independent.complete(true, start + 20ms);
    assert(independent.verified());

    const auto original = reference();
    Json forget{{"type", "forget"}, {"reference", original}};
    assert(exactCleanupForget(forget, original, true));
    assert(!exactCleanupForget(forget, original, false));
    auto foreign = forget;
    foreign["reference"]["target"]["generation"] = std::string(64, 'f');
    assert(!exactCleanupForget(foreign, original, true));
    foreign = forget;
    foreign["reference"]["epoch"] = std::string(64, 'f');
    assert(!exactCleanupForget(foreign, original, true));
    foreign = forget;
    foreign["extra"] = true;
    assert(!exactCleanupForget(foreign, original, true));
    assert(!exactCleanupForget(Json{{"type", "directive"}, {"reference", original}}, original, true));
    assert(!exactCleanupForget(Json{{"type", 1}, {"reference", original}}, original, true));

    const Json open{{"reference", original}, {"mode", "view"}};
    Json queued{{"type", "directive"}, {"directive", Json{{"open", open}, {"lease_millis", 10000}, {"signals", Json::array()}}}};
    assert(queuedOriginalDirective(queued, open));
    queued["directive"]["open"]["mode"] = "control";
    assert(!queuedOriginalDirective(queued, open));

    for (bool nativeSuccess : {true, false}) {
        std::promise<void> childStarted;
        auto started = childStarted.get_future();
        std::promise<void> releaseChild;
        auto release = releaseChild.get_future();
        std::atomic_bool joined{false};
        const bool stopped = finishCleanupConcurrently([&] {
            assert(started.wait_for(1s) == std::future_status::ready);
            releaseChild.set_value();
            return nativeSuccess;
        }, [&] {
            childStarted.set_value();
            release.wait();
            joined = true;
            return true;
        });
        assert(stopped == nativeSuccess && joined);
    }
    std::promise<void> childStarted;
    auto started = childStarted.get_future();
    std::promise<void> releaseChild;
    auto release = releaseChild.get_future();
    std::atomic_bool joined{false};
    bool threw = false;
    try {
        finishCleanupConcurrently([&]() -> bool {
            assert(started.wait_for(1s) == std::future_status::ready);
            releaseChild.set_value();
            throw std::runtime_error("fixture native callback failure");
        }, [&] {
            childStarted.set_value();
            release.wait();
            joined = true;
            return true;
        });
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw && joined); // Exception cannot leave a stop owner running.
    std::cout << "PASS request-local stop, callback pumping, deadline, sticky unknown, exact Forget and concurrent exception-safe child join\n";
}
