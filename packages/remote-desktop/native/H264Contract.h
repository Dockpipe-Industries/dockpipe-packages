#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dockpipe::desktop {

struct H264ProfileBounds {
    unsigned maximumWidth = 1920;
    unsigned maximumHeight = 1080;
    unsigned maximumLevel = 40;
};

struct H264SequenceParameters {
    unsigned width;
    unsigned height;
    unsigned level;
    unsigned referenceFrames;
};

namespace h264_detail {
struct LevelLimits {
    unsigned level;
    unsigned frameMacroblocks;
    unsigned dpbMacroblocks;
};

// ITU-T H.264 Annex A, Table A-1. Frame/DPB limits are measured in
// macroblocks; cropping cannot shrink the allocation admitted by these limits.
inline std::optional<LevelLimits> levelLimits(unsigned level, bool level1b)
{
    // Existing original VideoToolbox64x64 fixture emits raw level_idc1
    // (42c001). Preserve it only under the conservative Level1.0 budget.
    if (level == 1)
        return LevelLimits{1, 99, 396};
    if (level1b)
        return level == 11 ? std::optional<LevelLimits>({11, 99, 396}) : std::nullopt;
    constexpr LevelLimits levels[]{
        {10, 99, 396}, {11, 396, 900}, {12, 396, 2376}, {13, 396, 2376},
        {20, 396, 2376}, {21, 792, 4752}, {22, 1620, 8100}, {30, 1620, 8100},
        {31, 3600, 18000}, {32, 5120, 20480}, {40, 8192, 32768}, {41, 8192, 32768},
        {42, 8704, 34816}, {50, 22080, 110400}, {51, 36864, 184320}, {52, 36864, 184320},
    };
    for (const auto &entry : levels) {
        if (entry.level == level)
            return entry;
    }
    return std::nullopt;
}

class Bits final {
public:
    explicit Bits(const std::vector<std::uint8_t> &bytes) : m_bytes(bytes) {}
    bool read(unsigned count, std::uint32_t &value)
    {
        if (count > 32 || count > m_bytes.size() * 8 - m_offset)
            return false;
        value = 0;
        for (unsigned index = 0; index < count; ++index) {
            value = (value << 1) | ((m_bytes[m_offset / 8] >> (7 - m_offset % 8)) & 1U);
            ++m_offset;
        }
        return true;
    }
    bool skip(unsigned count)
    {
        std::uint32_t ignored;
        return read(count, ignored);
    }
    bool ue(std::uint32_t &value)
    {
        unsigned zeros = 0;
        std::uint32_t bit = 0;
        while (read(1, bit) && bit == 0) {
            if (++zeros > 31)
                return false;
        }
        if (bit != 1)
            return false;
        std::uint32_t suffix;
        if (!read(zeros, suffix))
            return false;
        value = static_cast<std::uint32_t>(((std::uint64_t(1) << zeros) - 1) + suffix);
        return true;
    }
    bool boundedUe(std::uint32_t maximum, std::uint32_t &value)
    {
        return ue(value) && value <= maximum;
    }
    bool trailing()
    {
        std::uint32_t bit;
        if (!read(1, bit) || bit != 1)
            return false;
        while (m_offset % 8 != 0) {
            if (!read(1, bit) || bit != 0)
                return false;
        }
        return m_offset == m_bytes.size() * 8;
    }
private:
    const std::vector<std::uint8_t> &m_bytes;
    std::size_t m_offset = 0;
};

inline bool hrd(Bits &bits)
{
    std::uint32_t count;
    if (!bits.boundedUe(31, count) || !bits.skip(8))
        return false;
    for (std::uint32_t index = 0; index <= count; ++index) {
        std::uint32_t ignored;
        if (!bits.ue(ignored) || !bits.ue(ignored) || !bits.skip(1))
            return false;
    }
    return bits.skip(20);
}

inline bool vui(Bits &bits, unsigned referenceFrames, unsigned maximumDpbFrames)
{
    std::uint32_t present;
    std::uint32_t value;
    if (!bits.read(1, present))
        return false;
    if (present) {
        if (!bits.read(8, value) || (value > 16 && value != 255))
            return false;
        if (value == 255) {
            std::uint32_t width;
            std::uint32_t height;
            if (!bits.read(16, width) || !bits.read(16, height) || !width || !height)
                return false;
        }
    }
    if (!bits.read(1, present) || (present && !bits.skip(1)))
        return false;
    if (!bits.read(1, present))
        return false;
    if (present) {
        if (!bits.read(3, value) || value > 5 || !bits.skip(1) || !bits.read(1, value)
            || (value && !bits.skip(24)))
            return false;
    }
    if (!bits.read(1, present))
        return false;
    if (present && (!bits.boundedUe(5, value) || !bits.boundedUe(5, value)))
        return false;
    if (!bits.read(1, present))
        return false;
    if (present) {
        std::uint32_t units;
        std::uint32_t scale;
        if (!bits.read(32, units) || !bits.read(32, scale) || !units || !scale || !bits.skip(1))
            return false;
    }
    std::uint32_t nalHrd;
    std::uint32_t vclHrd;
    if (!bits.read(1, nalHrd) || (nalHrd && !hrd(bits))
        || !bits.read(1, vclHrd) || (vclHrd && !hrd(bits)))
        return false;
    if ((nalHrd || vclHrd) && !bits.skip(1))
        return false;
    if (!bits.skip(1) || !bits.read(1, present))
        return false;
    if (present) {
        std::uint32_t reorder;
        std::uint32_t buffers;
        if (!bits.skip(1) || !bits.boundedUe(16, value) || !bits.boundedUe(16, value)
            || !bits.boundedUe(16, value) || !bits.boundedUe(16, value)
            || !bits.boundedUe(maximumDpbFrames, reorder) || !bits.boundedUe(maximumDpbFrames, buffers)
            || buffers < referenceFrames || reorder > buffers)
            return false;
    }
    return true;
}
} // namespace h264_detail

// A complete progressive constrained-baseline 4:2:0 SPS, including bounded VUI
// resource fields and RBSP termination. The input is one raw NAL, not Annex B.
inline std::optional<H264SequenceParameters> validatedSps(const std::uint8_t *nal, std::size_t size,
    H264ProfileBounds bounds = {})
{
    using namespace h264_detail;
    if (!nal || size < 5 || size > 65536 || (nal[0] & 0x80) || (nal[0] & 0x60) == 0 || (nal[0] & 0x1f) != 7
        || bounds.maximumWidth < 2 || bounds.maximumWidth > 3840 || bounds.maximumHeight < 2 || bounds.maximumHeight > 2160
        || !levelLimits(bounds.maximumLevel, false))
        return std::nullopt;
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(size - 1);
    unsigned zeros = 0;
    for (std::size_t index = 1; index < size; ++index) {
        const auto value = nal[index];
        if (zeros >= 2) {
            if (value == 3) {
                if (index + 1 == size || nal[index + 1] > 3)
                    return std::nullopt;
                zeros = 0;
                continue;
            }
            if (value < 3)
                return std::nullopt;
        }
        rbsp.push_back(value);
        zeros = value == 0 ? zeros + 1 : 0;
    }
    Bits bits(rbsp);
    std::uint32_t profile;
    std::uint32_t constraints;
    std::uint32_t level;
    std::uint32_t value;
    if (!bits.read(8, profile) || profile != 66 || !bits.read(8, constraints) || (constraints & 0x4f) != 0x40
        || !bits.read(8, level) || level > bounds.maximumLevel)
        return std::nullopt;
    const auto limits = levelLimits(level, level == 11 && (constraints & 0x10));
    if (!limits || !bits.boundedUe(31, value) || !bits.boundedUe(12, value) || !bits.boundedUe(2, value))
        return std::nullopt;
    const auto poc = value;
    if (poc == 0 && !bits.boundedUe(12, value))
        return std::nullopt;
    if (poc == 1) {
        if (!bits.skip(1) || !bits.ue(value) || !bits.ue(value) || !bits.boundedUe(255, value))
            return std::nullopt;
        const auto offsets = value;
        for (std::uint32_t index = 0; index < offsets; ++index) {
            if (!bits.ue(value))
                return std::nullopt;
        }
    }
    std::uint32_t references;
    std::uint32_t widthMinusOne;
    std::uint32_t heightMinusOne;
    if (!bits.boundedUe(16, references) || !bits.skip(1)
        || !bits.boundedUe((bounds.maximumWidth + 15) / 16 - 1, widthMinusOne)
        || !bits.boundedUe((bounds.maximumHeight + 15) / 16 - 1, heightMinusOne)
        || !bits.read(1, value) || value != 1 || !bits.skip(1))
        return std::nullopt;
    const auto widthMbs = widthMinusOne + 1;
    const auto heightMbs = heightMinusOne + 1;
    const auto macroblocks = widthMbs * heightMbs;
    if (macroblocks > limits->frameMacroblocks || widthMbs * widthMbs > limits->frameMacroblocks * 8
        || heightMbs * heightMbs > limits->frameMacroblocks * 8)
        return std::nullopt;
    const auto dpbFrames = std::min(16U, limits->dpbMacroblocks / macroblocks);
    if (references > dpbFrames || !bits.read(1, value))
        return std::nullopt;
    std::uint32_t left = 0;
    std::uint32_t right = 0;
    std::uint32_t top = 0;
    std::uint32_t bottom = 0;
    if (value && (!bits.ue(left) || !bits.ue(right) || !bits.ue(top) || !bits.ue(bottom)))
        return std::nullopt;
    const std::uint64_t cropWidth = 2 * (std::uint64_t(left) + right);
    const std::uint64_t cropHeight = 2 * (std::uint64_t(top) + bottom);
    if (cropWidth >= widthMbs * 16 || cropHeight >= heightMbs * 16)
        return std::nullopt;
    const auto width = static_cast<unsigned>(widthMbs * 16 - cropWidth);
    const auto height = static_cast<unsigned>(heightMbs * 16 - cropHeight);
    if (width > bounds.maximumWidth || height > bounds.maximumHeight || !bits.read(1, value)
        || (value && !vui(bits, references, dpbFrames)) || !bits.trailing())
        return std::nullopt;
    return H264SequenceParameters{width, height, level, references};
}

} // namespace dockpipe::desktop
