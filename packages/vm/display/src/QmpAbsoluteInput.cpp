#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <mstcpip.h>
#else
#include <sys/socket.h>
#ifdef __APPLE__
#include <sys/un.h>
#endif
#include <unistd.h>
#endif
#include <gio/gunixsocketaddress.h>
#include "QmpAbsoluteInput.h"
#include "DisplayAddress.h"
#include "NativeDisplayPlatform.h"
#include "FrameStore.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

namespace {
constexpr qsizetype MaximumMessageBytes = 65536;
constexpr unsigned MaximumAsyncEvents = 32;

bool peerMatches(GSocket *socket, int expectedPid)
{
#ifdef _WIN32
#ifndef SIO_AF_UNIX_GETPEERPID
#define SIO_AF_UNIX_GETPEERPID _WSAIOR(IOC_VENDOR, 256)
#endif
    // The Windows AF_UNIX provider returns the PID through u_long. Its
    // WSAIoctl variant can succeed with lpBytesReturned == 0, so use the
    // canonical ioctlsocket interface also used by QEMU itself.
    u_long pid = 0;
    static_assert(sizeof(pid) == sizeof(DWORD));
    return ioctlsocket(SOCKET(g_socket_get_fd(socket)), SIO_AF_UNIX_GETPEERPID, &pid) == 0 &&
           pid == DWORD(expectedPid);
#elif defined(__APPLE__)
    pid_t pid = 0;
    socklen_t size = sizeof(pid);
    return getsockopt(g_socket_get_fd(socket), SOL_LOCAL, LOCAL_PEERPID, &pid, &size) == 0 &&
           size == sizeof(pid) && pid == expectedPid;
#else
    struct ucred credentials{};
    socklen_t size = sizeof(credentials);
    return getsockopt(g_socket_get_fd(socket), SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
           size == sizeof(credentials) && credentials.pid == expectedPid && credentials.uid == getuid();
#endif
}
}

QmpAbsoluteInput::QmpAbsoluteInput(GCancellable *cancel)
    : cancel_(G_CANCELLABLE(g_object_ref(cancel)))
{
}

QmpAbsoluteInput::~QmpAbsoluteInput()
{
    close();
    g_object_unref(cancel_);
}

void QmpAbsoluteInput::close()
{
    received_.clear();
    if (connection_) {
        g_io_stream_close(G_IO_STREAM(connection_), nullptr, nullptr);
        g_object_unref(connection_);
        connection_ = nullptr;
    }
}

bool QmpAbsoluteInput::normalize(QPoint pixel, QSize frame, QPoint &normalized)
{
    if (frame.width() < 1 || frame.height() < 1 ||
        frame.width() > int(FrameStore::MaximumDimension) ||
        frame.height() > int(FrameStore::MaximumDimension) ||
        pixel.x() < 0 || pixel.y() < 0 || pixel.x() >= frame.width() || pixel.y() >= frame.height())
        return false;
    const auto axis = [](int coordinate, int dimension) {
        if (dimension == 1)
            return 0;
        return int((qint64(coordinate) * 32767 + (dimension - 1) / 2) / (dimension - 1));
    };
    normalized = QPoint(axis(pixel.x(), frame.width()), axis(pixel.y(), frame.height()));
    // The qualified Windows tablet ignores the all-zero pair. One normalized
    // unit still rounds to pixel zero at every supported dimension (<=4096).
    if (normalized.isNull())
        normalized = QPoint(1, 1);
    return true;
}

bool QmpAbsoluteInput::transfer(char *bytes, qsizetype size, bool writing, Deadline deadline)
{
    GSocket *socket = g_socket_connection_get_socket(connection_);
    qsizetype offset = 0;
    while (offset < size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || g_cancellable_is_cancelled(cancel_))
            return false;
        GError *error = nullptr;
        const auto condition = writing ? G_IO_OUT : G_IO_IN;
        if (!g_socket_condition_timed_wait(socket, GIOCondition(condition), remaining, cancel_, &error)) {
            g_clear_error(&error);
            return false;
        }
        const auto count = writing
            ? g_socket_send(socket, bytes + offset, size_t(size - offset), cancel_, &error)
            : g_socket_receive(socket, bytes + offset, size_t(size - offset), cancel_, &error);
        if (count < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            g_clear_error(&error);
            continue;
        }
        g_clear_error(&error);
        if (count <= 0)
            return false;
        offset += count;
    }
    return true;
}

bool QmpAbsoluteInput::readObject(QJsonObject &object, Deadline deadline)
{
    auto *socket = g_socket_connection_get_socket(connection_);
    for (;;) {
        const auto newline = received_.indexOf('\n');
        if (newline >= 0) {
            if (newline >= MaximumMessageBytes)
                return false;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(received_.left(newline), &error);
            received_.remove(0, newline + 1);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                return false;
            object = document.object();
            return true;
        }
        if (received_.size() >= MaximumMessageBytes)
            return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || g_cancellable_is_cancelled(cancel_))
            return false;
        GError *error = nullptr;
        if (!g_socket_condition_timed_wait(socket, G_IO_IN, remaining, cancel_, &error)) {
            g_clear_error(&error);
            return false;
        }
        char chunk[4096];
        const auto capacity = std::min(qsizetype(sizeof(chunk)), MaximumMessageBytes - received_.size());
        const auto count = g_socket_receive(socket, chunk, size_t(capacity), cancel_, &error);
        if (count < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            g_clear_error(&error);
            continue;
        }
        g_clear_error(&error);
        if (count <= 0)
            return false;
        received_.append(chunk, qsizetype(count));
    }
}

bool QmpAbsoluteInput::exchange(const QJsonObject &request)
{
    if (!connection_)
        return false;
    const bool success = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
        if (!transfer(bytes.data(), bytes.size(), true, deadline))
            return false;
        for (unsigned count = 0; count <= MaximumAsyncEvents; ++count) {
            QJsonObject reply;
            if (!readObject(reply, deadline))
                return false;
            if (reply.contains("event") && !reply.contains("id") && !reply.contains("return") && !reply.contains("error"))
                continue;
            return reply.size() == 2 && reply.value("id") == request.value("id") &&
                   reply.value("return").isObject() && reply.value("return").toObject().isEmpty();
        }
        return false;
    }();
    if (!success)
        close();
    return success;
}

bool QmpAbsoluteInput::connect(const QString &address, int expectedPid, const QString &startToken)
{
    close();
    QString path;
    if (!NativeDisplayPlatform::privateBusAddress(address) || !decodeDisplayBusPath(address, path) ||
        !NativeDisplayPlatform::processMatches(expectedPid, startToken))
        return false;
    auto *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 2);
    auto *endpoint = g_unix_socket_address_new(path.toUtf8().constData());
    GError *error = nullptr;
    connection_ = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(endpoint), cancel_, &error);
    g_object_unref(endpoint);
    g_object_unref(client);
    g_clear_error(&error);
    if (!connection_)
        return false;
    auto *socket = g_socket_connection_get_socket(connection_);
    g_socket_set_blocking(socket, false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    QJsonObject greeting;
    if (!peerMatches(socket, expectedPid) || !NativeDisplayPlatform::processMatches(expectedPid, startToken) ||
        !readObject(greeting, deadline) || !greeting.value("QMP").isObject() ||
        !exchange({{"execute", "qmp_capabilities"}, {"id", "dockpipe-input-capabilities"}})) {
        close();
        return false;
    }
    expectedPid_ = expectedPid;
    startToken_ = startToken;
    return true;
}

bool QmpAbsoluteInput::move(QPoint pixel, QSize frame)
{
    QPoint normalized;
    if (!connection_)
        return false;
    if (!NativeDisplayPlatform::processMatches(expectedPid_, startToken_) ||
        !normalize(pixel, frame, normalized)) {
        close();
        return false;
    }
    const QJsonArray events{
        QJsonObject{{"type", "abs"}, {"data", QJsonObject{{"axis", "x"}, {"value", normalized.x()}}}},
        QJsonObject{{"type", "abs"}, {"data", QJsonObject{{"axis", "y"}, {"value", normalized.y()}}}}
    };
    return exchange({{"execute", "input-send-event"}, {"arguments", QJsonObject{{"events", events}}},
                     {"id", QStringLiteral("dockpipe-input-%1").arg(++sequence_)}});
}
