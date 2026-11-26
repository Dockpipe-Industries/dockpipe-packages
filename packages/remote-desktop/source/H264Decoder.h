#pragma once

#include "FrameCodec.h"
#include "HardwareVideoDecoder.h"
#include <QImage>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace dockpipe::desktop {

// One decoder belongs to one bounded media worker, never the Qt GUI thread.
class H264Decoder final {
public:
    using Preference = HardwareVideoDecoder::Preference;
    explicit H264Decoder(H264ProfileBounds bounds = {}, Preference preference = Preference::Software);
    ~H264Decoder();
    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;
    struct Timings {
        std::chrono::nanoseconds decode{};
        std::chrono::nanoseconds download{};
        std::chrono::nanoseconds conversion{};
        unsigned decodedFrames = 0;
        unsigned downloads = 0;
        unsigned conversions = 0;
        unsigned skippedOutputs = 0;
    };

    // The predicate is checked after each picture is decoded and validated.
    // Declining presentation releases that output and keeps codec references.
    QImage decode(const VideoFrame &frame, const std::function<bool()> &shouldPresent = {},
        Timings *timings = nullptr,
        const std::function<bool(NativeVideoFrame)> &nativeOutput = {});
    // After transport loss, await an actual IDR to reset reference pictures
    // without flushing or replacing a healthy hardware device/codec.
    void resynchronize();
    bool requiresKeyframe() const;
    const std::string &backend() const;
    const std::string &fallbackReason() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dockpipe::desktop
