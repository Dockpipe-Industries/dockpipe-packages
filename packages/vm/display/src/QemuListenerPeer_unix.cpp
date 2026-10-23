#include "QemuListenerPeer.h"
#include <gio/gunixfdlist.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

GSocketConnection *registerQemuListenerPeer(GDBusConnection *control, const char *service,
    const char *consolePath, int, GCancellable *cancel, GError **error)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create listener socket pair");
        return nullptr;
    }
    if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) == -1 || fcntl(pair[1], F_SETFD, FD_CLOEXEC) == -1) {
        close(pair[0]);
        close(pair[1]);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot protect listener descriptors");
        return nullptr;
    }
    GUnixFDList *fds = g_unix_fd_list_new();
    const int index = g_unix_fd_list_append(fds, pair[1], error);
    close(pair[1]);
    GVariant *reply = nullptr;
    if (index >= 0)
        reply = g_dbus_connection_call_with_unix_fd_list_sync(control, service, consolePath,
            "org.qemu.Display1.Console", "RegisterListener", g_variant_new("(h)", index),
            nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, fds, nullptr, cancel, error);
    g_object_unref(fds);
    if (!reply) {
        close(pair[0]);
        return nullptr;
    }
    g_variant_unref(reply);
    GSocket *socket = g_socket_new_from_fd(pair[0], error);
    if (!socket) {
        close(pair[0]);
        return nullptr;
    }
    GSocketConnection *stream = g_socket_connection_factory_create_connection(socket);
    g_object_unref(socket);
    return stream;
}
