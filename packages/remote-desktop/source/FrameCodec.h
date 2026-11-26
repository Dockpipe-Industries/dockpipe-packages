#pragma once

#include "../native/H264Contract.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace dockpipe::desktop {

struct VideoFrame {
    std::uint64_t sequence = 0;
    std::uint64_t microseconds = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool keyframe = false;
    std::vector<std::uint8_t> annexB;
};

class FrameCodec final {
public:
    static constexpr std::size_t HeaderBytes = 32;
    static constexpr std::size_t MaximumBytes = 2 * 1024 * 1024;

    static std::vector<std::uint8_t> encode(const VideoFrame &frame, H264ProfileBounds bounds = {})
    {
        validate(frame, bounds);
        std::vector<std::uint8_t> output(HeaderBytes + frame.annexB.size(), 0);
        output[0] = 'D';
        output[1] = 'N';
        output[2] = 'P';
        output[3] = 1;
        output[4] = frame.keyframe ? 1 : 0;
        write(output.data() + 8, frame.width, 4);
        write(output.data() + 12, frame.height, 4);
        write(output.data() + 16, frame.microseconds, 8);
        write(output.data() + 24, frame.sequence, 8);
        std::copy(frame.annexB.begin(), frame.annexB.end(), output.begin() + HeaderBytes);
        return output;
    }

    static VideoFrame decode(const std::uint8_t *data, std::size_t length, H264ProfileBounds bounds = {})
    {
        if (!data || length <= HeaderBytes || length > MaximumBytes
            || data[0] != 'D' || data[1] != 'N' || data[2] != 'P' || data[3] != 1
            || data[4] > 1 || data[5] || data[6] || data[7])
            throw std::invalid_argument("Invalid native desktop frame envelope.");
        VideoFrame frame;
        frame.keyframe = data[4] == 1;
        frame.width = static_cast<std::uint32_t>(read(data + 8, 4));
        frame.height = static_cast<std::uint32_t>(read(data + 12, 4));
        frame.microseconds = read(data + 16, 8);
        frame.sequence = read(data + 24, 8);
        // Reject unnegotiated geometry/SPS before copying any encoded payload,
        // and before a CPU or hardware decoder can allocate its frame pool.
        validateGeometry(frame, bounds);
        validateAnnexB(data + HeaderBytes, length - HeaderBytes, frame, bounds);
        frame.annexB.assign(data + HeaderBytes, data + length);
        return frame;
    }

    static void validate(const VideoFrame &frame, H264ProfileBounds bounds = {})
    {
        validateGeometry(frame, bounds);
        validateAnnexB(frame.annexB.data(), frame.annexB.size(), frame, bounds);
    }

private:
    static void validateGeometry(const VideoFrame &frame, H264ProfileBounds bounds)
    {
        const bool level = bounds.maximumLevel == 40 || bounds.maximumLevel == 42
            || bounds.maximumLevel == 51 || bounds.maximumLevel == 52;
        if (!level || !bounds.maximumWidth || !bounds.maximumHeight
            || bounds.maximumWidth > 3840 || bounds.maximumHeight > 2160
            || ((bounds.maximumWidth > 1920 || bounds.maximumHeight > 1080) && bounds.maximumLevel < 51)
            || !frame.sequence || !frame.width || !frame.height || frame.width % 2 || frame.height % 2
            || frame.width > bounds.maximumWidth || frame.height > bounds.maximumHeight)
            throw std::invalid_argument("Native desktop geometry exceeds its frozen profile.");
    }

    static std::size_t startCode(const std::uint8_t *data, std::size_t length, std::size_t position)
    {
        if (position + 3 > length || data[position] || data[position + 1])
            return 0;
        if (data[position + 2] == 1)
            return 3;
        if (position + 4 <= length && !data[position + 2] && data[position + 3] == 1)
            return 4;
        return 0;
    }

    static void validateAnnexB(const std::uint8_t *data, std::size_t length,
        const VideoFrame &frame, H264ProfileBounds bounds)
    {
        if (!data || length < 5 || length > MaximumBytes - HeaderBytes || !startCode(data, length, 0))
            throw std::invalid_argument("Invalid bounded native H264 record.");
        std::size_t position = 0;
        unsigned count = 0;
        bool sawSps = false;
        while (position < length) {
            if (++count > 512)
                throw std::invalid_argument("Too many native H264 NAL records.");
            const auto prefix = startCode(data, length, position);
            if (!prefix || position + prefix >= length)
                throw std::invalid_argument("Truncated native H264 NAL record.");
            const auto begin = position + prefix;
            std::size_t end = begin + 1;
            while (end < length && !startCode(data, length, end))
                ++end;
            position = end;
            while (end > begin && data[end - 1] == 0)
                --end;
            if (end <= begin || data[begin] & 0x80)
                throw std::invalid_argument("Invalid native H264 NAL header.");
            const auto type = data[begin] & 0x1f;
            if (!type || type > 12 || (type >= 2 && type <= 4))
                throw std::invalid_argument("Unsupported native H264 NAL type.");
            if (type == 7) {
                const auto parameters = validatedSps(data + begin, end - begin, bounds);
                if (!parameters || parameters->width != frame.width || parameters->height != frame.height)
                    throw std::invalid_argument("Native H264 SPS exceeds the frozen profile or envelope.");
                sawSps = true;
            }
        }
        if (frame.keyframe && !sawSps)
            throw std::invalid_argument("A native H264 keyframe requires bounded sequence parameters.");
    }

    static void write(std::uint8_t *output, std::uint64_t value, int bytes)
    {
        for (int index = bytes - 1; index >= 0; --index) {
            output[index] = static_cast<std::uint8_t>(value & 0xff);
            value >>= 8;
        }
    }

    static std::uint64_t read(const std::uint8_t *input, int bytes)
    {
        std::uint64_t value = 0;
        for (int index = 0; index < bytes; ++index)
            value = (value << 8) | input[index];
        return value;
    }
};

} // namespace dockpipe::desktop
