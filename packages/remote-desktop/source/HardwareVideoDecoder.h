#pragma once
#include "NativeVideoFrame.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#include <cstdint>
#include <memory>
#include <string>

namespace dockpipe::desktop {
// One helper belongs to one decoder on its bounded media worker thread.
// It never owns packets, queues, callback opaque state or Qt image conversion.
class HardwareVideoDecoder final {
public:
    enum class Preference { Software, Automatic, Hardware };
    struct Limits {
        int maximumWidth;
        int maximumHeight;
        std::uint64_t maximumPixels;
    };
    static constexpr std::uint64_t MaximumPixels = 3840ULL * 2160;
    // Visible pixels stay capped above. Explicit finite pool padding does not
    // widen SPS/envelope admission; callers validate those before decode.
    static constexpr std::uint64_t MaximumAlignedPixels = 3840ULL * 2176;

    HardwareVideoDecoder(Preference preference, Limits limits);
    ~HardwareVideoDecoder();
    HardwareVideoDecoder(const HardwareVideoDecoder &) = delete;
    HardwareVideoDecoder &operator=(const HardwareVideoDecoder &) = delete;

    // Before avcodec_open2. Caller retains opaque/get_format/get_buffer2 and
    // forwards its get_format callback to selectFormat below. Software performs
    // no device work; Hardware throws if no supported device can be created.
    void configure(AVCodecContext *context, const AVCodec *codec);
    AVPixelFormat selectFormat(AVCodecContext *context, const AVPixelFormat *offered) noexcept;
    // After validated decode. HW downloads into this helper's bounded reusable
    // CPU frame; input ownership is unchanged. Actual backend is reported only
    // after successful transfer, never from device creation alone.
    const AVFrame *materialize(const AVFrame *decoded);
    NativeVideoFrame retainCudaFrame(const AVFrame *decoded);
    bool permitsGeometry(int width, int height) const;
    bool hardwareSelected() const;
    const std::string &backend() const;
    const std::string &fallbackReason() const;
    // Auto-only explicit transition, before caller recreates a software codec
    // and replays its current bounded packet. Hardware never silently falls back.
    void useSoftwareFallback();

private:
    void validateHardwareFrame(const AVFrame *decoded) const;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
