#include "NativePowerAssertion.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace dockpipe::desktop;

namespace {
class FakePower final : public NativePowerAssertion::Platform {
public:
    NativePowerAssertion::Creation create(PowerAssertionKind kind) override
    {
        ++creates;
        if (throwCreation)
            throw std::runtime_error("fixture create outcome unavailable");
        if (invalidIdentity)
            return {true, 0};
        if (creates == failCreation)
            return {};
        const auto identifier = ++next;
        owned.emplace(identifier, kind);
        peak = std::max(peak, owned.size());
        return {true, identifier};
    }
    bool verify(std::uint32_t identifier, PowerAssertionKind kind) override
    {
        ++verifications;
        const auto found = owned.find(identifier);
        return verifications != failVerification && found != owned.end() && found->second == kind;
    }
    bool release(std::uint32_t identifier) override
    {
        ++releases;
        releasedIDs.push_back(identifier);
        if (releaseFailures) {
            --releaseFailures;
            return false;
        }
        if (throwRelease)
            throw std::runtime_error("fixture release outcome unavailable");
        return owned.erase(identifier) == 1;
    }
    std::map<std::uint32_t, PowerAssertionKind> owned;
    std::vector<std::uint32_t> releasedIDs;
    std::size_t peak = 0;
    unsigned creates = 0;
    unsigned releases = 0;
    unsigned verifications = 0;
    unsigned failCreation = 0;
    unsigned failVerification = 0;
    unsigned releaseFailures = 0;
    std::uint32_t next = 100;
    bool throwCreation = false;
    bool throwRelease = false;
    bool invalidIdentity = false;
};

void fullPairIsIdempotentAndReleasesExactlyOwnedReferences()
{
    auto platform = std::make_shared<FakePower>();
    NativePowerAssertion assertion(platform);
    assert(assertion.cleanupVerified() && !assertion.active());
    const auto started = assertion.setActive(true);
    assert(started.success && started.active && !started.cleanupVerified);
    assert(platform->owned.size() == 2 && platform->verifications == 2);
    for (unsigned index = 0; index < 20; ++index)
        assert(assertion.setActive(true).success);
    assert(platform->creates == 2 && platform->peak == 2);
    const auto stopped = assertion.setActive(false);
    assert(stopped.success && !stopped.active && stopped.cleanupVerified);
    assert(platform->owned.empty() && platform->releasedIDs == std::vector<std::uint32_t>({102, 101}));
    assert(assertion.release().success && platform->releases == 2);
}

void partialAcquireAndVerificationRollback()
{
    for (unsigned failure : {1, 2}) {
        auto platform = std::make_shared<FakePower>();
        platform->failCreation = failure;
        NativePowerAssertion assertion(platform);
        const auto result = assertion.setActive(true);
        assert(!result.success && !result.active && result.cleanupVerified);
        assert(result.failure == PowerAssertionFailure::Acquire && platform->owned.empty());
        assert(platform->creates == failure && platform->releases == failure - 1);
    }
    for (unsigned failure : {1, 2}) {
        auto platform = std::make_shared<FakePower>();
        platform->failVerification = failure;
        NativePowerAssertion assertion(platform);
        const auto result = assertion.setActive(true);
        assert(!result.success && !result.active && result.cleanupVerified);
        assert(result.failure == PowerAssertionFailure::Verification && platform->owned.empty());
        assert(platform->releases == failure);
    }
}

void cancellationBeforeAndDuringAcquireReleasesPartialOwnership()
{
    for (unsigned stopAfter : {0, 1, 2}) {
        auto platform = std::make_shared<FakePower>();
        NativePowerAssertion assertion(platform);
        const auto result = assertion.setActive(true, [&] { return platform->creates >= stopAfter; });
        assert(!result.success && result.cancelled && !result.active && result.cleanupVerified);
        assert(platform->creates == stopAfter && platform->owned.empty());
    }
}

void releaseRetriesAreBoundedAndUnknownDoesNotAllocateOverOldIDs()
{
    auto platform = std::make_shared<FakePower>();
    NativePowerAssertion assertion(platform);
    assert(assertion.setActive(true).success);
    platform->releaseFailures = 100;
    const auto unknown = assertion.release();
    assert(!unknown.success && !unknown.cleanupVerified && !unknown.active);
    assert(unknown.failure == PowerAssertionFailure::Release && platform->releases == 6);
    assert(!assertion.setActive(true).success);
    assert(platform->creates == 2 && platform->releases == 12 && platform->owned.size() == 2);
    platform->releaseFailures = 0;
    assert(assertion.release().cleanupVerified && platform->owned.empty());
    assert(assertion.setActive(true).success && platform->creates == 4 && platform->peak == 2);
    assert(assertion.release().cleanupVerified);
}

void transientReleaseFailureAndCancellationUnknownAreTruthful()
{
    auto platform = std::make_shared<FakePower>();
    NativePowerAssertion assertion(platform);
    assert(assertion.setActive(true).success);
    platform->releaseFailures = 2;
    assert(assertion.release().cleanupVerified && platform->releases == 4);
    assert(assertion.setActive(true).success);
    platform->releaseFailures = 100;
    const auto cancelled = assertion.setActive(true, [] { return true; });
    assert(cancelled.cancelled && !cancelled.cleanupVerified && cancelled.failure == PowerAssertionFailure::Release);
    platform->releaseFailures = 0;
    assert(assertion.release().cleanupVerified);
}

void unexpectedFailuresDoNotInventAbsence()
{
    auto platform = std::make_shared<FakePower>();
    platform->throwCreation = true;
    NativePowerAssertion unknown(platform);
    assert(!unknown.setActive(true).cleanupVerified);
    platform->throwCreation = false;
    assert(!unknown.setActive(true).success && platform->creates == 1);
    assert(!unknown.release().cleanupVerified);

    auto invalid = std::make_shared<FakePower>();
    invalid->invalidIdentity = true;
    NativePowerAssertion invalidOwner(invalid);
    assert(!invalidOwner.setActive(true).cleanupVerified);
    assert(!invalidOwner.release().cleanupVerified);

    auto releasing = std::make_shared<FakePower>();
    NativePowerAssertion owner(releasing);
    assert(owner.setActive(true).success);
    releasing->throwRelease = true;
    assert(!owner.release().cleanupVerified && releasing->releases == 6);
    releasing->throwRelease = false;
    assert(owner.release().cleanupVerified);
}

void destructorReleasesAndConcurrentUpdatesStayBounded()
{
    auto platform = std::make_shared<FakePower>();
    {
        NativePowerAssertion assertion(platform);
        std::vector<std::thread> threads;
        for (unsigned index = 0; index < 4; ++index) {
            threads.emplace_back([&] {
                for (unsigned step = 0; step < 50; ++step)
                    assert(assertion.setActive(step % 2 == 0).success);
            });
        }
        for (auto &thread : threads)
            thread.join();
        assert(assertion.setActive(true).success && platform->peak <= 2);
    }
    assert(platform->owned.empty());
}
} // namespace

int main()
{
    NativePowerAssertion unavailable(nullptr);
    assert(!unavailable.setActive(true).success && unavailable.cleanupVerified());
    fullPairIsIdempotentAndReleasesExactlyOwnedReferences();
    partialAcquireAndVerificationRollback();
    cancellationBeforeAndDuringAcquireReleasesPartialOwnership();
    releaseRetriesAreBoundedAndUnknownDoesNotAllocateOverOldIDs();
    transientReleaseFailureAndCancellationUnknownAreTruthful();
    unexpectedFailuresDoNotInventAbsence();
    destructorReleasesAndConcurrentUpdatesStayBounded();
}
