#pragma once

#include "NativeDesktopBackend.h"

namespace dockpipe::desktop {
// Called only on the worker's main thread, with a slice capped by its caller.
void pumpMainRunLoop(Clock::time_point deadline);
} // namespace dockpipe::desktop
