#include "WorkerApplication.h"
#include <cassert>

int main()
{
    @autoreleasepool {
        dockpipe::desktop::prepareBackgroundWorkerApplication();

        assert(NSApp.activationPolicy == NSApplicationActivationPolicyAccessory);
        assert(NSRunningApplication.currentApplication.activationPolicy == NSApplicationActivationPolicyAccessory);
        assert(!NSRunningApplication.currentApplication.active);
        assert(NSApp.windows.count == 0);
    }
    return 0;
}
