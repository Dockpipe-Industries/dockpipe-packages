#include "WorkerCleanup.h"
#include "WorkerRunLoop.h"
#import <Foundation/Foundation.h>
#include <cassert>
#include <iostream>

using namespace dockpipe::desktop;
using namespace std::chrono_literals;

int main()
{
    @autoreleasepool {
        assert([NSThread isMainThread]);
        const auto deadline = Clock::now() + 1s;
        const auto completion = std::make_shared<StopCompletion>(deadline);
        // A real Foundation timer needs the main run loop. Sleeping/polling a
        // shared stopped bit would never deliver this request's callback.
        [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:NO block:^(NSTimer *) {
            completion->complete(true);
        }];
        assert(awaitStopCompletion(*completion, deadline, [] { return Clock::now(); }, pumpMainRunLoop));
        const auto timeout = Clock::now() + 20ms;
        StopCompletion missing(timeout);
        assert(!awaitStopCompletion(missing, timeout, [] { return Clock::now(); }, pumpMainRunLoop));
        missing.complete(true);
        assert(!missing.verified());
        std::cout << "PASS real main NSRunLoop callback, bounded timeout, no late success; no capture/input\n";
    }
}
