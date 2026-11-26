#include "ProcessChannel.h"
#include "ViewerPreferences.h"
#include "../native/ClipboardProtocol.h"
#include "../native/AssetClipboardProtocol.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>
#include <utility>

namespace dockpipe::desktop {
namespace {
constexpr qsizetype MaximumRecord = 2 * 1024 * 1024 + 1;
constexpr qsizetype MaximumBacklog = 32 * 1024 + ClipboardHeaderBytes + ClipboardMaximumTextBytes + 5
    + AssetClipboardMaximumRecordBytes + 5;

QByteArray framed(const QByteArray &payload)
{
    QByteArray result(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), result.data());
    result.append(payload);
    return result;
}

bool privatePipe(int descriptors[2])
{
    if (::pipe(descriptors) != 0)
        return false;
    for (int index = 0; index < 2; ++index) {
        if (::fcntl(descriptors[index], F_SETFD, FD_CLOEXEC) < 0) {
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            return false;
        }
    }
    return true;
}
}

ProcessChannel::ProcessChannel(QObject *parent) : QObject(parent), m_process(this)
{
    connect(&m_process, &QProcess::bytesWritten, this, [this](qint64 count) {
        while (count > 0 && !m_pendingWrites.empty()) {
            auto &[remaining, kind, assetChunk] = m_pendingWrites.front();
            const auto consumed = std::min(count, remaining);
            remaining -= consumed;
            count -= consumed;
            if (kind != 4 && kind != 5)
                m_inputBacklog -= consumed;
            if (assetChunk)
                m_assetChunkBacklog -= consumed;
            if (!remaining)
                m_pendingWrites.pop_front();
        }
        emit writable();
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &ProcessChannel::readPackets);
    // Diagnostics are never buffered, journaled or mixed with binary stdout.
    m_process.setStandardErrorFile(QProcess::nullDevice());
    connect(&m_process, &QProcess::started, this, [this] {
        if (m_childControl >= 0)
            ::close(std::exchange(m_childControl, -1));
        if (m_childStatus >= 0)
            ::close(std::exchange(m_childStatus, -1));
        if (m_closing)
            terminateGroup(SIGTERM);
        for (int &descriptor : m_reservedTargets) {
            if (descriptor >= 0)
                ::close(std::exchange(descriptor, -1));
        }
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_closing)
            fail(tr("The native encrypted channel could not run."));
    });
    connect(&m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (!m_closing)
            fail(tr("The native encrypted channel closed. Reconnect to authorize a new session."));
    });
}

ProcessChannel::~ProcessChannel()
{
    close();
}

void ProcessChannel::start(const QString &helper, const QString &configuration, const QJsonObject &reference,
                           const QString &role, const QString &mode, const QJsonObject &capture)
{
    bool captureValid = false;
    try {
        const auto preferences = ViewerCapturePreferences::parse(capture);
        captureValid = preferences.json() == capture && (!(preferences.clipboardText || preferences.clipboardImages || preferences.clipboardFiles) || mode == "control");
    } catch (const std::invalid_argument &) {
        // Reject before spawning any helper or allocating its private pipes.
    }
    if (!captureValid || m_process.state() != QProcess::NotRunning || m_closing || !QFileInfo(helper).isAbsolute()
        || !QFileInfo(helper).isExecutable() || !QFileInfo(configuration).isAbsolute()) {
        fail(tr("Invalid native channel installation."));
        return;
    }
    int controls[2];
    int statuses[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, controls) != 0) {
        fail(tr("Could not create private native channel pipes."));
        return;
    }
    if (!privatePipe(statuses)) {
        ::close(controls[0]);
        ::close(controls[1]);
        fail(tr("Could not create private native channel pipes."));
        return;
    }
    // High-number duplicates avoid collisions with Qt's own exec/status pipes.
    const int childControl = ::fcntl(controls[0], F_DUPFD_CLOEXEC, 64);
    const int childStatus = ::fcntl(statuses[1], F_DUPFD_CLOEXEC, 64);
    ::close(controls[0]);
    ::close(statuses[1]);
    if (childControl < 0 || childStatus < 0) {
        if (childControl >= 0)
            ::close(childControl);
        if (childStatus >= 0)
            ::close(childStatus);
        ::close(controls[1]);
        ::close(statuses[0]);
        fail(tr("Could not create private native channel pipes."));
        return;
    }
    m_control = controls[1];
    m_status = statuses[0];
    const int controlFlags = ::fcntl(m_control, F_GETFL);
    const int statusFlags = ::fcntl(m_status, F_GETFL);
    if (controlFlags < 0 || statusFlags < 0
        || ::fcntl(m_control, F_SETFL, controlFlags | O_NONBLOCK) < 0
        || ::fcntl(m_status, F_SETFL, statusFlags | O_NONBLOCK) < 0) {
        ::close(childControl);
        ::close(childStatus);
        fail(tr("Could not create private native channel pipes."));
        return;
    }
    m_reference = reference;
    m_role = role;
    m_mode = mode;
    m_capture = capture;
    m_childControl = childControl;
    m_childStatus = childStatus;
    // Keep otherwise-free destination descriptors occupied while QProcess
    // allocates its own exec/error pipes. Never replace a parent's existing FD.
    for (int index = 0; index < 2; ++index) {
        const int target = index + 3;
        const int reservation = ::fcntl(childControl, F_DUPFD_CLOEXEC, target);
        if (reservation < 0) {
            fail(tr("Could not create private native channel pipes."));
            return;
        }
        if (reservation == target)
            m_reservedTargets[index] = reservation;
        else
            ::close(reservation);
    }
    m_process.setChildProcessModifier([childControl, childStatus] {
        if (::setsid() < 0 || ::dup2(childControl, 3) < 0 || ::dup2(childStatus, 4) < 0)
            ::_exit(127);
        ::close(childControl);
        ::close(childStatus);
    });
    m_statusNotifier = new QSocketNotifier(m_status, QSocketNotifier::Read, this);
    connect(m_statusNotifier, &QSocketNotifier::activated, this, &ProcessChannel::readStatus);
    m_process.start(helper, {"connect", "--config", configuration, "--control-fd", "3", "--status-fd", "4"});
    // Parent duplicates remain live until QProcess confirms its exec boundary.
}

void ProcessChannel::readStatus()
{
    char buffer[4096];
    while (m_status >= 0) {
        const auto length = ::read(m_status, buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR)
            continue;
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (length <= 0) {
            if (m_statusNotifier)
                m_statusNotifier->setEnabled(false);
            ::close(m_status);
            m_status = -1;
            if (!m_ready)
                fail(tr("The native channel did not verify its peer."));
            return;
        }
        m_statusOutput.append(buffer, static_cast<qsizetype>(length));
        if (m_statusOutput.size() > 4100) {
            fail(tr("Invalid native channel readiness receipt."));
            return;
        }
        if (m_statusOutput.size() < 4)
            continue;
        const auto expected = qFromBigEndian<quint32>(m_statusOutput.constData());
        if (!expected || expected > 4096 || m_statusOutput.size() > expected + 4) {
            fail(tr("Invalid native channel readiness receipt."));
            return;
        }
        if (m_statusOutput.size() < expected + 4)
            continue;
        const auto receipt = QJsonDocument::fromJson(m_statusOutput.mid(4)).object();
        if (m_ready || receipt.size() != 5 || receipt.value("type") != "ready"
            || receipt.value("reference").toObject() != m_reference || receipt.value("role") != m_role
            || receipt.value("mode") != m_mode || receipt.value("capture").toObject() != m_capture) {
            fail(tr("The encrypted channel belongs to a different session."));
            return;
        }
        m_ready = true;
        emit connected();
        QTimer::singleShot(0, this, &ProcessChannel::readPackets);
    }
}

void ProcessChannel::readPackets()
{
    do {
        if (m_closing) {
            // Keep discarding bounded chunks during asynchronous teardown;
            // QProcess must not accumulate media while its child stops.
            while (m_process.bytesAvailable() > 0)
                m_process.read(65536);
            return;
        }
        if (m_process.bytesAvailable() > MaximumRecord + 4 + 65536 - m_output.size()) {
            fail(tr("Native desktop data exceeded its buffer limit."));
            return;
        }
        if (m_process.bytesAvailable() > 0)
            m_output.append(m_process.read(65536));
        if (m_output.size() > MaximumRecord + 4 + 65536) {
            fail(tr("Native desktop data exceeded its buffer limit."));
            return;
        }
        while (m_output.size() >= 4) {
            const auto length = qFromBigEndian<quint32>(m_output.constData());
            if (length < 2 || length > MaximumRecord) {
                fail(tr("Invalid native desktop data record."));
                return;
            }
            if (m_output.size() < length + 4)
                break;
            // Status and data are separate pipes; readiness can arrive in either event order.
            if (!m_ready) {
                readStatus();
                if (!m_ready)
                    return;
            }
            const quint8 kind = static_cast<quint8>(m_output[4]);
            const auto payload = m_output.mid(5, length - 1);
            m_output.remove(0, length + 4);
            if (kind == 5) {
                try {
                    const auto record = decodeAssetClipboardRecord(reinterpret_cast<const std::uint8_t *>(payload.constData()), payload.size());
                    if (m_role != "viewer" || m_mode != "control"
                        || (!m_capture.value("clipboard_images").toBool() && !m_capture.value("clipboard_files").toBool())
                        || record.operation == AssetOperation::Arm || record.operation == AssetOperation::Pause
                        || (record.operation == AssetOperation::Offer
                            && !(record.format == AssetFormat::Png ? m_capture.value("clipboard_images").toBool()
                                                                  : m_capture.value("clipboard_files").toBool())))
                        throw std::invalid_argument("Unnegotiated asset clipboard record.");
                } catch (const std::exception &) {
                    fail(tr("The native asset clipboard record violated its session contract."));
                    return;
                }
            }
            if (kind == 4) {
                try {
                    const auto record = decodeClipboardRecord(reinterpret_cast<const std::uint8_t *>(payload.constData()), payload.size());
                    if ((m_role != "viewer" && m_role != "worker") || m_mode != "control" || !m_capture.value("clipboard_text").toBool()
                        || !clipboardDirectionAllowed(record.operation, m_role == "viewer"))
                        throw std::invalid_argument("Unnegotiated clipboard direction.");
                } catch (const std::exception &) {
                    fail(tr("The native clipboard record violated its session contract."));
                    return;
                }
            }
            emit packet(kind, payload);
            if (m_closing)
                return;
        }
    } while (m_process.bytesAvailable() > 0);
}

void ProcessChannel::renew(const QDateTime &expiresAt)
{
    if (m_closing || m_control < 0)
        return;
    const auto message = framed(QJsonDocument(QJsonObject{{"type", "lease"},
        {"expires_at", expiresAt.toUTC().toString(Qt::ISODateWithMs)}}).toJson(QJsonDocument::Compact));
    // Nonblocking private socket; a short or blocked authority write fails closed.
    const auto written = ::send(m_control, message.constData(), static_cast<std::size_t>(message.size()), MSG_NOSIGNAL);
    if (written != message.size())
        fail(tr("The native channel could not accept its authorization renewal."));
}

bool ProcessChannel::send(quint8 kind, const QByteArray &payload)
{
    bool assetChunk = false;
    if (kind == 5) {
        try {
            const auto record = decodeAssetClipboardRecord(reinterpret_cast<const std::uint8_t *>(payload.constData()), payload.size());
            if (m_role != "viewer" || m_mode != "control"
                || record.operation == AssetOperation::EpochAcknowledged
                || (!m_capture.value("clipboard_images").toBool() && !m_capture.value("clipboard_files").toBool())
                || (record.operation == AssetOperation::Offer
                    && !(record.format == AssetFormat::Png ? m_capture.value("clipboard_images").toBool()
                                                          : m_capture.value("clipboard_files").toBool())))
                return false;
            assetChunk = record.operation == AssetOperation::Chunk;
            if (assetChunk && m_assetChunkBacklog != 0)
                return false;
        } catch (const std::exception &) {
            return false;
        }
    }
    if (kind == 4) {
        try {
            const auto record = decodeClipboardRecord(reinterpret_cast<const std::uint8_t *>(payload.constData()), payload.size());
            if ((m_role != "viewer" && m_role != "worker") || m_mode != "control" || !m_capture.value("clipboard_text").toBool()
                || !clipboardDirectionAllowed(record.operation, m_role == "worker"))
                return false;
        } catch (const std::exception &) {
            return false;
        }
    }
    if (!m_ready || m_closing || payload.isEmpty()
        || (kind == 2 && (m_mode != "control" || payload.size() > 4096))
        || (kind == 3 && payload != QByteArray(1, '\1')) || (kind != 2 && kind != 3 && kind != 4 && kind != 5))
        return false;
    QByteArray record(1, static_cast<char>(kind));
    record.append(payload);
    const auto message = framed(record);
    if (message.size() > MaximumBacklog - m_process.bytesToWrite()
        || (kind != 4 && kind != 5 && message.size() > 32 * 1024 - m_inputBacklog))
        return false;
    if (m_process.write(message) != message.size())
        return false;
    m_pendingWrites.push_back({message.size(), kind, assetChunk});
    if (kind != 4 && kind != 5)
        m_inputBacklog += message.size();
    if (assetChunk)
        m_assetChunkBacklog += message.size();
    return true;
}

void ProcessChannel::fail(const QString &message)
{
    if (m_closing)
        return;
    close();
    emit failed(message);
}

void ProcessChannel::beginClose()
{
    if (m_closing)
        return;
    m_closing = true;
    m_ready = false;
    if (m_statusNotifier)
        m_statusNotifier->setEnabled(false);
    if (m_control >= 0) {
        ::close(m_control);
        m_control = -1;
    }
    if (m_status >= 0) {
        ::close(m_status);
        m_status = -1;
    }
    if (m_childControl >= 0)
        ::close(std::exchange(m_childControl, -1));
    if (m_childStatus >= 0)
        ::close(std::exchange(m_childStatus, -1));
    for (int &descriptor : m_reservedTargets) {
        if (descriptor >= 0)
            ::close(std::exchange(descriptor, -1));
    }
    m_process.closeWriteChannel();
    m_pendingWrites.clear();
    m_inputBacklog = 0;
    m_assetChunkBacklog = 0;
    m_output.clear();
    m_statusOutput.clear();
}

void ProcessChannel::terminateGroup(int signal)
{
    // Only the unreaped owned child anchors this private process group.
    if (m_process.state() == QProcess::Running && m_process.processId() > 0)
        ::kill(-static_cast<pid_t>(m_process.processId()), signal);
}

void ProcessChannel::closeAsync()
{
    if (m_closing)
        return;
    beginClose();
    terminateGroup(SIGTERM);
    QTimer::singleShot(3000, this, [this] {
        terminateGroup(SIGKILL);
        if (m_process.state() != QProcess::NotRunning)
            m_process.kill();
    });
}

void ProcessChannel::close()
{
    beginClose();
    if (m_process.state() == QProcess::Starting)
        m_process.waitForStarted(1000);
    if (m_process.state() == QProcess::Running) {
        // The unreaped direct child anchors its private session/group identity.
        // Never signal this numeric group after QProcess has reaped the leader.
        const auto group = static_cast<pid_t>(m_process.processId());
        if (group > 0)
            ::kill(-group, SIGTERM);
        // The helper allows 200 ms for relay TERM and up to two seconds to reap.
        if (!m_process.waitForFinished(3000)) {
            if (m_process.state() == QProcess::Running && group > 0)
                ::kill(-group, SIGKILL);
            m_process.kill();
            m_process.waitForFinished(1500);
        }
    }
    m_pendingWrites.clear();
    m_inputBacklog = 0;
    m_assetChunkBacklog = 0;
    m_output.clear();
    m_statusOutput.clear();
}

} // namespace dockpipe::desktop
