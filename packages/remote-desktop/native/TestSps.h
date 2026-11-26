#pragma once

#include <cstdint>
#include <vector>

namespace dockpipe::desktop::test {

class SpsBits final {
public:
    void bits(std::uint32_t value, unsigned count)
    {
        for (unsigned index = count; index > 0; --index) {
            if (m_count % 8 == 0)
                m_bytes.push_back(0);
            m_bytes.back() |= ((value >> (index - 1)) & 1U) << (7 - m_count % 8);
            ++m_count;
        }
    }
    void ue(std::uint32_t value)
    {
        const std::uint64_t code = std::uint64_t(value) + 1;
        unsigned count = 0;
        for (auto current = code; current; current >>= 1)
            ++count;
        bits(0, count - 1);
        bits(static_cast<std::uint32_t>(code), count);
    }
    std::vector<std::uint8_t> finish()
    {
        bits(1, 1);
        while (m_count % 8)
            bits(0, 1);
        std::vector<std::uint8_t> output{0x67};
        unsigned zeros = 0;
        for (auto byte : m_bytes) {
            if (zeros >= 2 && byte <= 3) {
                output.push_back(3);
                zeros = 0;
            }
            output.push_back(byte);
            zeros = byte == 0 ? zeros + 1 : 0;
        }
        return output;
    }
private:
    std::vector<std::uint8_t> m_bytes;
    unsigned m_count = 0;
};

struct SpsFixture {
    unsigned width = 64;
    unsigned height = 64;
    unsigned level = 40;
    unsigned profile = 66;
    unsigned constraints = 0xc0;
    unsigned references = 1;
    bool progressive = true;
    unsigned extraRightCrop = 0;
    unsigned extraBottomCrop = 0;
    bool bitstreamRestriction = false;
    unsigned buffers = 1;
    unsigned reorder = 0;

    std::vector<std::uint8_t> nal() const
    {
        SpsBits bits;
        bits.bits(profile, 8);
        bits.bits(constraints, 8);
        bits.bits(level, 8);
        bits.ue(0); // SPS ID.
        bits.ue(0); // log2_max_frame_num_minus4.
        bits.ue(0); // pic_order_cnt_type.
        bits.ue(0); // log2_max_pic_order_cnt_lsb_minus4.
        bits.ue(references);
        bits.bits(0, 1); // gaps_in_frame_num_value_allowed_flag.
        bits.ue((width + 15) / 16 - 1);
        bits.ue((height + 15) / 16 - 1);
        bits.bits(progressive, 1);
        if (!progressive)
            bits.bits(0, 1);
        bits.bits(1, 1); // direct_8x8_inference_flag.
        const auto right = ((width + 15) / 16 * 16 - width) / 2 + extraRightCrop;
        const auto bottom = ((height + 15) / 16 * 16 - height) / 2 + extraBottomCrop;
        bits.bits(right || bottom, 1);
        if (right || bottom) {
            bits.ue(0);
            bits.ue(right);
            bits.ue(0);
            bits.ue(bottom);
        }
        bits.bits(bitstreamRestriction, 1); // vui_parameters_present_flag.
        if (bitstreamRestriction) {
            bits.bits(0, 8); // No aspect/overscan/video/chroma/timing/HRD/pic_struct.
            bits.bits(1, 1); // bitstream_restriction_flag.
            bits.bits(1, 1);
            bits.ue(2);
            bits.ue(1);
            bits.ue(16);
            bits.ue(16);
            bits.ue(reorder);
            bits.ue(buffers);
        }
        return bits.finish();
    }
};

inline std::vector<std::uint8_t> annexB(unsigned width = 64, unsigned height = 64, unsigned level = 40)
{
    SpsFixture fixture;
    fixture.width = width;
    fixture.height = height;
    fixture.level = level;
    auto sps = fixture.nal();
    std::vector<std::uint8_t> output{0, 0, 0, 1};
    output.insert(output.end(), sps.begin(), sps.end());
    output.insert(output.end(), {0, 0, 0, 1, 0x68, 0xce, 0x06, 0xe2, 0, 0, 0, 1, 0x65, 0x88});
    return output;
}

} // namespace dockpipe::desktop::test
