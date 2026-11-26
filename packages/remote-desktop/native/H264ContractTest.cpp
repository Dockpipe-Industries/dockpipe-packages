#include "CaptureProfile.h"
#include "H264Contract.h"
#include "TestSps.h"
#include <cassert>
#include <iostream>
#include <random>

using namespace dockpipe::desktop;

int main()
{
    const CaptureQuality qualities[]{CaptureQuality::Low, CaptureQuality::Balanced, CaptureQuality::High, CaptureQuality::Ultra};
    for (auto quality : qualities) {
        for (unsigned rate : {10, 15, 24, 30, 60}) {
            const auto bounds = captureCodecBounds({quality, rate});
            assert(bounds);
            test::SpsFixture fixture;
            fixture.width = bounds->maximumWidth;
            fixture.height = bounds->maximumHeight;
            fixture.level = bounds->maximumLevel;
            fixture.bitstreamRestriction = true;
            auto bytes = fixture.nal();
            const auto parsed = validatedSps(bytes.data(), bytes.size(), *bounds);
            assert(parsed && parsed->width == fixture.width && parsed->height == fixture.height && parsed->level == fixture.level);
            const auto macroblocks = ((fixture.width + 15) / 16) * ((fixture.height + 15) / 16);
            const auto limits = h264_detail::levelLimits(fixture.level, false);
            const auto maximumReferences = std::min(16U, limits->dpbMacroblocks / macroblocks);
            fixture.references = maximumReferences;
            fixture.buffers = maximumReferences;
            bytes = fixture.nal();
            assert(validatedSps(bytes.data(), bytes.size(), *bounds));
            fixture.references = maximumReferences + 1;
            bytes = fixture.nal();
            assert(!validatedSps(bytes.data(), bytes.size(), *bounds));
            fixture.references = 1;
            fixture.buffers = maximumReferences + 1;
            bytes = fixture.nal();
            assert(!validatedSps(bytes.data(), bytes.size(), *bounds));
            fixture.buffers = 1;
            fixture.reorder = 2;
            bytes = fixture.nal();
            assert(!validatedSps(bytes.data(), bytes.size(), *bounds));
            fixture.reorder = 0;
            fixture.level = 53;
            bytes = fixture.nal();
            assert(!validatedSps(bytes.data(), bytes.size(), *bounds));
        }
    }
    const std::uint8_t originalHardwareSps[]{0x27, 0x42, 0xc0, 0x01, 0xab, 0x41, 0x04, 0xf0, 0x43, 0xa0};
    const auto original = validatedSps(originalHardwareSps, sizeof(originalHardwareSps));
    assert(original && original->width == 64 && original->height == 64 && original->level == 1);
    test::SpsFixture fixture;
    fixture.width = 1920;
    fixture.height = 1080;
    fixture.level = 42;
    auto bytes = fixture.nal();
    assert(!validatedSps(bytes.data(), bytes.size()));
    assert(validatedSps(bytes.data(), bytes.size(), {1920, 1080, 42}));
    fixture.width = 3840;
    fixture.height = 2160;
    fixture.level = 52;
    bytes = fixture.nal();
    assert(!validatedSps(bytes.data(), bytes.size(), {3840, 2160, 51}));
    assert(validatedSps(bytes.data(), bytes.size(), {3840, 2160, 52}));
    // Cropping cannot disguise a larger coded allocation under a smaller bound.
    fixture.extraRightCrop = 960;
    fixture.extraBottomCrop = 540;
    bytes = fixture.nal();
    assert(!validatedSps(bytes.data(), bytes.size(), {1920, 1080, 52}));
    for (unsigned profile : {77, 100, 110}) {
        fixture = {};
        fixture.profile = profile;
        bytes = fixture.nal();
        assert(!validatedSps(bytes.data(), bytes.size()));
    }
    fixture = {};
    fixture.progressive = false;
    bytes = fixture.nal();
    assert(!validatedSps(bytes.data(), bytes.size()));
    fixture = {};
    fixture.constraints = 0x80;
    bytes = fixture.nal();
    assert(!validatedSps(bytes.data(), bytes.size()));
    fixture = {};
    fixture.extraRightCrop = 32;
    bytes = fixture.nal();
    assert(!validatedSps(bytes.data(), bytes.size()));
    fixture = {};
    bytes = fixture.nal();
    for (std::size_t length = 0; length < bytes.size(); ++length)
        assert(!validatedSps(bytes.data(), length));
    for (unsigned header : {0x07, 0x87, 0x68}) {
        auto changed = bytes;
        changed[0] = header;
        assert(!validatedSps(changed.data(), changed.size()));
    }
    auto badEscape = bytes;
    badEscape.insert(badEscape.end(), {0, 0, 3, 4});
    assert(!validatedSps(badEscape.data(), badEscape.size()));
    auto oversized = bytes;
    oversized.resize(65537);
    assert(!validatedSps(oversized.data(), oversized.size()));
    assert(!validatedSps(nullptr, 10));
    // Seeded mutations exercise bounded parsing; sanitizer builds use this same fixture.
    std::mt19937 random(26452);
    for (unsigned iteration = 0; iteration < 5000; ++iteration) {
        auto changed = bytes;
        for (unsigned mutations = 0; mutations < 3; ++mutations)
            changed[random() % changed.size()] ^= std::uint8_t(1U << (random() % 8));
        if (const auto value = validatedSps(changed.data(), changed.size())) {
            assert(value->width <= 1920 && value->height <= 1080 && value->referenceFrames <= 16 && value->level <= 40);
        }
    }
    std::cout << "PASS20profile SPS ceilings, actual VT fixture, coded/crop bounds, level DPB/VUIrefs, truncation/escape rejection and seeded mutations\n";
}
