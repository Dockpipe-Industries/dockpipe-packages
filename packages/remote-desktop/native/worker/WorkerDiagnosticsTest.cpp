#include "WorkerDiagnostics.h"
#include <cassert>
#include <iostream>

using namespace dockpipe::desktop;

int main()
{
    for (unsigned failed = 0; failed != 3; ++failed) {
        WorkerDiagnostics diagnostic;
        Result result{Outcome::Failure, "never emitted private error detail"};
        result.cleanup.capture = {true, failed != 0};
        result.cleanup.encoder = {true, failed != 1};
        result.cleanup.input = {true, failed != 2};
        diagnostic.stage(WorkerStage::NativeStop);
        diagnostic.nativeResult(result);
        diagnostic.failure();
        const auto snapshot = diagnostic.snapshot(false);
        assert(snapshot["reason"] == "cleanup-unverified");
        assert(snapshot["capture"]["verified"] == (failed != 0));
        assert(snapshot["encoder"]["verified"] == (failed != 1));
        assert(snapshot["input_release"]["verified"] == (failed != 2));
        assert(snapshot["native_unknown_observed"] == true);
        Result late{Outcome::Success, {}};
        late.cleanupVerified = true;
        late.cleanup = {{true, true}, {true, true}, {true, true}};
        diagnostic.nativeResult(late);
        assert(diagnostic.snapshot(false)["native_verified"] == false);
        assert(diagnostic.snapshot(false)["capture"] == snapshot["capture"]);
        assert(diagnostic.snapshot(false)["encoder"] == snapshot["encoder"]);
        assert(diagnostic.snapshot(false)["input_release"] == snapshot["input_release"]);
        assert(snapshot.dump().find("private error") == std::string::npos);
    }
    WorkerDiagnostics childFailure;
    Result native{Outcome::Success, {}};
    native.cleanupVerified = true;
    childFailure.nativeResult(native);
    childFailure.childStopped(false);
    childFailure.childStopped(true);
    childFailure.bridgeStopped(false);
    childFailure.bridgeStopped(true);
    assert(childFailure.snapshot(false)["native_verified"] == true);
    assert(childFailure.snapshot(false)["child_verified"] == false);
    assert(childFailure.snapshot(false)["bridge_verified"] == false);

    WorkerDiagnostics policy;
    policy.stage(WorkerStage::Policy);
    policy.failure();
    policy.stage(WorkerStage::Forget);
    policy.failure();
    assert(policy.snapshot(false)["reason"] == "policy-changed");
    assert(policy.snapshot(false)["failure_stage"] == "policy");
    assert(policy.snapshot(false)["forget_verified"] == false);
    policy.forgetVerified();
    assert(policy.snapshot(false)["forget_verified"] == true);
    assert(policy.snapshot(false)["reason"] == "policy-changed");

    WorkerDiagnostics unknown;
    unknown.stage(WorkerStage::Instance);
    unknown.failure();
    assert(unknown.unexpected());
    unknown.unexpectedHash(std::string(64, 'a'));
    const auto line = unknown.snapshot(false).dump();
    assert(line.size() < 2048 && line.find("unexpected_sha256") != std::string::npos);
    assert(line.find("/Users/") == std::string::npos && line.find("profile") == std::string::npos);
    std::cout << "PASS independent native components, sticky unknown child/bridge/native, first failure stage, bounded fixed-enum privacy\n";
}
