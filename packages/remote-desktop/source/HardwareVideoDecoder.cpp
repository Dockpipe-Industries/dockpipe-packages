#include "HardwareVideoDecoder.h"

extern "C" {
#include <libavutil/pixdesc.h>
}
#include <array>
#include <stdexcept>

namespace dockpipe::desktop {
struct HardwareVideoDecoder::Impl {
    Preference preference;
    Limits limits;
    AVBufferRef *device = nullptr;
    AVFrame *download = nullptr;
    AVPixelFormat hardwareFormat = AV_PIX_FMT_NONE;
    AVHWDeviceType deviceType = AV_HWDEVICE_TYPE_NONE;
    bool configured = false;
    bool forceSoftware = false;
    std::string actualBackend = "pending";
    std::string fallback;

    Impl(Preference selected, Limits bounds) : preference(selected), limits(bounds) {}
    ~Impl()
    {
        av_frame_free(&download);
        av_buffer_unref(&device);
    }
};

HardwareVideoDecoder::HardwareVideoDecoder(Preference preference, Limits limits)
    : m_impl(std::make_unique<Impl>(preference, limits))
{
    if (limits.maximumWidth < 1 || limits.maximumHeight < 1
        || limits.maximumWidth > 8192 || limits.maximumHeight > 8192
        || limits.maximumPixels < 1 || limits.maximumPixels > MaximumPixels)
        throw std::invalid_argument("Invalid bounded hardware decoder geometry.");
}
HardwareVideoDecoder::~HardwareVideoDecoder() = default;

bool HardwareVideoDecoder::permitsGeometry(int width, int height) const
{
    return width > 0 && height > 0 && width <= m_impl->limits.maximumWidth
        && height <= m_impl->limits.maximumHeight
        && std::uint64_t(width) * std::uint64_t(height) <= m_impl->limits.maximumPixels;
}

void HardwareVideoDecoder::configure(AVCodecContext *context, const AVCodec *codec)
{
    if (!context || !codec || codec->id != AV_CODEC_ID_H264 || m_impl->configured || context->hw_device_ctx)
        throw std::invalid_argument("Hardware decoder requires one fresh H264 codec context.");
    m_impl->configured = true;
    if (m_impl->preference == Preference::Software || m_impl->forceSoftware)
        return;
    constexpr std::array<AVHWDeviceType, 2> supported{AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VAAPI};
    for (const auto type : supported) {
        for (int index = 0; index < 64; ++index) {
            const auto *configuration = avcodec_get_hw_config(codec, index);
            if (!configuration)
                break;
            if (configuration->device_type != type
                || !(configuration->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                continue;
            AVBufferRef *device = nullptr;
            const int result = av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0);
            if (result < 0 || !device) {
                av_buffer_unref(&device);
                continue;
            }
            context->hw_device_ctx = av_buffer_ref(device);
            if (!context->hw_device_ctx) {
                av_buffer_unref(&device);
                throw std::runtime_error("Could not retain the hardware decoder device.");
            }
            m_impl->device = device;
            m_impl->deviceType = type;
            m_impl->hardwareFormat = configuration->pix_fmt;
            return;
        }
    }
    if (m_impl->preference == Preference::Hardware)
        throw std::runtime_error("Hardware H264 decoding is unavailable on this computer; select Software or Auto.");
    m_impl->fallback = "hardware-device-unavailable";
}

AVPixelFormat HardwareVideoDecoder::selectFormat(AVCodecContext *context, const AVPixelFormat *offered) noexcept
{
    if (!context || !offered || !permitsGeometry(context->width, context->height))
        return AV_PIX_FMT_NONE;
    // Bounded 128-pixel codec/device padding is allowed without widening the negotiated
    // visible bounds. The total coded allocation still obeys the global cap.
    const auto paddedWidth = (std::int64_t(m_impl->limits.maximumWidth) + 127) / 128 * 128;
    const auto paddedHeight = (std::int64_t(m_impl->limits.maximumHeight) + 127) / 128 * 128;
    if (context->coded_width < 1 || context->coded_height < 1
        || context->coded_width > paddedWidth || context->coded_height > paddedHeight
        || std::uint64_t(context->coded_width) * std::uint64_t(context->coded_height) > MaximumAlignedPixels)
        return AV_PIX_FMT_NONE;
    AVPixelFormat software = AV_PIX_FMT_NONE;
    for (int index = 0; index < 64 && offered[index] != AV_PIX_FMT_NONE; ++index) {
        const auto format = offered[index];
        if (hardwareSelected() && format == m_impl->hardwareFormat)
            return format;
        const auto *description = av_pix_fmt_desc_get(format);
        if (software == AV_PIX_FMT_NONE && description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL))
            software = format;
    }
    if (m_impl->preference == Preference::Hardware)
        return AV_PIX_FMT_NONE;
    return software;
}

const AVFrame *HardwareVideoDecoder::materialize(const AVFrame *decoded)
{
    if (!decoded || !permitsGeometry(decoded->width, decoded->height))
        throw std::runtime_error("Decoded hardware frame exceeds the negotiated geometry.");
    const auto *description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decoded->format));
    if (!description)
        throw std::runtime_error("Decoded video format is unavailable.");
    if (!(description->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        if (m_impl->preference == Preference::Hardware)
            throw std::runtime_error("Forced hardware decoding produced a software frame.");
        m_impl->actualBackend = "software";
        if (m_impl->preference == Preference::Automatic && hardwareSelected() && m_impl->fallback.empty())
            m_impl->fallback = "hardware-format-unavailable";
        return decoded;
    }
    validateHardwareFrame(decoded);
    if (!m_impl->download) {
        m_impl->download = av_frame_alloc();
        if (!m_impl->download)
            throw std::runtime_error("Could not allocate the bounded hardware transfer frame.");
    }
    av_frame_unref(m_impl->download);
    if (av_hwframe_transfer_data(m_impl->download, decoded, 0) < 0
        || !permitsGeometry(m_impl->download->width, m_impl->download->height)
        || m_impl->download->width != decoded->width || m_impl->download->height != decoded->height)
        throw std::runtime_error("Hardware H264 frame transfer failed.");
    const auto *transferred = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(m_impl->download->format));
    if (!transferred || (transferred->flags & AV_PIX_FMT_FLAG_HWACCEL))
        throw std::runtime_error("Hardware frame transfer did not produce CPU-readable pixels.");
    m_impl->actualBackend = m_impl->deviceType == AV_HWDEVICE_TYPE_CUDA ? "cuda" : "vaapi";
    return m_impl->download;
}

void HardwareVideoDecoder::validateHardwareFrame(const AVFrame *decoded) const
{
    if (!hardwareSelected() || decoded->format != m_impl->hardwareFormat || !decoded->hw_frames_ctx)
        throw std::runtime_error("Decoded hardware frame does not match the selected hardware device.");
    const auto *frames = reinterpret_cast<const AVHWFramesContext *>(decoded->hw_frames_ctx->data);
    if (!frames || !frames->device_ctx || frames->device_ctx->type != m_impl->deviceType || frames->width < decoded->width
        || frames->height < decoded->height
        || frames->width > (std::int64_t(m_impl->limits.maximumWidth) + 127) / 128 * 128
        || frames->height > (std::int64_t(m_impl->limits.maximumHeight) + 127) / 128 * 128
        || std::uint64_t(frames->width) * std::uint64_t(frames->height) > MaximumAlignedPixels)
        throw std::runtime_error("Hardware frame pool exceeds the bounded decoder allocation.");
}

NativeVideoFrame HardwareVideoDecoder::retainCudaFrame(const AVFrame *decoded)
{
    if (!decoded || decoded->format != AV_PIX_FMT_CUDA)
        return {};
    if (!permitsGeometry(decoded->width, decoded->height))
        throw std::runtime_error("CUDA presentation exceeds the negotiated geometry.");
    validateHardwareFrame(decoded);
    const auto *pool = reinterpret_cast<const AVHWFramesContext *>(decoded->hw_frames_ctx->data);
    if (pool->sw_format != AV_PIX_FMT_NV12)
        return {};
    if (!decoded->data[0] || !decoded->data[1] || decoded->linesize[0] < decoded->width
        || decoded->linesize[1] < decoded->width)
        throw std::runtime_error("CUDA presentation has invalid NV12 planes.");
    auto *retained = av_frame_clone(decoded);
    if (!retained)
        throw std::runtime_error("Could not retain a bounded CUDA presentation frame.");
    m_impl->actualBackend = "cuda";
    return NativeVideoFrame(retained, [](AVFrame *frame) { av_frame_free(&frame); });
}

bool HardwareVideoDecoder::hardwareSelected() const
{
    return m_impl->device != nullptr && !m_impl->forceSoftware;
}

const std::string &HardwareVideoDecoder::backend() const
{
    return m_impl->actualBackend;
}

const std::string &HardwareVideoDecoder::fallbackReason() const
{
    return m_impl->fallback;
}

void HardwareVideoDecoder::useSoftwareFallback()
{
    if (m_impl->preference != Preference::Automatic)
        throw std::runtime_error("Forced hardware decoding cannot fall back to software.");
    m_impl->forceSoftware = true;
    m_impl->configured = false;
    m_impl->fallback = "hardware-decode-failed";
    av_frame_free(&m_impl->download);
    av_buffer_unref(&m_impl->device);
    m_impl->hardwareFormat = AV_PIX_FMT_NONE;
    m_impl->deviceType = AV_HWDEVICE_TYPE_NONE;
}
}
