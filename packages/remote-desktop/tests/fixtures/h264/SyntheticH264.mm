#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
struct Output {
    std::mutex mutex;
    std::array<std::vector<std::uint8_t>, 2> frames;
    bool failed = false;
    std::array<std::uint8_t, 3> profile{};
};
bool append(std::vector<std::uint8_t> &value, const std::uint8_t *data, std::size_t length)
{
    if (!data || !length || length > 65536 || value.size() + length + 4 > 65536)
        return false;
    value.insert(value.end(), {0, 0, 0, 1});
    value.insert(value.end(), data, data + length);
    return true;
}
void encoded(void *context, void *ticket, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
    auto &output = *static_cast<Output *>(context);
    std::lock_guard lock(output.mutex);
    const auto index = reinterpret_cast<std::uintptr_t>(ticket) - 1;
    if (status != noErr || !sample || flags & kVTEncodeInfo_FrameDropped || index >= output.frames.size()) {
        output.failed = true;
        return;
    }
    const auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments)) {
        const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        if (CFDictionaryGetValue(dictionary, kCMSampleAttachmentKey_NotSync) == kCFBooleanTrue) {
            output.failed = true;
            return;
        }
    }
    const auto format = CMSampleBufferGetFormatDescription(sample);
    const std::uint8_t *data = nullptr;
    std::size_t length = 0;
    std::size_t count = 0;
    int lengthBytes = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &data, &length, &count, &lengthBytes) != noErr
        || count != 2 || lengthBytes != 4 || length < 4 || data[1] != 0x42 || (data[2] & 0x4f) != 0x40 || data[3] > 40) {
        output.failed = true;
        return;
    }
    output.profile = {data[1], data[2], data[3]};
    auto &frame = output.frames[index];
    for (std::size_t parameter = 0; parameter < count; ++parameter) {
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameter, &data, &length, nullptr, nullptr) != noErr || !append(frame, data, length)) {
            output.failed = true;
            return;
        }
    }
    const auto block = CMSampleBufferGetDataBuffer(sample);
    const auto size = block ? CMBlockBufferGetDataLength(block) : 0;
    if (!size || size > 65536) {
        output.failed = true;
        return;
    }
    std::vector<std::uint8_t> avcc(size);
    if (CMBlockBufferCopyDataBytes(block, 0, size, avcc.data()) != noErr) {
        output.failed = true;
        return;
    }
    std::size_t offset = 0;
    while (offset < size) {
        if (size - offset < 4) {
            output.failed = true;
            return;
        }
        const auto nal = (std::size_t(avcc[offset]) << 24) | (std::size_t(avcc[offset + 1]) << 16) | (std::size_t(avcc[offset + 2]) << 8) | avcc[offset + 3];
        offset += 4;
        if (nal > size - offset || !append(frame, avcc.data() + offset, nal)) {
            output.failed = true;
            return;
        }
        offset += nal;
    }
}
bool save(const std::string &path, const std::vector<std::uint8_t> &bytes)
{
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0)
        return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            close(descriptor);
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return close(descriptor) == 0;
}
}
int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 2)
            return 2;
        Output output;
        VTCompressionSessionRef session = nullptr;
        NSDictionary *specification = @{(__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES};
        OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault, 64, 64, kCMVideoCodecType_H264,
            (__bridge CFDictionaryRef)specification, nullptr, nullptr, encoded, &output, &session);
        if (status != noErr || !session)
            return 3;
        NSDictionary *properties = @{
            (__bridge NSString *)kVTCompressionPropertyKey_ProfileLevel: (__bridge NSString *)kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel,
            (__bridge NSString *)kVTCompressionPropertyKey_RealTime: @YES,
            (__bridge NSString *)kVTCompressionPropertyKey_AllowFrameReordering: @NO,
            (__bridge NSString *)kVTCompressionPropertyKey_ExpectedFrameRate: @30,
            (__bridge NSString *)kVTCompressionPropertyKey_AverageBitRate: @100000,
        };
        status = VTSessionSetProperties(session, (__bridge CFDictionaryRef)properties);
        if (status == noErr)
            status = VTCompressionSessionPrepareToEncodeFrames(session);
        CFTypeRef hardware = nullptr;
        if (status == noErr)
            status = VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, kCFAllocatorDefault, &hardware);
        const bool accelerated = hardware == kCFBooleanTrue;
        if (hardware)
            CFRelease(hardware);
        if (status == noErr && accelerated) {
            constexpr std::uint8_t bars[4][3]{{81, 90, 240}, {145, 54, 34}, {41, 240, 110}, {235, 128, 128}};
            for (unsigned index = 0; index < 2 && status == noErr; ++index) {
                CVPixelBufferRef pixel = nullptr;
                NSDictionary *attributes = @{(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{}};
                status = CVPixelBufferCreate(kCFAllocatorDefault, 64, 64, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, (__bridge CFDictionaryRef)attributes, &pixel);
                if (status != noErr || !pixel)
                    break;
                CVPixelBufferLockBaseAddress(pixel, 0);
                auto *luma = static_cast<std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel, 0));
                auto *chroma = static_cast<std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel, 1));
                for (unsigned row = 0; row < 64; ++row) {
                    for (unsigned column = 0; column < 64; ++column) {
                        const auto bar = index ? 3 - column / 16 : column / 16;
                        luma[row * CVPixelBufferGetBytesPerRowOfPlane(pixel, 0) + column] = bars[bar][0];
                    }
                }
                for (unsigned row = 0; row < 32; ++row) {
                    for (unsigned column = 0; column < 32; ++column) {
                        const auto bar = index ? 3 - column / 8 : column / 8;
                        const auto position = row * CVPixelBufferGetBytesPerRowOfPlane(pixel, 1) + 2 * column;
                        chroma[position] = bars[bar][1];
                        chroma[position + 1] = bars[bar][2];
                    }
                }
                CVPixelBufferUnlockBaseAddress(pixel, 0);
                NSDictionary *force = @{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES};
                status = VTCompressionSessionEncodeFrame(session, pixel, CMTimeMake(index, 30), CMTimeMake(1, 30), (__bridge CFDictionaryRef)force,
                    reinterpret_cast<void *>(std::uintptr_t(index + 1)), nullptr);
                CFRelease(pixel);
            }
        } else {
            output.failed = true;
        }
        if (status == noErr)
            status = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        if (status != noErr || output.failed || output.frames[0].empty() || output.frames[1].empty())
            return 4;
        for (unsigned index = 0; index < 2; ++index) {
            if (!save(std::string(argv[1]) + "-" + std::to_string(index) + ".h264", output.frames[index]))
                return 5;
        }
        printf("{\"width\":64,\"height\":64,\"frames\":2,\"idr\":true,\"hardware\":true,\"profile_level_id\":\"%02x%02x%02x\",\"source\":\"original synthetic four color bars; reversed second frame\"}\n", output.profile[0], output.profile[1], output.profile[2]);
    }
}
