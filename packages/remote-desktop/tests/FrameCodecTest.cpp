#include "FrameCodec.h"
#include "../native/TestSps.h"
#include <cassert>
#include <functional>

using dockpipe::desktop::FrameCodec;
using dockpipe::desktop::H264ProfileBounds;
using dockpipe::desktop::VideoFrame;
namespace fixture = dockpipe::desktop::test;

namespace {
void rejects(const std::function<void()> &action)
{
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
}

int main()
{
    VideoFrame source{42, 30000, 64, 64, true, fixture::annexB()};
    const auto bytes = FrameCodec::encode(source);
    const auto decoded = FrameCodec::decode(bytes.data(), bytes.size());
    assert(decoded.sequence == source.sequence && decoded.microseconds == source.microseconds);
    assert(decoded.width == 64 && decoded.height == 64 && decoded.keyframe);
    assert(decoded.annexB == source.annexB);
    for (std::size_t length = 0; length <= FrameCodec::HeaderBytes; ++length)
        rejects([&] { FrameCodec::decode(bytes.data(), length); });
    for (const auto offset : {0, 1, 2, 3, 4, 5, 6, 7}) {
        auto invalid = bytes;
        invalid[offset] = 0xff;
        rejects([&] { FrameCodec::decode(invalid.data(), invalid.size()); });
    }
    source.width = 1921;
    rejects([&] { FrameCodec::encode(source); });
    source.width = 64;
    source.height = 0;
    rejects([&] { FrameCodec::encode(source); });
    source.height = 64;
    source.sequence = 0;
    rejects([&] { FrameCodec::encode(source); });
    source.sequence = 1;
    source.annexB = {1, 0, 0, 1, 0x65};
    rejects([&] { FrameCodec::encode(source); });
    source.annexB = fixture::annexB();
    source.annexB.resize(FrameCodec::MaximumBytes - FrameCodec::HeaderBytes, 0xff);
    assert(FrameCodec::encode(source).size() == FrameCodec::MaximumBytes);
    source.annexB.push_back(0);
    rejects([&] { FrameCodec::encode(source); });

    // A wider global ceiling cannot admit unnegotiated geometry or level.
    for (const auto bounds : {H264ProfileBounds{1920, 1080, 42}, {3840, 2160, 51}, {3840, 2160, 52}}) {
        source = {1, 0, bounds.maximumWidth, bounds.maximumHeight, true,
                  fixture::annexB(bounds.maximumWidth, bounds.maximumHeight, bounds.maximumLevel)};
        const auto record = FrameCodec::encode(source, bounds);
        assert(FrameCodec::decode(record.data(), record.size(), bounds).width == bounds.maximumWidth);
        rejects([&] { FrameCodec::decode(record.data(), record.size()); });
    }
    source = {1, 0, 64, 64, true, fixture::annexB(3840, 2160, 52)};
    rejects([&] { FrameCodec::encode(source, {3840, 2160, 52}); }); // Envelope lies about SPS.
    source = {1, 0, 64, 64, true, {0, 0, 0, 1, 0x65, 0x80}};
    rejects([&] { FrameCodec::encode(source); }); // No SPS on fresh IDR.
}
