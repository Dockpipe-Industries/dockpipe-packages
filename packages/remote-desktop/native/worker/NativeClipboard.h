#pragma once
#include "WorkerClipboard.h"
#include <memory>

namespace dockpipe::desktop {
// Construction does not read or modify the pasteboard.
std::unique_ptr<ClipboardAdapter> makeNativeClipboard();
} // namespace dockpipe::desktop
