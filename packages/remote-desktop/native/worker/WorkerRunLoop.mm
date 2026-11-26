#include "WorkerRunLoop.h"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <thread>

namespace dockpipe::desktop {
void pumpMainRunLoop(Clock::time_point deadline)
{
    @autoreleasepool {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero())
            return;
        const auto seconds = std::chrono::duration<double>(remaining).count();
        const bool handled = [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
        const auto after = deadline - Clock::now();
        if (!handled && after > Clock::duration::zero())
            std::this_thread::sleep_for(std::min(after, Clock::duration(std::chrono::milliseconds(1))));
    }
}
} // namespace dockpipe::desktop
