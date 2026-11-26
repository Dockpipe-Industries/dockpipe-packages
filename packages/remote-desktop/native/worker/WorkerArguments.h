#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace dockpipe::desktop {
enum class PermissionMode { None, Screen, Input, Status };
struct WorkerArguments {
    PermissionMode permission = PermissionMode::None;
    std::map<std::string, std::string> paths;
};

inline WorkerArguments parseWorkerArguments(const std::vector<std::string> &arguments)
{
    WorkerArguments result;
    if (arguments.size() == 2 && arguments[0] == "--permissions") {
        if (arguments[1] == "screen")
            result.permission = PermissionMode::Screen;
        else if (arguments[1] == "input")
            result.permission = PermissionMode::Input;
        else if (arguments[1] == "status")
            result.permission = PermissionMode::Status;
        else
            throw std::invalid_argument("Unsupported local permission operation.");
        return result;
    }
    if (arguments.size() != 8)
        throw std::invalid_argument("Native worker requires four trusted absolute paths.");
    for (std::size_t index = 0; index < arguments.size(); index += 2) {
        if (arguments[index + 1].empty() || arguments[index + 1].front() != '/' || !result.paths.emplace(arguments[index], arguments[index + 1]).second)
            throw std::invalid_argument("Invalid native worker path argument.");
    }
    if (!result.paths.contains("--cli") || !result.paths.contains("--state") || !result.paths.contains("--channel-helper") || !result.paths.contains("--manifest"))
        throw std::invalid_argument("Native worker requires frozen trusted paths.");
    return result;
}
} // namespace dockpipe::desktop
