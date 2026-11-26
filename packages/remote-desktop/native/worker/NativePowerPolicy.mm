#include "PowerPolicy.h"
#include "PowerPolicyFile.h"
#import <AppKit/AppKit.h>
#include <cstdio>

namespace dockpipe::desktop {
namespace {
void reportPowerPolicy(PowerPolicy policy, bool saved)
{
    const Json result{{"schema", "dockpipe.desktop.power-settings/v1"}, {"mode", powerPolicyName(policy)},
        {"saved", saved}, {"assertion_observed", false}};
    const auto output = result.dump() + "\n";
    require(fwrite(output.data(), 1, output.size(), stdout) == output.size() && fflush(stdout) == 0,
        "Cannot report saved desktop power policy.");
}
}

PowerPolicy readPowerPolicy(const std::string &state)
{
    const auto record = PowerPolicyDirectory(state).read("desktop-power.json");
    return record ? parsePowerPolicyRecord(*record) : PowerPolicy::Off;
}

void writePowerPolicy(const std::string &state, PowerPolicy policy)
{
    PowerPolicyDirectory(state).write("desktop-power.json",
        Json{{"schema", "dockpipe.desktop.power/v1"}, {"mode", powerPolicyName(policy)}});
}

bool connectedPowerPolicyAvailable(const std::string &state)
{
    try {
        const PowerPolicyDirectory directory(state);
        const auto capability = directory.read("desktop-power-capabilities.json");
        const auto owner = directory.read("desktop.json");
        return capability && owner && capability->is_object() && capability->size() == 3
            && capability->value("schema", "") == "dockpipe.desktop.power-capabilities/v1"
            && capability->contains("connected") && (*capability)["connected"] == true
            && owner->value("schema", "") == "dockpipe.remote.desktop/v1"
            && capability->at("provider") == owner->at("provider");
    } catch (...) {
        return false; // Missing/stale/invalid capability never enables this choice.
    }
}

void publishConnectedPowerCapability(const std::string &state, const Json &provider)
{
    PowerPolicyDirectory(state).write("desktop-power-capabilities.json",
        Json{{"schema", "dockpipe.desktop.power-capabilities/v1"}, {"provider", provider}, {"connected", true}});
}

int configurePowerPolicy(const std::string &state, PowerPolicy policy)
{
    require(policy != PowerPolicy::Connected || connectedPowerPolicyAvailable(state),
        "While connected requires a matching upgraded desktop worker.");
    writePowerPolicy(state, policy);
    reportPowerPolicy(policy, true);
    return 0;
}

int runPowerSettings(const std::string &state)
{
    require([NSThread isMainThread], "Desktop power settings require the main thread.");
    const auto current = readPowerPolicy(state);
    const bool connectedAvailable = connectedPowerPolicyAvailable(state);
    auto *application = [NSApplication sharedApplication];
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    [application activateIgnoringOtherApps:YES];
    auto *choices = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 420, 30) pullsDown:NO];
    [choices addItemsWithTitles:@[@"Off", @"While connected", @"While remote desktop is enabled"]];
    choices.autoenablesItems = NO;
    [choices itemAtIndex:1].enabled = connectedAvailable;
    NSInteger selection = 0;
    if (current == PowerPolicy::Connected)
        selection = 1;
    else if (current == PowerPolicy::Enabled)
        selection = 2;
    [choices selectItemAtIndex:selection];
    auto *alert = [[NSAlert alloc] init];
    alert.messageText = @"Keep desktop awake";
    alert.informativeText = connectedAvailable
        ? @"Prevents idle display and system sleep. It does not unlock the Mac or grant remote access. The desktop power companion must be enabled for the host-wide option."
        : @"Prevents idle display and system sleep without unlocking the Mac or granting access. While connected is unavailable until a matching upgraded desktop worker starts. The host-wide option requires the desktop power companion.";
    alert.accessoryView = choices;
    [alert addButtonWithTitle:@"Save"];
    [alert addButtonWithTitle:@"Cancel"];
    if ([alert runModal] != NSAlertFirstButtonReturn) {
        reportPowerPolicy(current, false);
        return 0;
    }
    PowerPolicy selected = PowerPolicy::Off;
    if (choices.indexOfSelectedItem == 1)
        selected = PowerPolicy::Connected;
    else if (choices.indexOfSelectedItem == 2)
        selected = PowerPolicy::Enabled;
    return configurePowerPolicy(state, selected);
}
} // namespace dockpipe::desktop
