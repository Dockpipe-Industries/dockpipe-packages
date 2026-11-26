#pragma once

#include "NativeVirtualDisplayOwner.h"

namespace dockpipe::desktop {
inline constexpr std::size_t VirtualDisplayOwnerRecordLimit = 4096;

nlohmann::json parseVirtualDisplayOwnerJson(const std::string &record);
nlohmann::json virtualDisplayOwnerProfile(const VirtualDisplayOwnerRequest &request);
nlohmann::json virtualDisplayOwnerMessage(const VirtualDisplayOwnerRequest &request, const char *type);
void validateVirtualDisplayOwnerMessage(const nlohmann::json &message, const VirtualDisplayOwnerRequest &request,
    const char *type, std::size_t fields);
std::uint32_t virtualDisplayOwnerNumber(const nlohmann::json &value, std::uint32_t maximum);
} // namespace dockpipe::desktop
