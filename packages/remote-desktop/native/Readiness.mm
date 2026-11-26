#include "NativeDesktopBackend.h"
#include <iostream>
int main()
{
    const auto permission = dockpipe::desktop::NativeDesktopBackend::preflight();
    std::cout << "{\"screen_capture\":" << (permission.screenCapture ? "true" : "false")
        << ",\"accessibility\":" << (permission.accessibility ? "true" : "false")
        << ",\"event_posting\":" << (permission.eventPosting ? "true" : "false") << "}\n";
    // Deliberately no start, input or requestPermission operations.
}
