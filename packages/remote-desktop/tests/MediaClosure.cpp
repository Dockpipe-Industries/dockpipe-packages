extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

// A load fixture, not capture or decode qualification. RTLD_NOW resolves every
// native import before this verifies the selected ABI and decoder availability.
extern "C" int desktop_media_probe()
{
    const bool versions = avcodec_version() == AV_VERSION_INT(61, 19, 101)
        && avutil_version() == AV_VERSION_INT(59, 39, 100)
        && swscale_version() == AV_VERSION_INT(8, 3, 100);
    return versions && avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr;
}
