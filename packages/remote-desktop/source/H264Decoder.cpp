#include "H264Decoder.h"
#include "DecoderAllocation.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <cstring>
#include <cerrno>
#include <limits>
#include <stdexcept>

namespace dockpipe::desktop {

struct H264Decoder::Impl {
    H264ProfileBounds bounds;
    Preference preference;
    HardwareVideoDecoder hardware;
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *conversion = nullptr;
    std::uint64_t sequence = 0;
    bool fallbackUsed = false;
    bool awaitKeyframe = false;

    Impl(H264ProfileBounds limits, Preference mode)
        : bounds(limits), preference(mode), hardware(mode,
            {static_cast<int>(limits.maximumWidth), static_cast<int>(limits.maximumHeight),
             std::uint64_t(limits.maximumWidth) * limits.maximumHeight})
    {
    }

    ~Impl() { releaseCodec(); }

    void releaseCodec()
    {
        sws_freeContext(conversion);
        conversion = nullptr;
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }

    static int getBuffer(AVCodecContext *codec, AVFrame *output, int flags)
    {
        const auto *self = static_cast<const Impl *>(codec->opaque);
        if (!self || !permitsCodedAllocation(output->width, output->height, self->bounds))
            return AVERROR(EINVAL);
        return avcodec_default_get_buffer2(codec, output, flags);
    }

    static AVPixelFormat getFormat(AVCodecContext *codec, const AVPixelFormat *formats)
    {
        auto *self = static_cast<Impl *>(codec->opaque);
        return self ? self->hardware.selectFormat(codec, formats) : AV_PIX_FMT_NONE;
    }

    void openCodec()
    {
        const auto *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec)
            throw std::runtime_error("The selected native H264 decoder is unavailable.");
        context = avcodec_alloc_context3(codec);
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!context || !frame || !packet)
            throw std::runtime_error("Could not allocate the native decoder.");
        context->thread_count = 2;
        context->thread_type = FF_THREAD_SLICE;
        context->max_pixels = std::int64_t(bounds.maximumWidth) * bounds.maximumHeight;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        context->pkt_timebase = AVRational{1, 1000000};
        context->opaque = this;
        context->get_buffer2 = &getBuffer;
        context->get_format = &getFormat;
        hardware.configure(context, codec);
        if (avcodec_open2(context, codec, nullptr) < 0) {
            if (preference == Preference::Hardware)
                throw std::runtime_error("Hardware H264 decoder initialization failed.");
            throw std::runtime_error("Could not initialize the native H264 decoder.");
        }
    }

    QImage decodePacket(const VideoFrame &input, const std::function<bool()> &shouldPresent, Timings *timings,
        const std::function<bool(NativeVideoFrame)> &nativeOutput)
    {
        using Clock = std::chrono::steady_clock;
        const auto submitStarted = timings ? Clock::now() : Clock::time_point{};
        av_packet_unref(packet);
        if (av_new_packet(packet, static_cast<int>(input.annexB.size())) < 0)
            throw std::runtime_error("Could not allocate a bounded video packet.");
        std::memcpy(packet->data, input.annexB.data(), input.annexB.size());
        packet->pts = static_cast<std::int64_t>(input.microseconds);
        packet->dts = packet->pts;
        if (avcodec_send_packet(context, packet) < 0)
            throw std::runtime_error("The native decoder rejected a video packet.");
        if (timings)
            timings->decode += Clock::now() - submitStarted;

        QImage latest;
        for (int count = 0; count < 4; ++count) {
            const auto receiveStarted = timings ? Clock::now() : Clock::time_point{};
            const int result = avcodec_receive_frame(context, frame);
            if (timings)
                timings->decode += Clock::now() - receiveStarted;
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return latest;
            if (result < 0)
                throw std::runtime_error("The native decoder could not recover a video frame.");
            if (context->profile != AV_PROFILE_H264_CONSTRAINED_BASELINE || context->level > static_cast<int>(bounds.maximumLevel))
                throw std::runtime_error("The native decoder received an unnegotiated H264 profile.");
            if (!hardware.permitsGeometry(frame->width, frame->height)
                || frame->width != static_cast<int>(input.width) || frame->height != static_cast<int>(input.height))
                throw std::runtime_error("Decoded geometry differs from the frozen desktop frame.");
            if (timings)
                ++timings->decodedFrames;
            if (shouldPresent && !shouldPresent()) {
                if (timings)
                    ++timings->skippedOutputs;
                av_frame_unref(frame);
                continue;
            }

            if (nativeOutput) {
                auto retained = hardware.retainCudaFrame(frame);
                if (retained && nativeOutput(std::move(retained))) {
                    av_frame_unref(frame);
                    continue;
                }
            }

            const bool needsDownload = timings && frame->hw_frames_ctx;
            const auto transferStarted = needsDownload ? Clock::now() : Clock::time_point{};
            const auto *pixels = hardware.materialize(frame);
            if (needsDownload) {
                timings->download += Clock::now() - transferStarted;
                ++timings->downloads;
            }
            if (!pixels || !hardware.permitsGeometry(pixels->width, pixels->height)
                || pixels->width != frame->width || pixels->height != frame->height)
                throw std::runtime_error("Native decoder transfer changed its bounded geometry.");
            const auto conversionStarted = timings ? Clock::now() : Clock::time_point{};
            latest = QImage(pixels->width, pixels->height, QImage::Format_RGBA8888);
            if (latest.isNull())
                throw std::runtime_error("Could not allocate the bounded desktop image.");
            conversion = sws_getCachedContext(conversion, pixels->width, pixels->height,
                static_cast<AVPixelFormat>(pixels->format), pixels->width, pixels->height, AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!conversion)
                throw std::runtime_error("The native video conversion is unavailable.");
            std::uint8_t *outputs[] = {latest.bits(), nullptr, nullptr, nullptr};
            const int strides[] = {static_cast<int>(latest.bytesPerLine()), 0, 0, 0};
            if (sws_scale(conversion, pixels->data, pixels->linesize, 0, pixels->height, outputs, strides) != pixels->height)
                throw std::runtime_error("Native desktop image conversion was incomplete.");
            if (timings) {
                timings->conversion += Clock::now() - conversionStarted;
                ++timings->conversions;
            }
            av_frame_unref(frame);
        }
        throw std::runtime_error("A desktop record produced too many decoded frames.");
    }
};

H264Decoder::H264Decoder(H264ProfileBounds bounds, Preference preference)
    : m_impl(std::make_unique<Impl>(bounds, preference))
{
    m_impl->openCodec();
}

H264Decoder::~H264Decoder() = default;

QImage H264Decoder::decode(const VideoFrame &input, const std::function<bool()> &shouldPresent,
    Timings *timings, const std::function<bool(NativeVideoFrame)> &nativeOutput)
{
    // This checks raw SPS geometry/profile/level before any software packet
    // submission or hardware get_format/frame-pool creation.
    FrameCodec::validate(input, m_impl->bounds);
    if (input.sequence <= m_impl->sequence
        || input.microseconds > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("Invalid or replayed native desktop frame.");
    if (m_impl->awaitKeyframe && input.keyframe) {
        // An IDR resets the H264 reference-picture buffer itself. The flag and
        // bounded SPS alone are not evidence that this access unit is an IDR.
        bool containsIdr = false;
        for (std::size_t index = 0; index + 3 < input.annexB.size(); ++index) {
            if (input.annexB[index] == 0 && input.annexB[index + 1] == 0
                && input.annexB[index + 2] == 1 && (input.annexB[index + 3] & 0x1f) == 5) {
                containsIdr = true;
                break;
            }
        }
        if (!containsIdr)
            throw std::invalid_argument("Native desktop recovery requires an actual H264 IDR.");
    }
    m_impl->sequence = input.sequence;
    if (m_impl->awaitKeyframe && !input.keyframe)
        return {};
    m_impl->awaitKeyframe = false;
    try {
        return m_impl->decodePacket(input, shouldPresent, timings, nativeOutput);
    } catch (const std::exception &) {
        if (m_impl->preference == Preference::Hardware)
            throw std::runtime_error("Hardware H264 decoding failed; reconnect using Software or Auto.");
        if (m_impl->preference != Preference::Automatic || m_impl->fallbackUsed || !m_impl->hardware.hardwareSelected())
            throw;
        // A local codec retry never replays transport/input. Drop dependent
        // P-frames until a fresh IDR if hardware failed between keyframes.
        m_impl->fallbackUsed = true;
        m_impl->hardware.useSoftwareFallback();
        m_impl->releaseCodec();
        m_impl->openCodec();
        if (!input.keyframe) {
            m_impl->awaitKeyframe = true;
            return {};
        }
        return m_impl->decodePacket(input, shouldPresent, timings, nativeOutput);
    }
}

void H264Decoder::resynchronize()
{
    // Complete access units and no B-frames are admitted. A validated fresh
    // IDR resets references without flushing/reinitializing the hardware pool.
    m_impl->awaitKeyframe = true;
    // Preserve sequence, profile bounds, hardware selection and fallback policy.
}

bool H264Decoder::requiresKeyframe() const { return m_impl->awaitKeyframe; }
const std::string &H264Decoder::backend() const { return m_impl->hardware.backend(); }
const std::string &H264Decoder::fallbackReason() const { return m_impl->hardware.fallbackReason(); }

} // namespace dockpipe::desktop
