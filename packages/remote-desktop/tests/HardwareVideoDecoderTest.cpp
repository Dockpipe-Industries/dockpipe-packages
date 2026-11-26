#include "HardwareVideoDecoder.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace dockpipe::desktop;
namespace {
template<typename Action> void rejects(Action action)
{
    bool failed = false;
    try {
        action();
    } catch (const std::exception &) {
        failed = true;
    }
    assert(failed);
}
}
int main()
{
    using Preference = HardwareVideoDecoder::Preference;
    rejects([] { HardwareVideoDecoder invalid(Preference::Software, {3840, 2160, 8294401}); });
    HardwareVideoDecoder software(Preference::Software, {1920, 1080, 1920ULL * 1080});
    assert(software.backend() == "pending");
    assert(!software.permitsGeometry(1920, 1081));
    assert(!software.permitsGeometry(0, 1080));
    assert(!software.permitsGeometry(2147483647, 2147483647));
    const auto *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *context = avcodec_alloc_context3(codec);
    assert(context);
    context->opaque = &software;
    context->thread_count = 2;
    context->width = 1920;
    context->height = 1080;
    context->coded_width = 1920;
    context->coded_height = 1088;
    const auto originalCallback = context->get_format;
    software.configure(context, codec);
    assert(context->opaque == &software && context->get_format == originalCallback);
    assert(context->thread_count == 2 && !context->hw_device_ctx && !software.hardwareSelected());
    const AVPixelFormat offered[]{AV_PIX_FMT_CUDA, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    assert(software.selectFormat(context, offered) == AV_PIX_FMT_YUV420P);
    context->height = 1081;
    assert(software.selectFormat(context, offered) == AV_PIX_FMT_NONE);
    context->height = 1080;
    AVFrame *frame = av_frame_alloc();
    assert(frame);
    frame->width = 1920;
    frame->height = 1080;
    frame->format = AV_PIX_FMT_YUV420P;
    assert(software.materialize(frame) == frame && software.backend() == "software");
    HardwareVideoDecoder forced(Preference::Hardware, {1920, 1080, 1920ULL * 1080});
    rejects([&] { forced.materialize(frame); });
    rejects([&] { forced.useSoftwareFallback(); });
    HardwareVideoDecoder automatic(Preference::Automatic, {1920, 1080, 1920ULL * 1080});
    automatic.useSoftwareFallback();
    assert(automatic.backend() == "pending" && automatic.fallbackReason() == "hardware-decode-failed");
    AVCodecContext *fallback = avcodec_alloc_context3(codec);
    automatic.configure(fallback, codec);
    assert(!fallback->hw_device_ctx && !automatic.hardwareSelected());
    automatic.materialize(frame);
    assert(automatic.backend() == "software");
    frame->width = 3840;
    frame->height = 2160;
    rejects([&] { software.materialize(frame); });
    HardwareVideoDecoder ultra(Preference::Software, {3840, 2160, HardwareVideoDecoder::MaximumPixels});
    context->width = 3840;
    context->height = 2160;
    context->coded_width = 3840;
    context->coded_height = 2176;
    assert(ultra.selectFormat(context, offered) == AV_PIX_FMT_YUV420P);
    context->coded_height = 2304;
    assert(ultra.selectFormat(context, offered) == AV_PIX_FMT_NONE);
    av_frame_free(&frame);
    avcodec_free_context(&fallback);
    avcodec_free_context(&context);
    std::cout << "PASS software-no-device/opaque preservation, pending-vs-actual backend, forced-no-fallback, Auto-reported fallback, profile/coded/global bounds\n";
}
