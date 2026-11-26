#include "NativeVirtualDisplayObserver.h"
#include "NativeVirtualDisplayOwnerProtocol.h"
#include <climits>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dockpipe::desktop {
namespace {
void requireObservation(bool condition)
{
    if (!condition)
        throw std::runtime_error("Invalid private virtual display observation.");
}
}

VirtualDisplayObserverRequest parseVirtualDisplayObserverRequest(const nlohmann::json &profile)
{
    requireObservation(profile.is_object() && profile.size() == 3
        && profile.value("schema", "") == "dockpipe.desktop.native.host/v1"
        && profile.value("operation", "") == "virtual-display-observe");
    const auto &request = profile.at("request");
    requireObservation(request.is_object() && request.size() == 8
        && request.value("schema", "") == "dockpipe.desktop.virtual-observer/v1");
    // Reuse the strict nonce, preset and numeric admission contract. This only
    // maps typed fields; it does not introduce an alternate JSON parser.
    auto owner = request;
    owner["schema"] = "dockpipe.desktop.virtual-owner/v1";
    owner["create_deadline_unix_ms"] = owner.at("deadline_unix_ms");
    owner.erase("deadline_unix_ms");
    owner.erase("display");
    const auto typed = parseVirtualDisplayOwnerRequest({{"schema", "dockpipe.desktop.native.host/v1"},
        {"operation", "virtual-display-owner"}, {"request", owner}});
    VirtualDisplayObserverRequest result{typed.spec,
        virtualDisplayOwnerNumber(request.at("display"), std::numeric_limits<std::uint32_t>::max()),
        typed.nonce, typed.parentProcess, typed.createDeadlineUnixMilliseconds};
    requireObservation(result.display != 0);
    return result;
}

nlohmann::json virtualDisplayObserverProfile(const VirtualDisplayObserverRequest &request)
{
    return {{"schema", "dockpipe.desktop.native.host/v1"}, {"operation", "virtual-display-observe"},
        {"request", {{"schema", "dockpipe.desktop.virtual-observer/v1"}, {"nonce", request.nonce},
            {"parent_pid", request.parentProcess}, {"display", request.display}, {"serial", request.spec.serial},
            {"width", request.spec.width}, {"height", request.spec.height},
            {"deadline_unix_ms", request.deadlineUnixMilliseconds}}}};
}

nlohmann::json virtualDisplayObserverMessage(const VirtualDisplayObserverRequest &request, const char *type)
{
    return {{"schema", "dockpipe.desktop.virtual-observer/v1"}, {"type", type}, {"nonce", request.nonce},
        {"parent_pid", request.parentProcess}, {"display", request.display}, {"serial", request.spec.serial}};
}

void validateVirtualDisplayObserverMessage(const nlohmann::json &message,
    const VirtualDisplayObserverRequest &request, const char *type, std::size_t fields)
{
    requireObservation(message.is_object() && message.size() == fields
        && message.value("schema", "") == "dockpipe.desktop.virtual-observer/v1"
        && message.value("type", "") == type && message.value("nonce", "") == request.nonce
        && virtualDisplayOwnerNumber(message.at("parent_pid"), INT_MAX) == static_cast<unsigned>(request.parentProcess)
        && virtualDisplayOwnerNumber(message.at("display"), UINT_MAX) == request.display
        && virtualDisplayOwnerNumber(message.at("serial"), UINT_MAX) == request.spec.serial);
}

NativeVirtualMode parseVirtualDisplayObservation(const nlohmann::json &message,
    const VirtualDisplayObserverRequest &request, int observerProcess)
{
    validateVirtualDisplayObserverMessage(message, request, "observed", 8);
    requireObservation(virtualDisplayOwnerNumber(message.at("pid"), INT_MAX) == static_cast<unsigned>(observerProcess));
    const auto &value = message.at("observation");
    requireObservation(value.is_object() && value.size() == 18
        && value.at("online_complete") == true && value.at("online") == true && value.at("active") == true
        && value.at("mirrored") == false && value.at("mode_present") == true
        && virtualDisplayOwnerNumber(value.at("mirror_target"), UINT_MAX) == 0
        && virtualDisplayOwnerNumber(value.at("vendor"), UINT_MAX) == 0x4450
        && virtualDisplayOwnerNumber(value.at("product"), UINT_MAX) == 1
        && virtualDisplayOwnerNumber(value.at("serial"), UINT_MAX) == request.spec.serial
        && virtualDisplayOwnerNumber(value.at("pixels_width"), 3840) == request.spec.width
        && virtualDisplayOwnerNumber(value.at("pixels_height"), 2160) == request.spec.height
        && value.at("bounds_width").is_number() && value.at("bounds_height").is_number()
        && value.at("bounds_width").get<double>() == request.spec.width
        && value.at("bounds_height").get<double>() == request.spec.height
        && value.at("refresh_rate").is_number());
    NativeVirtualMode mode{virtualDisplayOwnerNumber(value.at("mode_width"), 3840),
        virtualDisplayOwnerNumber(value.at("mode_height"), 2160),
        virtualDisplayOwnerNumber(value.at("mode_pixel_width"), 3840),
        virtualDisplayOwnerNumber(value.at("mode_pixel_height"), 2160), value.at("refresh_rate").get<double>()};
    requireObservation(exactVirtualMode(mode, request.spec));
    return mode;
}
} // namespace dockpipe::desktop
