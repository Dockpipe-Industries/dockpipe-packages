#pragma once

#include "WorkerProtocol.h"
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace dockpipe::desktop {
struct VirtualDisplayHostTemplateReference {
    std::string path;
    std::string sha256;
};

struct WorkerHostIdentity {
    std::string moduleSha256;
    std::string cliPath;
    std::string cliSha256;
    std::string stateDirectory;
    std::string hostExecutable;
    std::string hostSha256;
    // Empty only for an explicitly admitted ad-hoc review host.
    std::string productionTeamIdentifier;
};

inline bool exactTemplateFields(const Json &value, std::initializer_list<const char *> fields)
{
    if (!value.is_object() || value.size() != fields.size())
        return false;
    for (const auto field : fields) {
        if (!value.contains(field))
            return false;
    }
    return true;
}

inline bool templateAbsolutePath(const Json &value)
{
    if (!value.is_string())
        return false;
    const auto path = value.get<std::string>();
    return !path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos
        && std::filesystem::path(path).is_absolute()
        && std::filesystem::path(path).lexically_normal().string() == path;
}

inline Json parseWorkerHostTemplate(const std::string &bytes)
{
    require(!bytes.empty() && bytes.size() <= 32768, "Virtual display host template exceeds its bound.");
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json &value) {
        require(depth >= 0 && depth <= 16, "Virtual display host template nesting exceeds its bound.");
        if (event == Json::parse_event_t::object_start) {
            keys.emplace_back();
        } else if (event == Json::parse_event_t::key) {
            require(!keys.empty() && keys.back().insert(value.get<std::string>()).second,
                "Virtual display host template contains duplicate fields.");
        } else if (event == Json::parse_event_t::object_end) {
            require(!keys.empty(), "Invalid virtual display host template structure.");
            keys.pop_back();
        }
        return true;
    });
}

inline void validateWorkerHostTemplate(const Json &descriptor, const WorkerHostIdentity &identity)
{
    require(exactTemplateFields(descriptor, {"schema", "module", "cli", "state_directory", "profile", "trust"})
        && descriptor["schema"] == "dockpipe.native-worker-host/v1", "Invalid virtual display host template.");
    const auto &module = descriptor["module"];
    const auto &cli = descriptor["cli"];
    require(exactTemplateFields(module, {"path", "sha256"}) && templateAbsolutePath(module["path"])
        && digest(module["sha256"]) && module["sha256"] == identity.moduleSha256,
        "Virtual display template differs from the actual loaded provider image.");
    require(exactTemplateFields(cli, {"path", "sha256"}) && templateAbsolutePath(cli["path"])
        && digest(cli["sha256"]) && cli["path"] == identity.cliPath && cli["sha256"] == identity.cliSha256
        && templateAbsolutePath(descriptor["state_directory"])
        && descriptor["state_directory"] == identity.stateDirectory,
        "Virtual display template changes the trusted CLI or state authority.");
    const auto expectedHost = std::filesystem::path(identity.cliPath).parent_path().parent_path() / "MacOS/DockPipe";
    require(expectedHost.string() == identity.hostExecutable,
        "Virtual display owner must use the actual bundled Root host executable.");
    require(exactTemplateFields(descriptor["profile"], {"schema", "operation"})
        && descriptor["profile"]["schema"] == "dockpipe.desktop.native.host/v1"
        && descriptor["profile"]["operation"] == "virtual-display-owner",
        "Virtual display template has an unsupported local operation.");
    const auto &trust = descriptor["trust"];
    if (identity.productionTeamIdentifier.empty()) {
        require(exactTemplateFields(trust, {"mode", "host_sha256"}) && trust["mode"] == "review-adhoc"
            && digest(trust["host_sha256"]) && trust["host_sha256"] == identity.hostSha256,
            "Virtual display template changes the explicit review host identity.");
    } else {
        require(identity.productionTeamIdentifier.size() == 10,
            "Actual production host team identifier is malformed.");
        for (const auto character : identity.productionTeamIdentifier)
            require((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'),
                "Actual production host team identifier is malformed.");
        require(exactTemplateFields(trust, {"mode", "team_identifier"}) && trust["mode"] == "production"
            && trust["team_identifier"] == identity.productionTeamIdentifier,
            "Virtual display template changes the production team policy.");
    }
}

inline Json cloneVirtualDisplayHostDescriptor(const Json &descriptor, const Json &childProfile)
{
    require(childProfile.is_object() && childProfile.value("schema", "") == "dockpipe.desktop.native.host/v1",
        "Invalid virtual-display-only child profile.");
    const auto operation = childProfile.value("operation", "");
    require((operation == "virtual-display-owner" || operation == "virtual-display-observe")
        && childProfile.dump().size() <= 16384, "Invalid virtual-display-only child profile.");
    auto result = descriptor;
    result["profile"] = childProfile;
    auto restored = result;
    restored["profile"] = descriptor["profile"];
    require(restored == descriptor, "Virtual display clone changed outer host authority.");
    return result;
}
struct VirtualDisplayOwnerContext;
VirtualDisplayOwnerContext loadVirtualDisplayOwnerContext(const VirtualDisplayHostTemplateReference &reference,
    const std::string &cli, const std::string &state, const std::string &providerImage);
} // namespace dockpipe::desktop
