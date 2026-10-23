#define NOMINMAX
#include <winsock2.h>
#include "QemuListenerPeer.h"
#include "NativeDisplayPlatform.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

static_assert(GLIB_CHECK_VERSION(2, 72, 0), "Windows AF_UNIX credentials require GLib >=2.72");

GSocketConnection *registerQemuListenerPeer(GDBusConnection *control, const char *service,
    const char *consolePath, int qemuPid, GCancellable *cancel, GError **error)
{
    // Windows has no socketpair API. Build an AF_UNIX pair inside a protected
    // user-owned directory; TCP would not supply QEMU's required peer PID.
    QTemporaryDir temporary(QDir::tempPath() + "/dp-display-XXXXXX");
    if (!temporary.isValid() || !NativeDisplayPlatform::protectDirectory(temporary.path())) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create private listener directory");
        return nullptr;
    }
    const auto path = temporary.path() + "/peer";
    const auto encoded = path.toUtf8();
    if (encoded.size() > 100) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Listener socket path exceeds bound");
        return nullptr;
    }
    GSocket *listener = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                    G_SOCKET_PROTOCOL_DEFAULT, error);
    if (!listener)
        return nullptr;
    g_socket_set_timeout(listener, 3);
    GSocketAddress *address = g_unix_socket_address_new(encoded.constData());
    bool ready = g_socket_bind(listener, address, false, error) && g_socket_listen(listener, error);
    GSocket *client = nullptr;
    GSocket *server = nullptr;
    if (ready) {
        client = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, error);
        ready = client != nullptr;
    }
    if (ready) {
        g_socket_set_timeout(client, 3);
        ready = g_socket_connect(client, address, cancel, error);
    }
    if (ready)
        server = g_socket_accept(listener, cancel, error);
    g_object_unref(address);
    g_object_unref(listener);
    QFile::remove(path);
    if (!server) {
        if (client)
            g_object_unref(client);
        return nullptr;
    }
    WSAPROTOCOL_INFOW information{};
    if (WSADuplicateSocketW(SOCKET(g_socket_get_fd(server)), DWORD(qemuPid), &information) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot duplicate listener into QEMU");
        g_object_unref(server);
        g_object_unref(client);
        return nullptr;
    }
    GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, &information, sizeof(information), 1);
    GVariant *reply = g_dbus_connection_call_sync(control, service, consolePath,
        "org.qemu.Display1.Console", "RegisterListener", g_variant_new("(@ay)", bytes),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, cancel, error);
    g_object_unref(server);
    if (!reply) {
        g_object_unref(client);
        return nullptr;
    }
    g_variant_unref(reply);
    GSocketConnection *stream = g_socket_connection_factory_create_connection(client);
    g_object_unref(client);
    return stream;
}
