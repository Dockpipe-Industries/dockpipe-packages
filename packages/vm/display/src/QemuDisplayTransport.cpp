#include <gio/gio.h>
#ifndef G_OS_WIN32
#include <gio/gunixfdlist.h>
#endif
#include "QemuDisplayTransport.h"
#include "SharedFrameSource.h"
#include "DisplayEndpoint.h"
#include "NativeDisplayPlatform.h"
#include "QemuListenerPeer.h"
#include "RawFrameAssembler.h"
#include "GeometryQueryWorker.h"
#include "DisplayPointerStateStore.h"
#include "QmpAbsoluteInput.h"
#include <atomic>
#include <cstdio>
#include <limits>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <QFileInfo>

namespace {
constexpr char ListenerPath[] = "/org/qemu/Display1/Listener";
#ifdef G_OS_WIN32
constexpr char MappingInterface[] = "org.qemu.Display1.Listener.Win32.Map";
constexpr char MappingHandleType[] = "t";
#else
constexpr char MappingInterface[] = "org.qemu.Display1.Listener.Unix.Map";
constexpr char MappingHandleType[] = "h";
#endif
constexpr char ListenerXml[] = R"XML(
<node><interface name="org.qemu.Display1.Listener">
 <method name="Scanout"><arg type="u" direction="in"/><arg type="u" direction="in"/><arg type="u" direction="in"/><arg type="u" direction="in"/><arg type="ay" direction="in"/></method>
 <method name="Update"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="in"/><arg type="u" direction="in"/><arg type="ay" direction="in"/></method>
 <method name="Disable"/>
 <method name="MouseSet"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
 <method name="CursorDefine"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="ay" direction="in"/></method>
 <property name="Interfaces" type="as" access="read"/>
</interface>
<interface name="MAPPING_INTERFACE">
 <method name="ScanoutMap"><arg type="MAPPING_HANDLE" direction="in"/><arg type="u" direction="in"/><arg type="u" direction="in"/><arg type="u" direction="in"/><arg type="u" direction="in"/><arg type="u" direction="in"/></method>
 <method name="UpdateMap"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
</interface></node>)XML";
}

class QemuDisplayTransport::Implementation {
public:
    Implementation(FrameStore &frames, QString address, unsigned console, int pid, QString token, bool allowShared, QString inputAddress)
        : frames(frames), address(std::move(address)),
          consolePath(QStringLiteral("/org/qemu/Display1/Console_%1").arg(console)),
          sharedSource(frames, stopped), rawFrames(frames), expectedPid(pid), expectedStartToken(std::move(token)), allowSharedMemory(allowShared)
    {
        absoluteInputAddress = std::move(inputAddress);
        cancel = g_cancellable_new();
        absoluteInput = std::make_unique<QmpAbsoluteInput>(cancel);
        geometryWorker = std::make_unique<GeometryQueryWorker>([this] { g_cancellable_cancel(cancel); });
    }

    ~Implementation()
    {
        stop();
        geometryWorker.reset();
        absoluteInput.reset();
        g_object_unref(cancel);
    }

    void stop()
    {
        // Give already queued key/button releases a bounded chance to complete
        // before cancelling a stalled peer. No VM/process stop is requested.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (ready && !stopped && std::chrono::steady_clock::now() < deadline) {
            bool empty;
            {
                std::lock_guard lock(mutex);
                empty = inputs.empty() && !inputInFlight;
            }
            if (empty)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        stopped = true;
        g_cancellable_cancel(cancel);
        if (worker.joinable())
            worker.join();
    }

    void fail(const QString &message)
    {
        std::lock_guard lock(mutex);
        failure = message;
        stopped = true;
    }

    static void method(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                       const gchar *name, GVariant *parameters,
                       GDBusMethodInvocation *invocation, gpointer user)
    {
        auto &self = *static_cast<Implementation *>(user);
        bool accepted = true;
        if (strcmp(name, "ScanoutMap") == 0) {
            guint32 offset, width, height, stride, format;
#ifdef G_OS_WIN32
            guint64 handle;
            g_variant_get(parameters, "(tuuuuu)", &handle, &offset, &width, &height, &stride, &format);
            self.resetRawGeometry();
            accepted = handle <= quint64(std::numeric_limits<quintptr>::max()) &&
                self.sharedSource.replace(qintptr(handle), offset, width, height, stride, format);
#else
            gint32 handle;
            g_variant_get(parameters, "(huuuuu)", &handle, &offset, &width, &height, &stride, &format);
            auto *message = g_dbus_method_invocation_get_message(invocation);
            auto *fds = g_dbus_message_get_unix_fd_list(message);
            GError *error = nullptr;
            int fd = fds ? g_unix_fd_list_get(fds, handle, &error) : -1;
            g_clear_error(&error);
            self.resetRawGeometry();
            accepted = self.sharedSource.replace(fd, offset, width, height, stride, format);
#endif
            if (++self.frameMessages <= 16)
                std::fprintf(stderr, "display ScanoutMap seq=%u width=%u height=%u stride=%u format=%08x accepted=%d\n",
                             self.frameMessages, width, height, stride, format, accepted);
            if (accepted) {
                ++self.sharedScanouts;
                self.frameKind = 1;
            }
        } else if (strcmp(name, "UpdateMap") == 0) {
            gint32 x, y, width, height;
            g_variant_get(parameters, "(iiii)", &x, &y, &width, &height);
            accepted = self.sharedSource.update(x, y, width, height);
            if (++self.frameMessages <= 16)
                std::fprintf(stderr, "display UpdateMap seq=%u x=%d y=%d width=%d height=%d accepted=%d\n",
                             self.frameMessages, x, y, width, height, accepted);
            if (accepted)
                ++self.sharedUpdates;
        } else if (strcmp(name, "Scanout") == 0 || strcmp(name, "Update") == 0) {
            guint32 stride, format;
            GVariant *bytes = nullptr;
            gsize size = 0;
            if (strcmp(name, "Scanout") == 0) {
                guint32 width, height;
                g_variant_get(parameters, "(uuuu@ay)", &width, &height, &stride, &format, &bytes);
                auto *data = static_cast<const unsigned char *>(g_variant_get_fixed_array(bytes, &size, 1));
                if (++self.frameMessages <= 16)
                    std::fprintf(stderr, "display Scanout seq=%u width=%u height=%u stride=%u format=%08x bytes=%zu\n",
                                 self.frameMessages, width, height, stride, format, size);
                self.sharedSource.clear();
                self.resetRawGeometry();
                accepted = self.frames.scanout(width, height, stride, format, data, size);
                if (accepted) {
                    ++self.rawScanouts;
                    self.frameKind = 2;
                }
            } else {
                gint32 x, y, width, height;
                g_variant_get(parameters, "(iiiiuu@ay)", &x, &y, &width, &height, &stride, &format, &bytes);
                auto *data = static_cast<const unsigned char *>(g_variant_get_fixed_array(bytes, &size, 1));
                if (++self.frameMessages <= 16)
                    std::fprintf(stderr, "display Update seq=%u x=%d y=%d width=%d height=%d stride=%u format=%08x bytes=%zu\n",
                                 self.frameMessages, x, y, width, height, stride, format, size);
                accepted = self.rawFrames.enqueue(x, y, width, height, stride, format, data, size);
                if (accepted && !self.geometryPending)
                    self.requestGeometry();
            }
            g_variant_unref(bytes);
        } else if (strcmp(name, "Disable") == 0) {
            self.sharedSource.clear();
            self.resetRawGeometry();
            self.frames.disable();
            self.pointerState.clear();
            self.frameKind = 0;
        } else if (strcmp(name, "MouseSet") == 0) {
            gint32 x, y, visible;
            g_variant_get(parameters, "(iii)", &x, &y, &visible);
            accepted = self.pointerState.setPosition(x, y, visible);
        } else if (strcmp(name, "CursorDefine") == 0) {
            gint32 width, height, hotX, hotY;
            GVariant *bytes = nullptr;
            g_variant_get(parameters, "(iiii@ay)", &width, &height, &hotX, &hotY, &bytes);
            gsize size = 0;
            auto *data = static_cast<const unsigned char *>(g_variant_get_fixed_array(bytes, &size, 1));
            accepted = self.pointerState.defineCursor(width, height, hotX, hotY, data, size);
            g_variant_unref(bytes);
        }
        if (accepted) {
            g_dbus_method_invocation_return_value(invocation, nullptr);
        } else {
            g_dbus_method_invocation_return_dbus_error(invocation,
                "org.dockpipe.Display.InvalidFrame", "Unsupported or invalid software frame");
            self.fail(QStringLiteral("Display listener rejected %1: %2")
                .arg(QString::fromLatin1(name), self.frames.error()));
        }
    }

    void resetRawGeometry()
    {
        rawFrames.clear();
        ++geometryGeneration;
        geometryPending = false;
        geometryNeeded = false;
    }

    void requestGeometry()
    {
        const bool started = geometryWorker->start(geometryGeneration, [this] {
            GeometryQueryWorker::Result result;
            GError *error = nullptr;
            auto *reply = g_dbus_connection_call_sync(control, service.toUtf8().constData(),
                consolePath.toUtf8().constData(), "org.freedesktop.DBus.Properties", "GetAll",
                g_variant_new("(s)", "org.qemu.Display1.Console"), G_VARIANT_TYPE("(a{sv})"),
                G_DBUS_CALL_FLAGS_NONE, 2000, cancel, &error);
            GVariant *properties = nullptr;
            if (reply)
                g_variant_get(reply, "(@a{sv})", &properties);
            result.success = properties &&
                g_variant_lookup(properties, "Width", "u", &result.width) &&
                g_variant_lookup(properties, "Height", "u", &result.height);
            if (properties)
                g_variant_unref(properties);
            if (reply)
                g_variant_unref(reply);
            g_clear_error(&error);
            return result;
        });
        geometryPending = started;
        geometryNeeded = !started;
    }

    void applyGeometryCompletion()
    {
        const auto result = geometryWorker->take();
        if (result && result->generation == geometryGeneration && !stopped) {
            geometryPending = false;
            if (!result->success || !rawFrames.applyGeometry(result->width, result->height)) {
                fail(QStringLiteral("QEMU authoritative raw-frame geometry rejected"));
            } else {
                ++rawBatches;
                frameKind = 2;
            }
        }
        if (geometryNeeded && !stopped)
            requestGeometry();
    }

    static GVariant *property(GDBusConnection *, const gchar *, const gchar *,
                              const gchar *, const gchar *, GError **, gpointer user)
    {
        const auto &self = *static_cast<Implementation *>(user);
        const char *interfaces[] = {MappingInterface, nullptr};
        return g_variant_new_strv(self.allowSharedMemory ? interfaces : nullptr,
                                 self.allowSharedMemory ? -1 : 0);
    }

    GVariant *call(const char *interface, const char *method, GVariant *parameters)
    {
        GError *error = nullptr;
        GVariant *result = g_dbus_connection_call_sync(control, service.toUtf8().constData(),
            consolePath.toUtf8().constData(), interface, method, parameters, nullptr,
            G_DBUS_CALL_FLAGS_NONE, 2000, cancel, &error);
        if (error) {
            fail(QStringLiteral("QEMU display operation failed"));
            g_error_free(error);
        }
        return result;
    }

    bool connectPeer()
    {
        if (!NativeDisplayPlatform::privateBusAddress(address)) {
            fail(QStringLiteral("Display bus must be an owned socket in a private directory"));
            return false;
        }
        GError *error = nullptr;
        control = g_dbus_connection_new_for_address_sync(address.toUtf8().constData(),
            GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                 G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, cancel, &error);
        if (!control) {
            g_clear_error(&error);
            fail(QStringLiteral("Cannot connect to private display bus"));
            return false;
        }
        g_dbus_connection_set_exit_on_close(control, false);
        auto *owner = g_dbus_connection_call_sync(control, "org.freedesktop.DBus",
            "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
            g_variant_new("(s)", "org.qemu"), G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, cancel, &error);
        if (!owner) {
            g_clear_error(&error);
            fail(QStringLiteral("QEMU display service is unavailable"));
            return false;
        }
        const gchar *ownerName = nullptr;
        g_variant_get(owner, "(&s)", &ownerName);
        service = QString::fromUtf8(ownerName);
        g_variant_unref(owner);
        auto *identity = g_dbus_connection_call_sync(control, "org.freedesktop.DBus",
            "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixProcessID",
            g_variant_new("(s)", service.toUtf8().constData()), G_VARIANT_TYPE("(u)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, cancel, &error);
        guint32 ownerPid = 0;
        if (identity) {
            g_variant_get(identity, "(u)", &ownerPid);
            g_variant_unref(identity);
        }
        if (!identity || ownerPid != guint32(expectedPid) ||
            !DisplayEndpoint::processMatches(expectedPid, expectedStartToken)) {
            g_clear_error(&error);
            fail(QStringLiteral("QEMU display service process identity mismatch"));
            return false;
        }
        auto *value = call("org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", "org.qemu.Display1.Mouse", "IsAbsolute"));
        if (!value)
            return false;
        GVariant *wrapped = nullptr;
        g_variant_get(value, "(v)", &wrapped);
        if (!g_variant_is_of_type(wrapped, G_VARIANT_TYPE_BOOLEAN)) {
            fail(QStringLiteral("Invalid mouse capability"));
        } else {
            absolute = g_variant_get_boolean(wrapped);
        }
        g_variant_unref(wrapped);
        g_variant_unref(value);
        mouseSubscription = g_dbus_connection_signal_subscribe(control,
            service.toUtf8().constData(), "org.freedesktop.DBus.Properties", "PropertiesChanged",
            consolePath.toUtf8().constData(), "org.qemu.Display1.Mouse", G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *,
               GVariant *parameters, gpointer data) {
                auto &self = *static_cast<Implementation *>(data);
                if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv}as)"))) {
                    self.fail(QStringLiteral("Invalid mouse capability notification"));
                    return;
                }
                GVariant *properties = g_variant_get_child_value(parameters, 1);
                GVariant *mode = g_variant_lookup_value(properties, "IsAbsolute", nullptr);
                if (mode) {
                    if (g_variant_is_of_type(mode, G_VARIANT_TYPE_BOOLEAN))
                        self.absolute = g_variant_get_boolean(mode);
                    else
                        self.fail(QStringLiteral("Invalid mouse capability value"));
                    g_variant_unref(mode);
                }
                g_variant_unref(properties);
            }, this, nullptr);
        GSocketConnection *stream = registerQemuListenerPeer(control,
            service.toUtf8().constData(), consolePath.toUtf8().constData(), expectedPid, cancel, &error);
        if (!stream) {
            g_clear_error(&error);
            fail(QStringLiteral("QEMU listener registration failed"));
            return false;
        }
        peer = g_dbus_connection_new_sync(G_IO_STREAM(stream), nullptr,
            GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                 G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING),
            nullptr, cancel, &error);
        g_object_unref(stream);
        if (!peer) {
            g_clear_error(&error);
            fail(QStringLiteral("QEMU listener authentication failed"));
            return false;
        }
        g_dbus_connection_set_exit_on_close(peer, false);
        QByteArray xml(ListenerXml);
        xml.replace("MAPPING_INTERFACE", MappingInterface);
        xml.replace("MAPPING_HANDLE", MappingHandleType);
        node = g_dbus_node_info_new_for_xml(xml.constData(), &error);
        static const GDBusInterfaceVTable table = {method, property, nullptr, {nullptr}};
        registration = g_dbus_connection_register_object(peer, ListenerPath,
            node->interfaces[0], &table, this, nullptr, &error);
        if (!registration) {
            g_clear_error(&error);
            fail(QStringLiteral("Cannot export display listener"));
            return false;
        }
        mapRegistration = g_dbus_connection_register_object(peer, ListenerPath,
            node->interfaces[1], &table, this, nullptr, &error);
        if (!mapRegistration) {
            g_clear_error(&error);
            fail(QStringLiteral("Cannot export shared framebuffer listener"));
            return false;
        }
        g_dbus_connection_start_message_processing(peer);
        ready = !stopped;
        return !stopped;
    }

    void dispatch(DisplayInput input)
    {
        if (!absoluteInputAddress.isEmpty()) {
            if (input.kind == DisplayInput::Kind::Absolute) {
                pointerPositionValid = false;
                if (input.frameSize != frames.snapshot().size())
                    return;
                if (!absoluteInput->move(QPoint(input.first, input.second), input.frameSize)) {
                    fail(QStringLiteral("Private absolute input channel failed"));
                    return;
                }
                pointerPositionValid = true;
                ++acknowledged;
                return;
            }
            if (input.kind == DisplayInput::Kind::ButtonPress && absolute && !pointerPositionValid)
                return;
        }
        const char *interface = "org.qemu.Display1.Mouse";
        const char *method = nullptr;
        GVariant *parameters = nullptr;
        switch (input.kind) {
        case DisplayInput::Kind::KeyPress:
        case DisplayInput::Kind::KeyRelease:
            interface = "org.qemu.Display1.Keyboard";
            method = input.kind == DisplayInput::Kind::KeyPress ? "Press" : "Release";
            parameters = g_variant_new("(u)", guint32(input.first));
            break;
        case DisplayInput::Kind::ButtonPress:
        case DisplayInput::Kind::ButtonRelease:
            method = input.kind == DisplayInput::Kind::ButtonPress ? "Press" : "Release";
            parameters = g_variant_new("(u)", guint32(input.first));
            break;
        case DisplayInput::Kind::Absolute:
            method = "SetAbsPosition";
            parameters = g_variant_new("(uu)", guint32(input.first), guint32(input.second));
            break;
        case DisplayInput::Kind::Relative:
            method = "RelMotion";
            parameters = g_variant_new("(ii)", input.first, input.second);
            break;
        }
        auto *reply = call(interface, method, parameters);
        if (reply) {
            ++acknowledged;
            g_variant_unref(reply);
        }
    }

    void run()
    {
        GMainContext *context = g_main_context_new();
        g_main_context_push_thread_default(context);
        bool inputReady = true;
        if (!absoluteInputAddress.isEmpty()) {
            inputReady = absoluteInput->connect(absoluteInputAddress, expectedPid, expectedStartToken);
            if (!inputReady)
                fail(QStringLiteral("Cannot admit private absolute input channel"));
        }
        if (inputReady && connectPeer()) {
            while (!stopped && !g_dbus_connection_is_closed(control) &&
                   !g_dbus_connection_is_closed(peer)) {
                if (!DisplayEndpoint::processMatches(expectedPid, expectedStartToken)) {
                    fail(QStringLiteral("QEMU display process identity expired"));
                    break;
                }
                applyGeometryCompletion();
                // Bound callback dispatch per pass so cancellation/input cannot starve.
                for (int count = 0; count < 16 && !stopped && g_main_context_pending(context); ++count)
                    g_main_context_iteration(context, false);
                std::deque<DisplayInput> pending;
                {
                    std::lock_guard lock(mutex);
                    pending.swap(inputs);
                    inputInFlight = !pending.empty();
                }
                for (auto input : pending) {
                    if (stopped)
                        break;
                    dispatch(input);
                }
                inputInFlight = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!stopped)
                fail(QStringLiteral("Local display disconnected"));
        }
        ready = false;
        g_cancellable_cancel(cancel);
        geometryWorker->stop();
        absoluteInput->close();
        rawFrames.clear();
        sharedSource.clear();
        if (peer) {
            if (registration)
                g_dbus_connection_unregister_object(peer, registration);
            if (mapRegistration)
                g_dbus_connection_unregister_object(peer, mapRegistration);
            g_dbus_connection_close_sync(peer, nullptr, nullptr);
            g_object_unref(peer);
        }
        if (node)
            g_dbus_node_info_unref(node);
        if (control) {
            if (mouseSubscription)
                g_dbus_connection_signal_unsubscribe(control, mouseSubscription);
            g_dbus_connection_close_sync(control, nullptr, nullptr);
            g_object_unref(control);
        }
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }

    FrameStore &frames;
    QString address;
    QString consolePath;
    std::thread worker;
    GCancellable *cancel;
    std::atomic<bool> stopped{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> absolute{true};
    std::atomic<bool> inputInFlight{false};
    std::atomic<quint64> acknowledged{0};
    mutable std::mutex mutex;
    QString failure;
    std::deque<DisplayInput> inputs;
    GDBusConnection *control = nullptr;
    GDBusConnection *peer = nullptr;
    GDBusNodeInfo *node = nullptr;
    guint registration = 0;
    guint mouseSubscription = 0;
    unsigned frameMessages = 0;
    guint mapRegistration = 0;
    SharedFrameSource sharedSource;
    RawFrameAssembler rawFrames;
    DisplayPointerStateStore pointerState;
    bool geometryPending = false;
    quint64 geometryGeneration = 0;
    bool geometryNeeded = false;
    std::unique_ptr<GeometryQueryWorker> geometryWorker;
    std::atomic<int> frameKind{0};
    std::atomic<quint64> rawScanouts{0};
    std::atomic<quint64> rawBatches{0};
    std::atomic<quint64> sharedScanouts{0};
    std::atomic<quint64> sharedUpdates{0};
    int expectedPid;
    QString expectedStartToken;
    QString service;
    bool allowSharedMemory;
    QString absoluteInputAddress;
    std::unique_ptr<QmpAbsoluteInput> absoluteInput;
    bool pointerPositionValid = false;
};

QemuDisplayTransport::QemuDisplayTransport(FrameStore &frames, QString address, unsigned console,
                                           int pid, QString token, bool allowShared, QString inputAddress)
    : implementation_(std::make_unique<Implementation>(frames, std::move(address), console, pid, std::move(token), allowShared, std::move(inputAddress)))
{
}
QemuDisplayTransport::~QemuDisplayTransport() = default;
void QemuDisplayTransport::start()
{
    if (!implementation_->worker.joinable())
        implementation_->worker = std::thread([this] { implementation_->run(); });
}
void QemuDisplayTransport::stop()
{
    implementation_->stop();
}
void QemuDisplayTransport::submit(DisplayInput input)
{
    std::lock_guard lock(implementation_->mutex);
    if (!implementation_->ready)
        return;
    // Position/motion events coalesce, while release events are never silently lost.
    auto &queue = implementation_->inputs;
    if (!queue.empty() && queue.back().kind == input.kind &&
        input.kind == DisplayInput::Kind::Absolute) {
        queue.back() = input;
    } else if (queue.size() < 128) {
        queue.push_back(input);
    } else {
        implementation_->failure = QStringLiteral("Display input queue exceeded bound");
        implementation_->stopped = true;
    }
}
QString QemuDisplayTransport::error() const
{
    std::lock_guard lock(implementation_->mutex);
    return implementation_->failure;
}
bool QemuDisplayTransport::connected() const
{
    return implementation_->ready;
}
bool QemuDisplayTransport::absolutePointer() const
{
    return implementation_->absolute;
}
quint64 QemuDisplayTransport::inputAcknowledgements() const
{
    return implementation_->acknowledged;
}

quint64 QemuDisplayTransport::sharedScanouts() const
{
    return implementation_->sharedScanouts;
}
quint64 QemuDisplayTransport::sharedUpdates() const
{
    return implementation_->sharedUpdates;
}

quint64 QemuDisplayTransport::rawScanouts() const
{
    return implementation_->rawScanouts;
}

quint64 QemuDisplayTransport::rawBatches() const
{
    return implementation_->rawBatches;
}

QString QemuDisplayTransport::deliveryKind() const
{
    const int kind = implementation_->frameKind;
    if (kind == 1)
        return QStringLiteral("shared-memory");
    if (kind == 2)
        return QStringLiteral("raw-local");
    return QStringLiteral("none");
}

DisplayPointerState QemuDisplayTransport::pointerState() const
{
    return implementation_->pointerState.snapshot(implementation_->absolute);
}
