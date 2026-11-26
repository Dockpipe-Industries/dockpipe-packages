// Inspect the production configuration builders without capture, hardware
// encoder creation, input posting or native permission APIs.
#include "../NativeDesktopBackend.mm"
#include <cassert>
#include <iostream>

using namespace dockpipe::desktop;

int main()
{
    @autoreleasepool {
        const CaptureQuality qualities[]{CaptureQuality::Low, CaptureQuality::Balanced, CaptureQuality::High, CaptureQuality::Ultra};
        const unsigned widths[]{1280, 1600, 1920, 3840};
        const unsigned heights[]{720, 900, 1080, 2160};
        const unsigned bitrates[]{1000000, 2500000, 4000000, 16000000};
        // Legacy Existing-only hosts must reject Virtual before touching TCC,
        // creating a display, or starting capture when no owner is configured.
        for (const auto quality : qualities) {
            NativeDesktopBackend unconfigured;
            SessionGrant grant{true, true, Clock::now() + std::chrono::seconds(1),
                {quality, 30, DisplaySource::Virtual}};
            unsigned completions = 0;
            unconfigured.start(grant, {}, [&completions](Result result) {
                ++completions;
                assert(result.outcome == Outcome::Failure && !result.error.empty());
            });
            assert(completions == 1);
        }

        for (unsigned quality = 0; quality < 4; ++quality) {
            for (unsigned rate : {10, 15, 24, 30, 60}) {
                auto state = std::make_shared<MacState>();
                state->profile = {qualities[quality], rate};
                state->displayBounds = CGRectMake(0, 0, 1920, 1080);
                state->displayPixelWidth = 3840;
                state->displayPixelHeight = 2160;
                const auto geometry = captureGeometry(state->profile, state->displayPixelWidth, state->displayPixelHeight);
                assert(geometry);
                state->width = geometry->width;
                state->height = geometry->height;
                SCStreamConfiguration *configuration = captureConfiguration(state);
                assert(configuration && configuration.width == widths[quality] && configuration.height == heights[quality]);
                assert(CMTimeCompare(configuration.minimumFrameInterval, CMTimeMake(1, rate)) == 0);
                assert(configuration.queueDepth == 3 && configuration.showsCursor && !configuration.capturesAudio);
                assert(configuration.pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
                NSDictionary *properties = encoderProperties(state->profile);
                assert(properties);
                assert([properties[(__bridge NSString *)kVTCompressionPropertyKey_ExpectedFrameRate] unsignedIntValue] == rate);
                assert([properties[(__bridge NSString *)kVTCompressionPropertyKey_AverageBitRate] unsignedIntValue] == bitrates[quality]);
                assert([properties[(__bridge NSString *)kVTCompressionPropertyKey_MaxKeyFrameInterval] unsignedIntValue] == rate * 2);
                assert([properties[(__bridge NSString *)kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration] unsignedIntValue] == 2);
                assert([properties[(__bridge NSString *)kVTCompressionPropertyKey_RealTime] boolValue]);
                assert(![properties[(__bridge NSString *)kVTCompressionPropertyKey_AllowFrameReordering] boolValue]);
                assert([properties[(__bridge NSString *)kVTCompressionPropertyKey_ProfileLevel]
                    isEqual:(__bridge NSString *)kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel]);
            }
        }
        assert([encoderSpecification()[(__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] boolValue]);
        assert(hardwareEncoderConfirmed(noErr, kCFBooleanTrue));
        assert(!hardwareEncoderConfirmed(noErr, kCFBooleanFalse));
        assert(!hardwareEncoderConfirmed(noErr, nullptr));
        assert(!hardwareEncoderConfirmed(-1, kCFBooleanTrue));
        assert(!hardwareEncoderConfirmed(noErr, (__bridge CFTypeRef)@1));
        assert(!encoderProperties({CaptureQuality::High, 120}));
        auto invalid = std::make_shared<MacState>();
        invalid->profile = {CaptureQuality(99), 30};
        invalid->width = 1920;
        invalid->height = 1080;
        assert(!captureConfiguration(invalid));
    }
    std::cout << "PASS actual SCStream FPS/geometry/cursor/audio bounds and VT bitrate/profile properties for all 20 captures; no capture/TCC/input\n";
}
