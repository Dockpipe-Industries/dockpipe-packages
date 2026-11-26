#pragma once

#include "WorkerProtocol.h"
#include <string_view>

namespace dockpipe::desktop {
enum class PowerPolicy { Off, Connected, Enabled };

inline PowerPolicy parsePowerPolicy(std::string_view mode)
{
    if (mode == "off")
        return PowerPolicy::Off;
    if (mode == "connected")
        return PowerPolicy::Connected;
    require(mode == "enabled", "Unsupported desktop power policy.");
    return PowerPolicy::Enabled;
}

inline const char *powerPolicyName(PowerPolicy policy)
{
    switch (policy) {
    case PowerPolicy::Off: return "off";
    case PowerPolicy::Connected: return "connected";
    case PowerPolicy::Enabled: return "enabled";
    }
    throw std::runtime_error("Invalid desktop power policy.");
}

inline PowerPolicy parsePowerPolicyRecord(const Json &record)
{
    require(record.is_object() && record.size() == 2
        && record.value("schema", "") == "dockpipe.desktop.power/v1"
        && record.contains("mode") && record["mode"].is_string(), "Invalid desktop power policy record.");
    return parsePowerPolicy(record["mode"].get<std::string>());
}

inline bool connectedPowerAssertion(PowerPolicy policy, bool sessionRunning)
{
    return policy == PowerPolicy::Connected && sessionRunning;
}

inline bool enabledPowerAssertion(PowerPolicy policy, bool ownerView, bool serviceRegistered)
{
    return policy == PowerPolicy::Enabled && ownerView && serviceRegistered;
}

PowerPolicy readPowerPolicy(const std::string &state);
void writePowerPolicy(const std::string &state, PowerPolicy policy);
bool connectedPowerPolicyAvailable(const std::string &state);
void publishConnectedPowerCapability(const std::string &state, const Json &provider);
int runPowerSettings(const std::string &state);
int configurePowerPolicy(const std::string &state, PowerPolicy policy);
} // namespace dockpipe::desktop
