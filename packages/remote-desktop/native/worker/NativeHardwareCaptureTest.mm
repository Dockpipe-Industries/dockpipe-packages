// Original synthetic NV12 luma bars. No display capture, input, TCC request or
// screen pixels: this exercises the production encoder settings/SPS converter.
#include "../NativeDesktopBackend.mm"
#include <array>
#include <cassert>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace dockpipe::desktop;

namespace {
struct Output {
    CaptureProfile profile;
    unsigned width;
    unsigned height;
    std::mutex mutex;
    std::array<EncodedFrame, 2> frames;
    std::array<bool, 2> received{};
    bool failed = false;
};

void encoded(void *context, void *ticket, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
    auto &output = *static_cast<Output *>(context);
    const auto index = reinterpret_cast<std::uintptr_t>(ticket) - 1;
    std::lock_guard lock(output.mutex);
    if (status != noErr || !sample || (flags & kVTEncodeInfo_FrameDropped) || index >= output.frames.size()) {
        output.failed = true;
        return;
    }
    auto &frame = output.frames[index];
    frame.width = output.width;
    frame.height = output.height;
    const auto result = encodedFrame(sample, frame, output.profile);
    if (!result || !frame.keyframe || frame.annexB.size() > 2 * 1024 * 1024 - 32) {
        output.failed = true;
        return;
    }
    output.received[index] = true;
}

void save(const std::string &path, const std::vector<std::uint8_t> &bytes)
{
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(descriptor >= 0);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = write(descriptor, bytes.data() + offset, bytes.size() - offset);
        assert(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    assert(close(descriptor) == 0);
}

nlohmann::json run(const char *name, CaptureProfile profile, unsigned width, unsigned height, unsigned expectedLevel)
{
    Output output{profile, width, height};
    VTCompressionSessionRef session = nullptr;
    NSDictionary *specification = encoderSpecification();
    assert(VTCompressionSessionCreate(kCFAllocatorDefault, width, height, kCMVideoCodecType_H264,
        (__bridge CFDictionaryRef)specification, nullptr, nullptr, encoded, &output, &session) == noErr);
    assert(session);
    assert(VTSessionSetProperties(session, (__bridge CFDictionaryRef)encoderProperties(profile)) == noErr);
    assert(VTCompressionSessionPrepareToEncodeFrames(session) == noErr);
    assert(confirmHardwareEncoder(session));
    for (unsigned index = 0; index < 2; ++index) {
        CVPixelBufferRef pixel = nullptr;
        NSDictionary *attributes = @{(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        assert(CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            (__bridge CFDictionaryRef)attributes, &pixel) == kCVReturnSuccess);
        assert(CVPixelBufferLockBaseAddress(pixel, 0) == kCVReturnSuccess);
        auto *luma = static_cast<std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel, 0));
        auto *chroma = static_cast<std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel, 1));
        const auto yStride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
        const auto uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 1);
        const std::uint8_t bars[]{16, 82, 145, 235};
        for (unsigned y = 0; y < height; ++y) {
            std::fill(luma + y * yStride, luma + (y + 1) * yStride, 16);
            for (unsigned x = 0; x < width; ++x) {
                const auto bar = std::min(3U, x * 4 / width);
                luma[y * yStride + x] = bars[index ? 3 - bar : bar];
            }
        }
        for (unsigned y = 0; y < height / 2; ++y)
            std::fill(chroma + y * uvStride, chroma + (y + 1) * uvStride, 128);
        assert(CVPixelBufferUnlockBaseAddress(pixel, 0) == kCVReturnSuccess);
        NSDictionary *options = @{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES};
        assert(VTCompressionSessionEncodeFrame(session, pixel, CMTimeMake(index, profile.frameRate),
            CMTimeMake(1, profile.frameRate), (__bridge CFDictionaryRef)options,
            reinterpret_cast<void *>(std::uintptr_t(index + 1)), nullptr) == noErr);
        CVPixelBufferRelease(pixel);
    }
    assert(VTCompressionSessionCompleteFrames(session, kCMTimeInvalid) == noErr);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    std::lock_guard lock(output.mutex);
    assert(!output.failed && output.received[0] && output.received[1]);
    const auto bounds = captureCodecBounds(profile);
    nlohmann::json result{{"name", name}, {"width", width}, {"height", height},
        {"configured_frame_rate", profile.frameRate}, {"hardware", true}, {"frames", nlohmann::json::array()}};
    for (unsigned index = 0; index < 2; ++index) {
        const auto &bytes = output.frames[index].annexB;
        assert(bytes.size() > 8 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1);
        std::size_t end = 4;
        while (end + 4 <= bytes.size() && !(bytes[end] == 0 && bytes[end + 1] == 0 && bytes[end + 2] == 0 && bytes[end + 3] == 1))
            ++end;
        const auto sps = validatedSps(bytes.data() + 4, end - 4, *bounds);
        assert(sps && sps->width == width && sps->height == height && sps->level == expectedLevel);
        const auto filename = std::string(name) + "-" + std::to_string(index) + ".h264";
        if (const char *directory = std::getenv("DOCKPIPE_SYNTHETIC_CAPTURE_OUTPUT"))
            save(std::string(directory) + "/" + filename, bytes);
        result["frames"].push_back({{"file", filename}, {"bytes", bytes.size()}, {"level", sps->level}, {"index", index}});
        std::cout << "SYNTHETIC " << name << " index=" << index << " width=" << width << " height=" << height
                  << " configured_fps=" << profile.frameRate << " level=" << sps->level
                  << " bytes=" << bytes.size() << " hardware=true\n";
    }
    return result;
}
}

int main()
{
    @autoreleasepool {
        const auto display = CGMainDisplayID();
        const auto logical = CGDisplayBounds(display);
        nlohmann::json report{{"schema", "dockpipe.desktop.synthetic-hardware/v1"},
            {"source", "original generated NV12 luma bars; no captured screen pixels"},
            {"primary_display", {{"physical_width", CGDisplayPixelsWide(display)}, {"physical_height", CGDisplayPixelsHigh(display)},
                {"logical", {{"x", logical.origin.x}, {"y", logical.origin.y}, {"width", logical.size.width}, {"height", logical.size.height}}}}},
            {"cases", nlohmann::json::array()}};
        report["cases"].push_back(run("1080p60", {CaptureQuality::High, 60}, 1920, 1080, 42));
        report["cases"].push_back(run("4k30", {CaptureQuality::Ultra, 30}, 3840, 2160, 51));
        report["cases"].push_back(run("4k60", {CaptureQuality::Ultra, 60}, 3840, 2160, 52));
        if (const char *directory = std::getenv("DOCKPIPE_SYNTHETIC_CAPTURE_OUTPUT")) {
            const auto text = report.dump(2) + "\n";
            save(std::string(directory) + "/hardware-qualification.json", std::vector<std::uint8_t>(text.begin(), text.end()));
        }
    }
    std::cout << "PASS required-hardware synthetic1080p60/4K30/4K60, actual SPS/dimensions and2MiB cap; configured rates are not throughput measurements\n";
}
