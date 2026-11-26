#include "NativeVirtualDisplay.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>

using namespace dockpipe::desktop;
using namespace std::chrono_literals;

namespace {
class Gate {
public:
    void arriveAndWait()
    {
        std::unique_lock lock(m_mutex);
        m_arrived = true;
        m_condition.notify_all();
        assert(m_condition.wait_for(lock, 3s, [&] { return m_open; }));
    }
    void waitForArrival()
    {
        std::unique_lock lock(m_mutex);
        assert(m_condition.wait_for(lock, 3s, [&] { return m_arrived; }));
    }
    void open()
    {
        std::lock_guard lock(m_mutex);
        m_open = true;
        m_condition.notify_all();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_arrived = false;
    bool m_open = false;
};

class FakePlatform final : public NativeVirtualDisplay::Platform {
public:
    bool available() override
    {
        return supported;
    }
    NativeVirtualDisplay::Creation create(NativeVirtualDisplaySpec spec) override
    {
        ++creations;
        width = spec.width;
        height = spec.height;
        return {effects, effects ? returnedID : 0U};
    }
    NativeVirtualDisplay::Observation observe(std::uint32_t display, NativeVirtualDisplaySpec spec) override
    {
        assert(display == returnedID);
        assert(spec.width == width && spec.height == height);
        if (blockObservation.exchange(false) || (releases && blockRemovalObservation.exchange(false)))
            observation.arriveAndWait();
        ++observations;
        return {querySucceeded, online, matches && !(modeAttempted && !selectionSucceeds)};
    }
    NativeVirtualDisplay::Observation configureAndObserve(std::uint32_t display, NativeVirtualDisplaySpec spec,
        const NativeVirtualDisplay::SelectionAdmission &admission) override
    {
        const auto current = observe(display, spec);
        if (!selectionRequired || current.matches || modeAttempted)
            return current;
        if (blockSelection)
            selecting.arriveAndWait();
        const bool permitted = admission && admission();
        ++modeAdmissionChecks;
        if (permitted) {
            modeAttempted = true;
            ++modeSelections;
            matches = true; // Even a failing setter may have partial effects.
        }
        return observe(display, spec);
    }
    void release() override
    {
        ++releases;
        if (blockRelease)
            releasing.arriveAndWait();
        if (!persistent)
            online = false;
    }

    std::uint32_t returnedID = 42;
    bool supported = true;
    bool effects = true;
    bool blockRelease = false;
    bool persistent = false;
    bool selectionRequired = false;
    bool selectionSucceeds = true;
    bool blockSelection = false;
    bool modeAttempted = false;
    std::atomic_uint modeSelections{0};
    std::atomic_uint modeAdmissionChecks{0};
    std::atomic_bool querySucceeded{true};
    std::atomic_bool online{true};
    std::atomic_bool matches{true};
    std::atomic_bool blockObservation{false};
    std::atomic_bool blockRemovalObservation{false};
    std::atomic_uint creations{0};
    std::atomic_uint releases{0};
    std::atomic_uint observations{0};
    unsigned width = 0;
    unsigned height = 0;
    Gate observation;
    Gate releasing;
    Gate selecting;
};

struct Reply {
    std::promise<NativeVirtualDisplayResult> promise;
    std::future<NativeVirtualDisplayResult> future = promise.get_future();
    NativeVirtualDisplay::Completion completion()
    {
        return [this](NativeVirtualDisplayResult result) { promise.set_value(std::move(result)); };
    }
    NativeVirtualDisplayResult get()
    {
        assert(future.wait_for(3s) == std::future_status::ready);
        return future.get();
    }
};

NativeVirtualDisplayResult create(NativeVirtualDisplay &display, NativeVirtualDisplaySpec spec = {1920, 1080})
{
    Reply reply;
    display.create(spec, NativeVirtualDisplay::Clock::now() + 1s, reply.completion());
    return reply.get();
}
NativeVirtualDisplayResult close(NativeVirtualDisplay &display)
{
    Reply reply;
    display.close(NativeVirtualDisplay::Clock::now() + 1s, reply.completion());
    return reply.get();
}

void unavailableAndInvalidHaveNoEffects()
{
    NativeVirtualDisplay missing(nullptr);
    const auto unavailable = create(missing);
    assert(!unavailable.success && unavailable.cleanupVerified);
    assert(close(missing).cleanupVerified);

    auto platform = std::make_shared<FakePlatform>();
    platform->supported = false;
    NativeVirtualDisplay unsupported(platform);
    assert(create(unsupported).cleanupVerified);
    assert(platform->creations == 0);
    assert(close(unsupported).cleanupVerified);

    auto untouched = std::make_shared<FakePlatform>();
    NativeVirtualDisplay invalid(untouched);
    const auto bad = create(invalid, {999, 999});
    assert(!bad.success && bad.cleanupVerified && untouched->creations == 0);
    assert(close(invalid).cleanupVerified);
}

void presetsAndSingleOwnership()
{
    for (const auto spec : {NativeVirtualDisplaySpec{1280, 720}, {1600, 900}, {1920, 1080}, {3840, 2160}}) {
        auto platform = std::make_shared<FakePlatform>();
        NativeVirtualDisplay display(platform);
        assert(!display.ownerAlive());
        const auto result = create(display, spec);
        assert(display.ownerAlive());
        assert(result.success && !result.cleanupVerified && result.display == 42);
        const auto reused = create(display, spec);
        assert(!reused.success && !reused.cleanupVerified);
        auto competingPlatform = std::make_shared<FakePlatform>();
        NativeVirtualDisplay competing(competingPlatform);
        const auto competingResult = create(competing, spec);
        assert(!competingResult.success && competingResult.cleanupVerified && competingPlatform->creations == 0);
        const auto stopped = close(display);
        assert(stopped.success && stopped.cleanupVerified && platform->releases == 1);
        assert(!display.ownerAlive());
        assert(platform->observations >= 3); // Creation plus two independent absence reads.
    }
}


void lostReadyStillRequiresOwnedScopeAbsence()
{
    auto platform = std::make_shared<FakePlatform>();
    platform->returnedID = 0;
    NativeVirtualDisplay display(platform);
    const auto started = create(display);
    assert(!started.success && !started.cleanupVerified && !display.ownerAlive());
    assert(close(display).cleanupVerified);
    assert(platform->releases == 1 && platform->observations >= 2);
}

void cancellationWinsBlockedSuccessfulObservation()
{
    auto platform = std::make_shared<FakePlatform>();
    platform->blockObservation = true;
    NativeVirtualDisplay display(platform);
    Reply created;
    display.create({1920, 1080}, NativeVirtualDisplay::Clock::now() + 1s, created.completion());
    platform->observation.waitForArrival();
    Reply stopped;
    display.close(NativeVirtualDisplay::Clock::now() + 1s, stopped.completion());
    platform->observation.open();
    assert(!created.get().success);
    assert(stopped.get().cleanupVerified);
}

void deadlineWinsBlockedSuccessfulObservation()
{
    auto platform = std::make_shared<FakePlatform>();
    platform->blockObservation = true;
    NativeVirtualDisplay display(platform);
    Reply created;
    display.create({1920, 1080}, NativeVirtualDisplay::Clock::now() + 500ms, created.completion());
    platform->observation.waitForArrival();
    const auto expired = created.get(); // Deadline expires while the OS observation is blocked.
    assert(!expired.success && !expired.cleanupVerified);
    platform->observation.open();
    assert(close(display).cleanupVerified);
}

void pendingCloseDoesNotLeakRetainedOwner()
{
    auto platform = std::make_shared<FakePlatform>();
    platform->blockRelease = true;
    const std::weak_ptr<FakePlatform> lifetime = platform;
    auto display = std::make_unique<NativeVirtualDisplay>(platform);
    assert(create(*display).success);
    Reply stopped;
    display->close(NativeVirtualDisplay::Clock::now() + 1s, stopped.completion());
    platform->releasing.waitForArrival();
    display.reset(); // Destructor must retain pending ownership until this proof finishes.
    platform->releasing.open();
    assert(stopped.get().cleanupVerified);
    platform.reset();
    const auto deadline = NativeVirtualDisplay::Clock::now() + 1s;
    while (!lifetime.expired() && NativeVirtualDisplay::Clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(lifetime.expired());
    NativeVirtualDisplay next(std::make_shared<FakePlatform>());
    assert(create(next).success);
    assert(close(next).cleanupVerified);
}

void duplicateCallbackMayReenter()
{
    NativeVirtualDisplay display(std::make_shared<FakePlatform>());
    assert(create(display).success);
    assert(close(display).cleanupVerified);
    Reply duplicate;
    display.close(NativeVirtualDisplay::Clock::now() + 1s, [&](auto result) {
        assert(result.cleanupVerified);
        const auto reused = create(display);
        assert(!reused.success && reused.cleanupVerified);
        duplicate.promise.set_value(std::move(result));
    });
    assert(duplicate.get().cleanupVerified);
}

void wrongModeStillMustBeRemoved()
{
    auto platform = std::make_shared<FakePlatform>();
    platform->matches = false;
    platform->blockObservation = true;
    NativeVirtualDisplay display(platform);
    Reply created;
    display.create({1920, 1080}, NativeVirtualDisplay::Clock::now() + 500ms, created.completion());
    platform->observation.waitForArrival();
    platform->observation.open();
    assert(!created.get().success);
    assert(platform->creations == 1 && platform->observations > 0);
    assert(close(display).cleanupVerified);
}

void lateAbsenceCannotUpgradeDeliveredUnknown()
{
    auto platform = std::make_shared<FakePlatform>();
    NativeVirtualDisplay display(platform);
    assert(create(display).success);
    platform->blockRemovalObservation = true;
    Reply stopped;
    display.close(NativeVirtualDisplay::Clock::now() + 500ms, stopped.completion());
    platform->observation.waitForArrival();
    assert(!stopped.get().cleanupVerified);
    platform->observation.open();
    std::this_thread::sleep_for(30ms);
    auto competingPlatform = std::make_shared<FakePlatform>();
    NativeVirtualDisplay competing(competingPlatform);
    const auto denied = create(competing);
    assert(!denied.success && denied.cleanupVerified && competingPlatform->creations == 0);
}

void exactModeIsSelectedOnlyDuringCreation()
{
    auto platform = std::make_shared<FakePlatform>();
    platform->selectionRequired = true;
    platform->matches = false;
    NativeVirtualDisplay display(platform);
    assert(create(display).success && platform->modeSelections == 1);
    assert(close(display).cleanupVerified && platform->modeSelections == 1);

    auto correct = std::make_shared<FakePlatform>();
    correct->selectionRequired = true;
    NativeVirtualDisplay unchanged(correct);
    assert(create(unchanged).success && correct->modeSelections == 0);
    assert(close(unchanged).cleanupVerified && correct->modeSelections == 0);
}

void cancelledOrExpiredSelectionCannotMutate()
{
    for (const bool expire : {false, true}) {
        auto platform = std::make_shared<FakePlatform>();
        platform->selectionRequired = true;
        platform->matches = false;
        platform->blockSelection = true;
        NativeVirtualDisplay display(platform);
        Reply created;
        display.create({1920, 1080}, NativeVirtualDisplay::Clock::now() + 500ms, created.completion());
        platform->selecting.waitForArrival();
        if (expire) {
            assert(!created.get().success);
            platform->selecting.open();
            const auto checkedBy = NativeVirtualDisplay::Clock::now() + 1s;
            while (!platform->modeAdmissionChecks && NativeVirtualDisplay::Clock::now() < checkedBy)
                std::this_thread::sleep_for(1ms);
            assert(platform->modeAdmissionChecks == 1 && platform->modeSelections == 0);
        }
        Reply stopped;
        display.close(NativeVirtualDisplay::Clock::now() + 1s, stopped.completion());
        if (!expire) {
            platform->selecting.open();
            assert(!created.get().success);
        }
        assert(stopped.get().cleanupVerified && platform->modeSelections == 0);
    }
}

void failedSelectionCannotRetryOrAdmitPartialSuccess()
{
    auto platform = std::make_shared<FakePlatform>();
    platform->selectionRequired = true;
    platform->selectionSucceeds = false;
    platform->matches = false;
    NativeVirtualDisplay display(platform);
    Reply created;
    display.create({1920, 1080}, NativeVirtualDisplay::Clock::now() + 500ms, created.completion());
    assert(!created.get().success);
    assert(platform->modeSelections == 1 && platform->matches);
    assert(close(display).cleanupVerified && platform->modeSelections == 1);
}

void unverifiedRemovalRetainsExclusiveOwnership(bool observable)
{
    auto platform = std::make_shared<FakePlatform>();
    platform->persistent = true;
    NativeVirtualDisplay display(platform);
    assert(create(display).success);
    platform->querySucceeded = observable;
    Reply stopped;
    display.close(NativeVirtualDisplay::Clock::now() + 50ms, stopped.completion());
    const auto result = stopped.get();
    assert(!result.success && !result.cleanupVerified && platform->releases == 1);
    auto competingPlatform = std::make_shared<FakePlatform>();
    NativeVirtualDisplay competing(competingPlatform);
    const auto denied = create(competing);
    assert(!denied.success && denied.cleanupVerified && competingPlatform->creations == 0);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--unknown-online") {
        unverifiedRemovalRetainsExclusiveOwnership(true);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--unknown-query") {
        unverifiedRemovalRetainsExclusiveOwnership(false);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--unknown-late") {
        lateAbsenceCannotUpgradeDeliveredUnknown();
        return 0;
    }
    assert(argc == 1);
    unavailableAndInvalidHaveNoEffects();
    presetsAndSingleOwnership();
    lostReadyStillRequiresOwnedScopeAbsence();
    cancellationWinsBlockedSuccessfulObservation();
    deadlineWinsBlockedSuccessfulObservation();
    pendingCloseDoesNotLeakRetainedOwner();
    duplicateCallbackMayReenter();
    wrongModeStillMustBeRemoved();
    exactModeIsSelectedOnlyDuringCreation();
    cancelledOrExpiredSelectionCannotMutate();
    failedSelectionCannotRetryOrAdmitPartialSuccess();
    return 0;
}
