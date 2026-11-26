#pragma once

#include "NativeVirtualDisplay.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace dockpipe::desktop {

struct VirtualDisplayOwnerContext {
    std::string hostExecutable;
    // Effect-free context construction. Called only during create(), after a
    // private directory exists. The trusted callback clones only the opaque
    // profile of an operator-frozen full host descriptor, preserving its trust.
    std::function<std::string(const std::string &privateDirectory, const nlohmann::json &childProfile)> materializeDescriptor;
    std::function<bool(const std::string &snapshotPath)> verifySnapshot;
};

struct VirtualDisplayOwnerRequest {
    NativeVirtualDisplaySpec spec;
    std::string nonce;
    int parentProcess = 0;
    std::int64_t createDeadlineUnixMilliseconds = 0;
};

// Strict package-private profile parser. Root profile is exactly schema,
// operation and request; no CLI, capture, input or permission action is accepted.
VirtualDisplayOwnerRequest parseVirtualDisplayOwnerRequest(const nlohmann::json &profile);

// Main-thread CABI child entry. Owns only a virtual display, including its mode
// selection. Returning cleanupVerified=false honestly requires host process exit;
// only the parent may prove closure after reaping and public display absence.
NativeVirtualDisplayResult runVirtualDisplayOwnerChild(const VirtualDisplayOwnerRequest &request,
    const std::function<bool()> &stopRequested, const std::string &loadedModulePath);

// Construction performs no spawn, filesystem write or display operation. Each
// returned platform admits exactly one child/display during create().
std::shared_ptr<NativeVirtualDisplay::Platform> makeVirtualDisplayOwnerPlatform(VirtualDisplayOwnerContext context);

} // namespace dockpipe::desktop
