#pragma once
#include <gio/gio.h>

// Returns the frontend side after QEMU has imported the other end. Authentication
// is performed by the caller with its cancellable before dispatch is enabled.
GSocketConnection *registerQemuListenerPeer(GDBusConnection *control, const char *service,
    const char *consolePath, int qemuPid, GCancellable *cancel, GError **error);
