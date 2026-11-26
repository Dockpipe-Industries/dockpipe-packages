#pragma once

#include <DockpipeNativeWorkerPackage.h>
#include "WorkerProtocol.h"
#include "WorkerArguments.h"
#include "PowerPolicy.h"
#include "WorkerHostTemplate.h"
#include "../NativeVirtualDisplayOwner.h"
#include "../NativeVirtualDisplayOwnerProtocol.h"
#include "../NativeVirtualDisplayObserver.h"
#include <optional>
#include <cstring>

namespace dockpipe::desktop {
enum class WorkerOperation { Worker, PowerAgent, PowerSettings, ConfigurePower, VirtualDisplayOwner, VirtualDisplayObserver };

struct WorkerPackageProfile {
    WorkerOperation operation = WorkerOperation::Worker;
    PowerPolicy powerPolicy = PowerPolicy::Off;
    Json desktopProvider;
    std::string desktopServiceLabel;
    PermissionMode permission = PermissionMode::None;
    std::string helper;
    std::string manifest;
    std::optional<VirtualDisplayHostTemplateReference> virtualDisplayHostTemplate;
    std::optional<VirtualDisplayOwnerRequest> virtualDisplayOwnerRequest;
    std::optional<VirtualDisplayObserverRequest> virtualDisplayObserverRequest;
};

inline bool absoluteHostPath(const char *path)
{
    return path && path[0] == '/' && strnlen(path, 4097) <= 4096;
}

inline WorkerPackageProfile parseWorkerPackageProfile(const DockpipeNativeWorkerHostV1 &host)
{
    require(host.abi_version == DOCKPIPE_NATIVE_WORKER_ABI_V1 && host.struct_size == sizeof(host),
        "Unsupported native worker ABI.");
    require(absoluteHostPath(host.cli_path) && absoluteHostPath(host.state_directory)
        && host.stop_requested && host.profile_json && host.profile_json_bytes > 0
        && host.profile_json_bytes <= DOCKPIPE_NATIVE_WORKER_PROFILE_LIMIT_V1,
        "Invalid native worker host context.");
    const std::string rawProfile(host.profile_json, host.profile_json_bytes);
    const auto profile = parseWorkerHostTemplate(rawProfile);
    require(profile.is_object() && profile.value("schema", "") == "dockpipe.desktop.native.host/v1"
        && profile.contains("operation") && profile["operation"].is_string(), "Invalid native worker profile.");
    WorkerPackageProfile result;
    const auto operation = profile["operation"].get<std::string>();
    if (operation == "status")
        result.permission = PermissionMode::Status;
    else if (operation == "request-screen")
        result.permission = PermissionMode::Screen;
    else if (operation == "request-input")
        result.permission = PermissionMode::Input;
    else if (operation == "power-settings")
        result.operation = WorkerOperation::PowerSettings;
    else if (operation == "configure-power")
        result.operation = WorkerOperation::ConfigurePower;
    else if (operation == "virtual-display-observe")
        result.operation = WorkerOperation::VirtualDisplayObserver;
    else if (operation == "virtual-display-owner")
        result.operation = WorkerOperation::VirtualDisplayOwner;
    else if (operation == "power-agent")
        result.operation = WorkerOperation::PowerAgent;
    else
        require(operation == "worker", "Unsupported local native worker operation.");

    if (result.permission != PermissionMode::None) {
        require(profile.size() == 2, "Permission actions cannot carry worker configuration.");
    } else if (result.operation == WorkerOperation::VirtualDisplayObserver) {
        result.virtualDisplayObserverRequest = parseVirtualDisplayObserverRequest(parseVirtualDisplayOwnerJson(rawProfile));
    } else if (result.operation == WorkerOperation::VirtualDisplayOwner) {
        result.virtualDisplayOwnerRequest = parseVirtualDisplayOwnerRequest(parseVirtualDisplayOwnerJson(rawProfile));
    } else if (result.operation == WorkerOperation::PowerSettings) {
        require(profile.size() == 2, "Power settings cannot carry worker configuration.");
    } else if (result.operation == WorkerOperation::ConfigurePower) {
        require(profile.size() == 3 && profile.contains("mode") && profile["mode"].is_string(),
            "Invalid desktop power configuration operation.");
        result.powerPolicy = parsePowerPolicy(profile["mode"].get<std::string>());
    } else {
        const bool hasVirtualTemplate = profile.contains("virtual_display_host_template");
        require(!hasVirtualTemplate || result.operation == WorkerOperation::Worker,
            "Only the worker may configure a virtual display owner template.");
        std::size_t expectedSize = 4;
        if (result.operation == WorkerOperation::PowerAgent)
            expectedSize = 6;
        else if (hasVirtualTemplate)
            expectedSize = 5;
        require(profile.size() == expectedSize && profile.contains("channel_helper") && profile["channel_helper"].is_string()
            && profile.contains("manifest") && profile["manifest"].is_string(), "Invalid native worker configuration.");
        if (result.operation == WorkerOperation::PowerAgent) {
            require(profile.contains("desktop_provider") && profile["desktop_provider"].is_object()
                && profile["desktop_provider"].size() == 3 && profile["desktop_provider"].contains("id")
                && profile["desktop_provider"]["id"].is_string() && profile["desktop_provider"].contains("version")
                && profile["desktop_provider"]["version"].is_string() && profile["desktop_provider"].contains("artifact")
                && digest(profile["desktop_provider"]["artifact"])
                && profile.contains("desktop_service_label") && profile["desktop_service_label"].is_string(),
                "Invalid independent desktop power service binding.");
            result.desktopProvider = profile["desktop_provider"];
            result.desktopServiceLabel = profile["desktop_service_label"].get<std::string>();
            require(!result.desktopServiceLabel.empty() && result.desktopServiceLabel.size() <= 128,
                "Desktop service label exceeds its bound.");
            for (const unsigned char character : result.desktopServiceLabel)
                require((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9') || character == '.' || character == '-',
                    "Desktop service label contains unsupported characters.");
        }
        if (hasVirtualTemplate) {
            const auto &value = profile["virtual_display_host_template"];
            require(exactTemplateFields(value, {"path", "sha256"}) && templateAbsolutePath(value["path"])
                && digest(value["sha256"]), "Invalid pinned virtual display host template.");
            result.virtualDisplayHostTemplate = VirtualDisplayHostTemplateReference{
                value["path"].get<std::string>(), value["sha256"].get<std::string>()};
        }
        result.helper = profile["channel_helper"].get<std::string>();
        result.manifest = profile["manifest"].get<std::string>();
        require(result.helper.find('\0') == std::string::npos && result.manifest.find('\0') == std::string::npos
            && absoluteHostPath(result.helper.c_str()) && absoluteHostPath(result.manifest.c_str()),
            "Native worker requires trusted absolute asset paths.");
    }
    return result;
}
} // namespace dockpipe::desktop
