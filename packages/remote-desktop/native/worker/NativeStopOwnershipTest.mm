// Include the private implementation to exercise the actual two stop helpers,
// without adding production hooks or invoking capture/privacy/input APIs.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace {
std::promise<void> finalizerStarted;
std::promise<void> allowFinalizer;
std::shared_future<void> finalizerAllowed = allowFinalizer.get_future().share();
std::atomic_bool invalidated{false};
VTCompressionSessionRef fixtureEncoder = nullptr;

OSStatus finishFixtureEncoder(VTCompressionSessionRef encoder, CMTime)
{
    assert(encoder == fixtureEncoder);
    finalizerStarted.set_value();
    assert(finalizerAllowed.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    return noErr;
}

void invalidateFixtureEncoder(VTCompressionSessionRef encoder)
{
    assert(encoder == fixtureEncoder);
    invalidated = true;
}
}

// Only the hardware finalization calls are replaced. The tested production
// finalizer still releases its real CF object and dispatches back to state.queue.
#define VTCompressionSessionCompleteFrames finishFixtureEncoder
#define VTCompressionSessionInvalidate invalidateFixtureEncoder
#include "../NativeDesktopBackend.mm"
#undef VTCompressionSessionCompleteFrames
#undef VTCompressionSessionInvalidate

@interface DPStopOwnershipStream : NSObject
@property(nonatomic, copy) void (^pendingStop)(NSError *);
- (void)stopCaptureWithCompletionHandler:(void (^)(NSError *))completion;
@end
@implementation DPStopOwnershipStream
- (void)stopCaptureWithCompletionHandler:(void (^)(NSError *))completion
{
    self.pendingStop = completion;
}
@end

using namespace dockpipe::desktop;
using namespace std::chrono_literals;

namespace {
bool waitUntil(const std::function<bool()> &ready, std::chrono::milliseconds bound)
{
    const auto deadline = Clock::now() + bound;
    while (!ready() && Clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return ready();
}
}

int main(int argc, char **)
{
    assert(argc == 1 || argc == 2);
    @autoreleasepool {
        if (argc == 1) {
            std::weak_ptr<MacState> finalizingState;
            auto started = finalizerStarted.get_future();
            std::promise<void> callerReturned;
            auto returned = callerReturned.get_future();
            std::promise<Result> stopped;
            auto completion = stopped.get_future();
            auto *callerReturnedPointer = &callerReturned;
            auto *stoppedPointer = &stopped;
            dispatch_queue_t callerQueue;
            {
                auto owner = std::make_shared<MacState>();
                finalizingState = owner;
                CFStringRef token = CFStringCreateWithCString(nullptr, "fixture encoder ownership", kCFStringEncodingUTF8);
                fixtureEncoder = reinterpret_cast<VTCompressionSessionRef>(const_cast<void *>(static_cast<const void *>(token)));
                owner->encoder = fixtureEncoder;
                callerQueue = owner->queue;
                dispatch_async(callerQueue, ^{
                    stopOnQueue(owner, [stoppedPointer](Result result) { stoppedPointer->set_value(std::move(result)); });
                    callerReturnedPointer->set_value();
                });
            }
            assert(started.wait_for(1s) == std::future_status::ready);
            assert(returned.wait_for(1s) == std::future_status::ready);
            // A following serial-queue item proves the caller closure returned and
            // was released; retaining the queue itself cannot retain MacState.
            dispatch_sync(callerQueue, ^{});
            if (finalizingState.expired()) {
                std::cerr << "FAIL escaped finalizer/watchdog borrowed caller shared_ptr storage\n";
                std::_Exit(13); // Old shape fails before dereferencing freed storage.
            }
            allowFinalizer.set_value();
            assert(completion.wait_for(2s) == std::future_status::ready);
            assert(completion.get().cleanupVerified && invalidated);
            // The production five-second watchdog also owns state until it retires.
            assert(!finalizingState.expired());
            assert(waitUntil([&] { return finalizingState.expired(); }, 6s));
        }

        DPStopOwnershipStream *stream = [[DPStopOwnershipStream alloc] init];
        std::weak_ptr<MacState> captureState;
        {
            auto owner = std::make_shared<MacState>();
            owner->stream = (SCStream *)stream; // Fake selector; no SCStream is created.
            captureState = owner;
            stopStream(owner);
        }
        if (captureState.expired()) {
            std::cerr << "FAIL delayed capture-stop completion borrowed caller shared_ptr storage\n";
            std::_Exit(14);
        }
        assert(stream.pendingStop);
        @autoreleasepool {
            void (^callback)(NSError *) = stream.pendingStop;
            stream.pendingStop = nil;
            callback(nil);
            callback = nil;
        }
        assert(waitUntil([&] { return captureState.expired(); }, 1s));
        std::cout << "PASS actual delayed finalizer, watchdog and fake capture-stop retain/release ownership after caller closure dies; no capture/privacy/input APIs\n";
    }
}
