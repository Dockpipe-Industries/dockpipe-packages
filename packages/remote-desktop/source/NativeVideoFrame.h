#pragma once

extern "C" {
#include <libavutil/frame.h>
}
#include <memory>

namespace dockpipe::desktop {
using NativeVideoFrame = std::shared_ptr<AVFrame>;
}
