#include "NativeVirtualDisplayOwnerProtocol.h"
#include "CaptureProfile.h"
#include <limits>
#include <stdexcept>
#include <set>
#include <vector>

namespace dockpipe::desktop {
namespace {
void requireOwner(bool condition)
{
    if (!condition)
        throw std::runtime_error("Invalid private virtual display owner protocol.");
}

bool nonceValid(const std::string &nonce)
{
    if (nonce.size() != 64)
        return false;
    for (const char value : nonce) {
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
            return false;
    }
    return true;
}

bool presetValid(NativeVirtualDisplaySpec spec)
{
    for (auto quality : {CaptureQuality::Low, CaptureQuality::Balanced, CaptureQuality::High, CaptureQuality::Ultra}) {
        const auto preset = capturePreset({quality, 30, DisplaySource::Virtual});
        if (preset && preset->maximumWidth == spec.width && preset->maximumHeight == spec.height)
            return true;
    }
    return false;
}
}

std::uint32_t virtualDisplayOwnerNumber(const nlohmann::json &value, std::uint32_t maximum)
{
    requireOwner(value.is_number_integer());
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        requireOwner(number <= maximum);
        return static_cast<std::uint32_t>(number);
    }
    const auto number = value.get<std::int64_t>();
    requireOwner(number >= 0 && static_cast<std::uint64_t>(number) <= maximum);
    return static_cast<std::uint32_t>(number);
}

nlohmann::json parseVirtualDisplayOwnerJson(const std::string &record)
{
    requireOwner(!record.empty() && record.size() <= VirtualDisplayOwnerRecordLimit);
    std::vector<std::set<std::string>> objects;
    auto callback = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json &value) {
        requireOwner(depth <= 8);
        if (event == nlohmann::json::parse_event_t::object_start) {
            objects.emplace_back();
        } else if (event == nlohmann::json::parse_event_t::key) {
            requireOwner(!objects.empty() && objects.back().insert(value.get<std::string>()).second);
        } else if (event == nlohmann::json::parse_event_t::object_end) {
            requireOwner(!objects.empty());
            objects.pop_back();
        }
        return true;
    };
    return nlohmann::json::parse(record, callback);
}

VirtualDisplayOwnerRequest parseVirtualDisplayOwnerRequest(const nlohmann::json &profile)
{
    requireOwner(profile.is_object() && profile.size() == 3
        && profile.value("schema", "") == "dockpipe.desktop.native.host/v1"
        && profile.value("operation", "") == "virtual-display-owner" && profile.contains("request"));
    const auto &value = profile.at("request");
    requireOwner(value.is_object() && value.size() == 7
        && value.value("schema", "") == "dockpipe.desktop.virtual-owner/v1"
        && value.contains("nonce") && value["nonce"].is_string());
    VirtualDisplayOwnerRequest result;
    result.nonce = value["nonce"].get<std::string>();
    requireOwner(nonceValid(result.nonce));
    result.spec.width = virtualDisplayOwnerNumber(value.at("width"), 3840);
    result.spec.height = virtualDisplayOwnerNumber(value.at("height"), 2160);
    result.spec.serial = virtualDisplayOwnerNumber(value.at("serial"), std::numeric_limits<std::uint32_t>::max());
    result.parentProcess = static_cast<int>(virtualDisplayOwnerNumber(value.at("parent_pid"), std::numeric_limits<int>::max()));
    requireOwner(result.spec.serial && result.parentProcess > 0 && presetValid(result.spec));
    const auto &deadline = value.at("create_deadline_unix_ms");
    requireOwner(deadline.is_number_integer());
    if (deadline.is_number_unsigned()) {
        const auto number = deadline.get<std::uint64_t>();
        requireOwner(number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        result.createDeadlineUnixMilliseconds = static_cast<std::int64_t>(number);
    } else {
        result.createDeadlineUnixMilliseconds = deadline.get<std::int64_t>();
    }
    requireOwner(result.createDeadlineUnixMilliseconds > 0);
    return result;
}

nlohmann::json virtualDisplayOwnerProfile(const VirtualDisplayOwnerRequest &request)
{
    return {{"schema", "dockpipe.desktop.native.host/v1"}, {"operation", "virtual-display-owner"},
        {"request", {{"schema", "dockpipe.desktop.virtual-owner/v1"}, {"nonce", request.nonce},
            {"serial", request.spec.serial}, {"width", request.spec.width}, {"height", request.spec.height},
            {"parent_pid", request.parentProcess}, {"create_deadline_unix_ms", request.createDeadlineUnixMilliseconds}}}};
}

nlohmann::json virtualDisplayOwnerMessage(const VirtualDisplayOwnerRequest &request, const char *type)
{
    return {{"schema", "dockpipe.desktop.virtual-owner/v1"}, {"type", type}, {"nonce", request.nonce},
        {"serial", request.spec.serial}, {"parent_pid", request.parentProcess}};
}

void validateVirtualDisplayOwnerMessage(const nlohmann::json &message, const VirtualDisplayOwnerRequest &request,
    const char *type, std::size_t fields)
{
    requireOwner(message.is_object() && message.size() == fields
        && message.value("schema", "") == "dockpipe.desktop.virtual-owner/v1" && message.value("type", "") == type
        && message.value("nonce", "") == request.nonce
        && virtualDisplayOwnerNumber(message.at("serial"), std::numeric_limits<std::uint32_t>::max()) == request.spec.serial
        && virtualDisplayOwnerNumber(message.at("parent_pid"), std::numeric_limits<int>::max()) == static_cast<unsigned>(request.parentProcess));
}
} // namespace dockpipe::desktop
