#include "DesktopViewerSurface.h"
#include "H264Decoder.h"
#include "GpuVideoWidget.h"
#include <QGuiApplication>
#include "InputMapping.h"
#include "ProcessChannel.h"
#include "RichClipboardTransfer.h"
#include "ClipboardPasteIntent.h"
#include <QProgressBar>
#include "ClipboardSelection.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include "ViewerPreferences.h"
#include "../native/ClipboardProtocol.h"
#include <QClipboard>
#include <QMimeData>

#include <QDateTime>
#include <QEnterEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSet>
#include <QScrollBar>
#include <QStyle>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QWheelEvent>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace dockpipe::desktop {
namespace {
constexpr auto RememberedSessions = "remote/uncertainDesktopSessions";
constexpr int MaximumRememberedSessions = 64;
constexpr int CloseObservationMilliseconds = 750;
constexpr int CloseCompletionMilliseconds = 12000;

bool digest(const QString &value)
{
    static const QRegularExpression pattern("^[0-9a-f]{64}$");
    return pattern.match(value).hasMatch();
}

bool writePrivate(const QString &path, const QJsonObject &value)
{
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    return output.open(QIODevice::WriteOnly)
        && output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        && output.write(bytes) == bytes.size() && output.commit();
}

bool remember(const QJsonObject &reference, bool remove)
{
    QSettings settings;
    const auto bytes = settings.value(RememberedSessions).toByteArray();
    if (bytes.size() > 256 * 1024)
        return false;
    QJsonParseError error{};
    QJsonArray previous;
    if (!bytes.isEmpty()) {
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isArray())
            return false;
        previous = document.array();
    }
    if (previous.size() > MaximumRememberedSessions)
        return false;
    QJsonArray next;
    for (const auto &entry : previous) {
        if (entry.toObject() != reference)
            next.append(entry);
    }
    if (!remove) {
        if (next.size() >= MaximumRememberedSessions)
            return false;
        next.append(reference);
    }
    settings.setValue(RememberedSessions, QJsonDocument(next).toJson(QJsonDocument::Compact));
    settings.sync();
    return settings.status() == QSettings::NoError;
}

std::optional<int> nativeButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton: return 0;
    case Qt::RightButton: return 1;
    case Qt::MiddleButton: return 2;
    default: return std::nullopt;
    }
}


}

struct DesktopViewerSurface::Impl {
    DesktopViewerSurface *owner;
    QString cli;
    QString stateDirectory;
    QString helper;
    QString mode;
    QJsonObject catalog;
    ViewerCapturePreferences capture;
    QString decoderPreference = "software";
    QJsonObject decoderState;
    QJsonObject decoderFailureState;
    QJsonObject decodedTiming;
    QJsonObject pendingDecoderState;
    QJsonObject pendingDecodedTiming;
    ViewerPresentationPreferences presentation;
    QScrollBar horizontalScroll;
    QScrollBar verticalScroll;
    QRect imageViewport;
    QJsonObject reference;
    QJsonObject intent;
    QTemporaryDir directory;
    QProcess control;
    QProcess identity;
    ProcessChannel channel;
    InputMapping keyboard;
    std::unique_ptr<RichClipboardTransfer> richClipboard;
    QLabel *clipboardStatus = nullptr;
    QProgressBar *clipboardProgress = nullptr;
    ClipboardPasteIntent automaticPaste;
    QElapsedTimer automaticPasteElapsed;
    QTimer automaticPastePoll;
    QTimer startup;
    QTimer motion;
    QTimer closePoll;
    QTimer clipboardDeadline;
    QElapsedTimer clipboardElapsed;
    bool active = false;
    bool clipboardArmed = false;
    bool applyingClipboard = false;
    quint64 clipboardEpoch = 0;
    quint64 clipboardRevision = 0;
    quint64 remoteClipboardRevision = 0;
    bool clipboardPending = false;
    std::optional<QByteArray> nextClipboard;
    std::optional<QByteArray> incomingClipboard;
    std::deque<QJsonObject> blockedInput;
    QSet<int> sentKeys;
    QSet<int> sentButtons;
    QElapsedTimer closeElapsed;
    QJsonObject pendingMotion;
    QDateTime leaseExpiry;
    QString localFingerprint;
    QString peerFingerprint;
    QByteArray controlOutput;
    QByteArray identityOutput;
    bool started = false;
    bool closing = false;
    bool preparedAck = false;
    bool bindingSent = false;
    bool channelStarted = false;
    bool nativeReady = false;
    bool nativeClosed = false;
    bool synchronousClosed = false;
    bool closeRequested = false;
    bool closeCompleted = false;
    bool closeRejected = false;
    bool closeTimedOut = false;
    bool controlTerminated = false;
    QJsonObject closeResult;
    quint64 inputSequence = 0;
    quint64 receivedFrame = 0;
    quint64 signalSequence = 0;
    quint64 outgoingSignal = 0;
    quint64 acknowledgedSignal = 0;
    QSet<int> keys;
    QSet<int> suppressedPasteKeys;
    QSet<int> buttons;
    QPointF pointer;
    QPoint hoverGlobalPosition;
    bool pointerOverSurface = false;
    bool localCursorHidden = false;
    QString status;
    QImage displayed;
    QSize displaySize;
    GpuVideoWidget *gpu = nullptr;
    bool gpuDisplayed = false;
    std::mutex decoderMutex;
    std::condition_variable decoderChanged;
    // Relay bursts can deliver adjacent H.264 pictures in one UI event.
    // Absorb short relay bursts while the decoder drains reference pictures.
    static constexpr std::size_t MaximumPendingFrames = 4;
    std::deque<VideoFrame> pendingFrames;
    QImage pendingImage;
    NativeVideoFrame pendingNative;
    bool imageQueued = false;
    bool decoderStopping = false;
    bool keyframeQueued = false;
    std::shared_ptr<std::atomic<bool>> callbacksAlive = std::make_shared<std::atomic<bool>>(true);
    std::thread decoderThread;
    std::atomic<bool> decoderExited{true};

    Impl(DesktopViewerSurface *widget, const QJsonObject &options, QString channelHelper)
        : owner(widget), cli(options.value("cli").toString()),
          stateDirectory(options.value("state_directory").toString()), helper(std::move(channelHelper)),
          mode(options.value("mode").toString()), catalog(options.value("catalog").toObject()),
          horizontalScroll(Qt::Horizontal, widget), verticalScroll(Qt::Vertical, widget),
          control(widget), identity(widget), channel(widget), startup(widget), motion(widget), closePoll(widget)
    {
        if ((options.value("schema") != "dockpipe.remote.desktop-display/v1"
            && options.value("schema") != "dockpipe.desktop.native/v1") || !directory.isValid()
            || !QFileInfo(cli).isAbsolute() || !QFileInfo(cli).isExecutable()
            || !QFileInfo(stateDirectory).isAbsolute() || !QFileInfo(stateDirectory).isDir()
            || (mode != "view" && mode != "control") || !catalog.value("view").toBool()
            || (mode == "control" && !catalog.value("input").toBool())
            || catalog.value("schema") != "dockpipe.remote.desktop/v1"
            || !digest(catalog.value("epoch").toString()))
            throw std::invalid_argument("The selected native desktop context is unavailable.");
        const auto target = catalog.value("target").toObject();
        const auto provider = catalog.value("provider").toObject();
        if (target.value("node").toString().isEmpty() || !digest(target.value("fingerprint").toString())
            || !digest(target.value("generation").toString()) || provider.value("id") != "dockpipe.desktop.native"
            || provider.value("version") != "0.1.0" || !digest(provider.value("artifact").toString()))
            throw std::invalid_argument("The native desktop provider identity does not match this package.");
        if (mode == "control" && !keyboard.nativeMappingAvailable())
            throw std::invalid_argument("Native keyboard control requires an available X11 physical key map.");
        if (options.contains("capture")) {
            if (!options.value("capture").isObject())
                throw std::invalid_argument("Unsupported native capture preferences.");
            capture = ViewerCapturePreferences::parse(options.value("capture").toObject());
        }
        if ((capture.clipboardText || capture.clipboardImages || capture.clipboardFiles) && mode != "control")
            throw std::invalid_argument("Clipboard sharing requires control mode.");
        if (options.contains("decoder")) {
            const auto selected = options.value("decoder");
            if (selected != "software" && selected != "auto" && selected != "hardware")
                throw std::invalid_argument("Unsupported native decoder preference.");
            decoderPreference = selected.toString();
        }
        if (options.contains("display")) {
            if (!options.value("display").isObject())
                throw std::invalid_argument("Unsupported native display preferences.");
            presentation = ViewerPresentationPreferences::parse(options.value("display").toObject());
        }
        if (decoderPreference != "software" && QGuiApplication::platformName() == "xcb") {
            gpu = new GpuVideoWidget(owner);
            gpu->hide();
            gpu->presentationFailed = [this] {
                gpuDisplayed = false;
                gpu->hide();
                owner->update();
            };
        }
        horizontalScroll.setObjectName("nativeDesktopHorizontalScroll");
        verticalScroll.setObjectName("nativeDesktopVerticalScroll");
        horizontalScroll.setCursor(Qt::ArrowCursor);
        verticalScroll.setCursor(Qt::ArrowCursor);
        horizontalScroll.setFocusPolicy(Qt::NoFocus);
        verticalScroll.setFocusPolicy(Qt::NoFocus);
        horizontalScroll.hide();
        verticalScroll.hide();
        const auto scrolled = [this] {
            if (gpu)
                gpu->setDestination(imageRect());
            refreshCursor();
            owner->update();
        };
        QObject::connect(&horizontalScroll, &QScrollBar::valueChanged, owner, scrolled);
        QObject::connect(&verticalScroll, &QScrollBar::valueChanged, owner, scrolled);
        control.setStandardErrorFile(QProcess::nullDevice());
        identity.setStandardErrorFile(QProcess::nullDevice());
        startup.setSingleShot(true);
        clipboardDeadline.setSingleShot(true);
        clipboardDeadline.setTimerType(Qt::PreciseTimer);
        clipboardDeadline.setInterval(3000);
        QObject::connect(&clipboardDeadline, &QTimer::timeout, owner, [this] {
            fail(owner->tr("Clipboard synchronization timed out. Pending input was discarded."));
        });
        if (capture.clipboardImages || capture.clipboardFiles) {
            clipboardStatus = new QLabel(owner);
            clipboardStatus->setObjectName("nativeDesktopClipboardStatus");
            clipboardStatus->setAttribute(Qt::WA_TransparentForMouseEvents);
            clipboardStatus->setStyleSheet("QLabel { background: #20242c; color: white; padding: 8px; }");
            clipboardStatus->hide();
            clipboardProgress = new QProgressBar(owner);
            clipboardProgress->setObjectName("nativeDesktopClipboardProgress");
            clipboardProgress->setAttribute(Qt::WA_TransparentForMouseEvents);
            clipboardProgress->setTextVisible(false);
            clipboardProgress->setFixedHeight(6);
            clipboardProgress->hide();
            automaticPastePoll.setInterval(25);
            QObject::connect(&automaticPastePoll, &QTimer::timeout, owner, [this] { tryAutomaticPaste(); });
            richClipboard = std::make_unique<RichClipboardTransfer>(channel,
                capture.clipboardImages, capture.clipboardFiles, [this] {
                    return active && mode == "control" && nativeReady && !closing
                        && owner->isVisible() && owner->hasFocus() && owner->isActiveWindow()
                        && leaseExpiry > QDateTime::currentDateTimeUtc()
                        && QDateTime::fromString(intent.value("deadline").toString(), Qt::ISODateWithMs)
                            > QDateTime::currentDateTimeUtc();
                }, owner);
            QObject::connect(richClipboard.get(), &RichClipboardTransfer::status, owner, [this](const QString &text) {
                showClipboardStatus(text);
                setStatus(text);
            });
            QObject::connect(richClipboard.get(), &RichClipboardTransfer::incomingOffer,
                owner, [this] {
                    cancelAutomaticPaste();
                    retireClipboardBarrier();
                });
            QObject::connect(richClipboard.get(), &RichClipboardTransfer::invalidated,
                owner, [this] { cancelAutomaticPaste(); });
            QObject::connect(richClipboard.get(), &RichClipboardTransfer::outgoingCommitted,
                owner, [this](qint64 generation, quint64 epoch) {
                    automaticPaste.outgoingCommitted(pasteBinding(generation, epoch));
                    tryAutomaticPaste();
                });
            QObject::connect(richClipboard.get(), &RichClipboardTransfer::progress,
                owner, [this](int percent) {
                    if (percent == -2) {
                        clipboardProgress->hide();
                        return;
                    }
                    clipboardProgress->setRange(0, percent < 0 ? 0 : 100);
                    if (percent >= 0)
                        clipboardProgress->setValue(qBound(0, percent, 100));
                    layoutClipboardProgress();
                    clipboardProgress->show();
                    clipboardProgress->raise();
                });
            owner->setAcceptDrops(capture.clipboardFiles && mode == "control");
        }
        if (capture.clipboardText || richClipboard) {
            QObject::connect(QGuiApplication::clipboard(), &QClipboard::changed, owner, [this](QClipboard::Mode changedMode) {
                if (changedMode != QClipboard::Clipboard || applyingClipboard)
                    return;
                if (richClipboard && richClipboard->ownsClipboard())
                    return;
                const bool eligible = active && nativeReady && !closing && owner->isVisible()
                    && owner->hasFocus() && owner->isActiveWindow();
                if (!eligible)
                    return;
                const auto *mime = QGuiApplication::clipboard()->mimeData();
                const auto kind = mime ? clipboardSelectionKind(*mime) : ClipboardSelectionKind::Unsupported;
                if (kind == ClipboardSelectionKind::Text) {
                    if (richClipboard)
                        richClipboard->superseded();
                    if (clipboardArmed)
                        offerClipboard(false);
                } else if (richClipboard) {
                    retireClipboardBarrier();
                    richClipboard->changed();
                } else if (clipboardArmed) {
                    offerClipboard(false);
                }
            });
        }
        motion.setSingleShot(true);
        motion.setInterval(16);
        closePoll.setInterval(25);
        QObject::connect(&closePoll, &QTimer::timeout, owner, [this] { advanceClose(); });
        QObject::connect(&motion, &QTimer::timeout, owner, [this] { flushMotion(); });
        QObject::connect(&startup, &QTimer::timeout, owner, [this] {
            fail(owner->tr("The native desktop connection timed out."));
        });
        QObject::connect(&control, &QProcess::readyReadStandardOutput, owner, [this] { readControl(); });
        QObject::connect(&control, &QProcess::finished, owner, [this](int, QProcess::ExitStatus) {
            readControl();
            if (!closing)
                fail(owner->tr("The desktop control connection ended. Start a new session to reconnect."));
        });
        QObject::connect(&control, &QProcess::errorOccurred, owner, [this](QProcess::ProcessError) {
            if (!closing)
                fail(owner->tr("Could not start the trusted desktop control client."));
        });
        QObject::connect(&identity, &QProcess::readyReadStandardOutput, owner, [this] {
            identityOutput.append(identity.read(4097));
            if (identityOutput.size() > 4096)
                fail(owner->tr("Invalid native endpoint identity response."));
        });
        QObject::connect(&identity, &QProcess::finished, owner, [this](int code, QProcess::ExitStatus exit) {
            if (closing)
                return;
            if (identityOutput.size() > 4096 || identity.bytesAvailable() > 4096 - identityOutput.size()) {
                fail(owner->tr("Invalid native endpoint identity response."));
                return;
            }
            identityOutput.append(identity.readAllStandardOutput());
            const auto value = QJsonDocument::fromJson(identityOutput).object();
            localFingerprint = value.value("fingerprint").toString();
            if (code || exit != QProcess::NormalExit || value.size() != 1 || !digest(localFingerprint)) {
                fail(owner->tr("Could not create the native endpoint identity."));
                return;
            }
            bindingSent = true;
            sendControl({{"type", "signal"}, {"signal", QJsonObject{{"reference", reference},
                {"sequence", static_cast<qint64>(++outgoingSignal)}, {"type", "binding"}, {"fingerprint", localFingerprint}}}});
            startChannel();
        });
        QObject::connect(&identity, &QProcess::errorOccurred, owner, [this](QProcess::ProcessError) {
            if (!closing)
                fail(owner->tr("Could not run the native endpoint identity helper."));
        });
        QObject::connect(&channel, &ProcessChannel::connected, owner, [this] {
            if (closing)
                return;
            nativeReady = true;
            if (gpu) {
                gpu->setGeometry(owner->rect());
                gpu->show();
            }
            refreshCursor();
            startup.stop();
            setStatus(mode == "control" ? owner->tr("Connected · encrypted relay · H264 · control · release keyboard: Ctrl+Alt+Shift+Esc")
                                        : owner->tr("Connected · encrypted relay · H264 · view only"));
            channel.send(3, QByteArray(1, '\1'));
            refreshClipboard();
        });
        QObject::connect(&channel, &ProcessChannel::failed, owner, [this](const QString &message) { fail(message); });
        QObject::connect(&channel, &ProcessChannel::packet, owner, [this](quint8 kind, const QByteArray &payload) {
            if (closing)
                return;
            if (kind == 5 && nativeReady && richClipboard) {
                try {
                    richClipboard->receive(payload);
                } catch (const std::exception &) {
                    fail(owner->tr("Clipboard transfer protocol failed."));
                }
                return;
            }
            if (kind == 4 && nativeReady) {
                receiveClipboard(payload);
                return;
            }
            if (kind != 1 || !nativeReady) {
                fail(owner->tr("Invalid native desktop media direction."));
                return;
            }
            try {
                auto frame = FrameCodec::decode(reinterpret_cast<const std::uint8_t *>(payload.constData()), payload.size(), capture.bounds());
                if (!capture.acceptsFrameGeometry(frame.width, frame.height))
                    throw std::invalid_argument("Frame exceeds the agreed capture profile.");
                if (frame.sequence <= receivedFrame)
                    throw std::invalid_argument("Replayed frame.");
                receivedFrame = frame.sequence;
                {
                    std::lock_guard lock(decoderMutex);
                    if (pendingFrames.size() == MaximumPendingFrames)
                        pendingFrames.clear(); // The decoder handles the sequence gap at the next IDR.
                    pendingFrames.push_back(std::move(frame));
                }
                decoderChanged.notify_one();
            } catch (const std::exception &) {
                fail(owner->tr("The native desktop frame did not match its session contract."));
            }
        });
    }

    void layoutClipboardProgress()
    {
        if (!clipboardProgress || !clipboardStatus)
            return;
        const int available = qMax(1, owner->width() - 24);
        clipboardProgress->setGeometry(12, 12 + clipboardStatus->height(), qMin(360, available), 6);
    }

    void showClipboardStatus(const QString &message)
    {
        if (!clipboardStatus)
            return;
        clipboardStatus->setText(message);
        clipboardStatus->setWordWrap(true);
        clipboardStatus->setMaximumWidth(qMax(1, owner->width() - 24));
        clipboardStatus->adjustSize();
        clipboardStatus->move(12, 12);
        clipboardStatus->show();
        clipboardStatus->raise();
        layoutClipboardProgress();
    }

    ClipboardPasteBinding pasteBinding(qint64 generation, quint64 epoch) const
    {
        return {reference, generation, epoch, presentation.swapControlCommand};
    }

    bool automaticPasteEligible() const
    {
        const auto now = QDateTime::currentDateTimeUtc();
        return richClipboard && richClipboard->isAdmitted() && active && mode == "control"
            && nativeReady && !closing && !clipboardPending && owner->isVisible()
            && owner->hasFocus() && owner->isActiveWindow() && leaseExpiry > now
            && QDateTime::fromString(intent.value("deadline").toString(), Qt::ISODateWithMs) > now;
    }

    void cancelAutomaticPaste()
    {
        const bool pending = automaticPaste.pending();
        automaticPaste.cancel();
        automaticPastePoll.stop();
        if (pending && !closing)
            showClipboardStatus(owner->tr("Automatic paste cancelled."));
    }

    void queueAutomaticPaste(ClipboardPasteChord chord)
    {
        cancelAutomaticPaste();
        if (!automaticPasteEligible())
            return;
        const auto binding = pasteBinding(richClipboard->selectionGeneration(), richClipboard->focusEpoch());
        if (!automaticPaste.request(binding, std::move(chord), richClipboard->outgoingReady()))
            return;
        automaticPasteElapsed.start();
        automaticPastePoll.start();
        showClipboardStatus(owner->tr("Transferring clipboard… will paste into the active remote app."));
    }

    void queueAutomaticPaste(const QKeyEvent &event)
    {
        const auto usage = keyboard.hidForNativeKey(event);
        if (!usage)
            return;
        ClipboardPasteChord chord;
        chord.key = mapControlCommandUsage(*usage, presentation.swapControlCommand);
        const auto modifiers = event.modifiers();
        if (modifiers.testFlag(Qt::ControlModifier))
            chord.modifiers.append(mapControlCommandUsage(224, presentation.swapControlCommand));
        if (modifiers.testFlag(Qt::ShiftModifier))
            chord.modifiers.append(225);
        if (modifiers.testFlag(Qt::MetaModifier))
            chord.modifiers.append(mapControlCommandUsage(227, presentation.swapControlCommand));
        queueAutomaticPaste(std::move(chord));
    }

    void tryAutomaticPaste()
    {
        if (!automaticPaste.pending()) {
            automaticPastePoll.stop();
            return;
        }
        if (automaticPasteElapsed.elapsed() >= 600000) {
            cancelAutomaticPaste();
            return;
        }
        const auto binding = pasteBinding(richClipboard->selectionGeneration(), richClipboard->focusEpoch());
        const bool released = keys.isEmpty() && sentKeys.isEmpty() && buttons.isEmpty()
            && sentButtons.isEmpty() && suppressedPasteKeys.isEmpty()
            // Qt's cached modifiers can remain set after native key release.
            // Query current platform state only after all tracked input drained.
            && QGuiApplication::queryKeyboardModifiers() == Qt::NoModifier
            && QGuiApplication::mouseButtons() == Qt::NoButton;
        const auto chord = automaticPaste.take(binding, automaticPasteEligible(), released);
        if (!automaticPaste.pending())
            automaticPastePoll.stop();
        if (!chord)
            return;
        // Consume before the first event. A partial write closes the session;
        // this original gesture is never retried or synthesized as local input.
        flushMotion();
        showClipboardStatus(owner->tr("Pasting into the active remote app…"));
        for (const auto modifier : chord->modifiers) {
            sendInput({{"kind", "key"}, {"hid_usage", modifier}, {"down", true}});
            if (closing)
                return;
        }
        sendInput({{"kind", "key"}, {"hid_usage", chord->key}, {"down", true}});
        if (closing)
            return;
        sendInput({{"kind", "key"}, {"hid_usage", chord->key}, {"down", false}});
        if (closing)
            return;
        for (auto modifier = chord->modifiers.crbegin(); modifier != chord->modifiers.crend(); ++modifier) {
            sendInput({{"kind", "key"}, {"hid_usage", *modifier}, {"down", false}});
            if (closing)
                return;
        }
        showClipboardStatus(owner->tr("Paste sent."));
    }

    void setStatus(const QString &message)
    {
        status = message;
        owner->update();
        if (owner->statusChanged)
            owner->statusChanged(message);
    }

    void fail(const QString &message)
    {
        if (closing)
            return;
        close();
        setStatus(message);
        if (owner->failed)
            owner->failed(message);
    }

    bool sendControl(const QJsonObject &message)
    {
        const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        if (bytes.size() > 4096 || control.bytesToWrite() > 65536 || control.write(bytes) != bytes.size()) {
            fail(owner->tr("The desktop control channel could not accept a bounded message."));
            return false;
        }
        return true;
    }

    void start()
    {
        if (started || closing)
            return;
        started = true;
        const auto deadline = QDateTime::currentDateTimeUtc().addSecs(1800);
        intent = {{"schema", "dockpipe.remote.desktop/v1"}, {"target", catalog.value("target")},
            {"epoch", catalog.value("epoch")}, {"provider", catalog.value("provider")}, {"mode", mode},
            {"nonce", QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128)},
            {"deadline", deadline.toString(Qt::ISODateWithMs)}};
        const auto request = directory.filePath("open.json");
        if (!writePrivate(request, intent)) {
            fail(owner->tr("Could not retain the exact desktop connection request."));
            return;
        }
        startDecoder();
        startup.start(30000);
        setStatus(owner->tr("Authorizing native desktop…"));
        control.start(cli, {"remote", "desktop", "--stdio", "--request", request,
            "--state", stateDirectory, "--auth", "never"});
    }

    void readControl()
    {
        while (control.bytesAvailable() > 0) {
            controlOutput.append(control.read(4096));
            if (controlOutput.size() > 1024 * 1024) {
                fail(owner->tr("The desktop control response exceeded its limit."));
                return;
            }
            qsizetype newline;
            while ((newline = controlOutput.indexOf('\n')) >= 0) {
                const auto line = controlOutput.left(newline);
                controlOutput.remove(0, newline + 1);
                QJsonParseError error{};
                const auto document = QJsonDocument::fromJson(line, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                    if (!closing)
                        fail(owner->tr("Invalid desktop control response."));
                    return;
                }
                consume(document.object());
            }
        }
    }

    void consume(const QJsonObject &message)
    {
        if (message.value("type") == "prepared" && !closing) {
            const auto candidate = message.value("reference").toObject();
            if (!reference.isEmpty() || candidate.value("schema") != intent.value("schema")
                || candidate.value("target") != intent.value("target") || candidate.value("epoch") != intent.value("epoch")
                || !digest(candidate.value("id").toString()) || message.value("provider") != intent.value("provider")
                || message.value("mode") != mode
                || QDateTime::fromString(message.value("deadline").toString(), Qt::ISODateWithMs)
                    != QDateTime::fromString(intent.value("deadline").toString(), Qt::ISODateWithMs)) {
                fail(owner->tr("The prepared desktop session changed its target or permission."));
                return;
            }
            reference = candidate;
            if (!remember(reference, false) || !sendControl({{"type", "prepared-ack"}, {"reference", reference}})) {
                fail(owner->tr("Could not retain native desktop recovery metadata."));
                return;
            }
            preparedAck = true;
            identity.start(helper, {"identity", "--directory", directory.filePath("identity")});
            return;
        }
        if (message.value("type") != "view") {
            if (!closing)
                fail(owner->tr("Unexpected native desktop control message."));
            return;
        }
        const auto view = message.value("view").toObject();
        if (reference.isEmpty() || view.value("reference").toObject() != reference) {
            if (!closing)
                fail(owner->tr("Desktop control belongs to a different session."));
            return;
        }
        const auto state = view.value("state").toString();
        if (state == "closed") {
            if (!view.value("lease_millis").isDouble() || view.value("lease_millis").toInteger(-1) != 0
                || (view.contains("signals") && !view.value("signals").isArray())
                || !view.value("signals").toArray().isEmpty()) {
                closeRejected = true;
                if (!closing)
                    fail(owner->tr("Invalid desktop cleanup response."));
                return;
            }
            nativeClosed = true;
            remember(reference, true);
            if (!closing) {
                close();
                setStatus(owner->tr("Desktop disconnected. Native cleanup verified."));
            }
            return;
        }
        if (closing) {
            if (state == "unknown")
                closeRejected = true;
            return;
        }
        const auto lease = view.value("lease_millis").toInteger();
        if ((state != "queued" && state != "opening" && state != "running") || lease <= 0 || lease > 10000) {
            fail(owner->tr("Desktop authorization ended; native cleanup may need inspection."));
            return;
        }
        const auto acknowledgment = view.value("ack").toInteger(-1);
        if (acknowledgment < 0 || static_cast<quint64>(acknowledgment) < acknowledgedSignal
            || static_cast<quint64>(acknowledgment) > outgoingSignal) {
            fail(owner->tr("The authenticated desktop signaling acknowledgment changed."));
            return;
        }
        acknowledgedSignal = static_cast<quint64>(acknowledgment);
        const auto expires = QDateTime::currentDateTimeUtc().addMSecs(lease);
        if (expires > leaseExpiry) {
            leaseExpiry = expires;
            channel.renew(expires);
        }
        const auto negotiationFrames = view.value("signals").toArray();
        for (const auto &entry : negotiationFrames) {
            const auto signal = entry.toObject();
            const auto sequence = signal.value("sequence").toInteger();
            const auto fingerprint = signal.value("fingerprint").toString();
            if (signal.value("reference").toObject() != reference || sequence != signalSequence + 1
                || signal.value("type") != "binding" || !digest(fingerprint)
                || (!peerFingerprint.isEmpty() && fingerprint != peerFingerprint)) {
                fail(owner->tr("The authenticated native endpoint binding changed."));
                return;
            }
            signalSequence = sequence;
            peerFingerprint = fingerprint;
        }
        startChannel();
    }

    void startChannel()
    {
        if (closing || channelStarted || !bindingSent || acknowledgedSignal < 1 || peerFingerprint.isEmpty()
            || localFingerprint.isEmpty() || leaseExpiry <= QDateTime::currentDateTimeUtc())
            return;
        const auto configuration = directory.filePath("channel.json");
        const QJsonObject value{{"schema", "dockpipe.desktop.channel/v2"}, {"reference", reference},
            {"role", "viewer"}, {"mode", mode}, {"capture", capture.json()}, {"local_fingerprint", localFingerprint},
            {"peer_fingerprint", peerFingerprint}, {"identity_directory", directory.filePath("identity")},
            {"cli", cli}, {"state_directory", stateDirectory}, {"deadline", intent.value("deadline")},
            {"lease_expires_at", leaseExpiry.toString(Qt::ISODateWithMs)}};
        if (!writePrivate(configuration, value)) {
            fail(owner->tr("Could not prepare the private encrypted channel."));
            return;
        }
        channelStarted = true;
        setStatus(owner->tr("Connecting the authenticated native endpoints…"));
        channel.start(helper, configuration, reference, "viewer", mode, capture.json());
    }

    void startDecoder()
    {
        decoderExited.store(false);
        decoderThread = std::thread([this] {
            // Declared before decoder: publish exit only after its destructor.
            struct ExitNotice {
                std::atomic<bool> &exited;
                ~ExitNotice() { exited.store(true); }
            } notice{decoderExited};
            std::unique_ptr<H264Decoder> decoder;
            quint64 previous = 0;
            bool waitForKeyframe = true;
            int failures = 0;
            bool softwareFallback = false;
            QString fallbackReason;
            quint64 decodedFrames = 0;
            quint64 firstPts = 0;
            quint64 lastPts = 0;
            bool timingMonotonic = true;
            std::chrono::steady_clock::time_point firstDecoded;
            std::chrono::steady_clock::time_point lastPresented;
            const auto shouldPresent = [&] {
                std::lock_guard lock(decoderMutex);
                if (decoderStopping)
                    return false;
                // Prefer the latest output during a burst, but refresh the
                // picture periodically if the compressed queue stays busy.
                return decodedFrames == 0 || pendingFrames.empty()
                    || std::chrono::steady_clock::now() - lastPresented >= std::chrono::milliseconds(33);
            };
            for (;;) {
                VideoFrame frame;
                {
                    std::unique_lock lock(decoderMutex);
                    decoderChanged.wait(lock, [this] { return decoderStopping || !pendingFrames.empty(); });
                    if (decoderStopping)
                        return;
                    frame = std::move(pendingFrames.front());
                    pendingFrames.pop_front();
                }
                if (previous && frame.sequence != previous + 1)
                    waitForKeyframe = true;
                previous = frame.sequence;
                if (waitForKeyframe && !frame.keyframe) {
                    requestKeyframe();
                    continue;
                }
                try {
                    if (!decoder) {
                        auto preference = H264Decoder::Preference::Software;
                        if (decoderPreference == "hardware")
                            preference = H264Decoder::Preference::Hardware;
                        else if (decoderPreference == "auto" && !softwareFallback)
                            preference = H264Decoder::Preference::Automatic;
                        decoder = std::make_unique<H264Decoder>(capture.bounds(), preference);
                    } else if (waitForKeyframe) {
                        decoder->resynchronize();
                    }
                    NativeVideoFrame nativeImage;
                    const auto acceptNative = [this, &nativeImage](NativeVideoFrame value) {
                        if (!gpu || !gpu->available())
                            return false;
                        nativeImage = std::move(value);
                        return true;
                    };
                    auto image = decoder->decode(frame, shouldPresent, nullptr, acceptNative);
                    if (decoderPreference == "auto" && !decoder->fallbackReason().empty()) {
                        softwareFallback = true;
                        fallbackReason = QString::fromStdString(decoder->fallbackReason());
                    }
                    if (image.isNull() && !nativeImage) {
                        waitForKeyframe = decoder->requiresKeyframe();
                        if (waitForKeyframe)
                            requestKeyframe();
                        else
                            failures = 0;
                        continue;
                    }
                    const auto decodedAt = std::chrono::steady_clock::now();
                    lastPresented = decodedAt;
                    if (decodedFrames == 0) {
                        firstDecoded = decodedAt;
                        firstPts = frame.microseconds;
                    } else if (frame.microseconds < lastPts) {
                        timingMonotonic = false;
                    }
                    ++decodedFrames;
                    lastPts = frame.microseconds;
                    const auto span = std::chrono::duration_cast<std::chrono::microseconds>(decodedAt - firstDecoded).count();
                    const QJsonObject actualDecoder{{"requested", decoderPreference},
                        {"backend", QString::fromStdString(decoder->backend())}, {"fallback_reason", fallbackReason}};
                    const QJsonObject timing{{"decoded_frames", static_cast<qint64>(decodedFrames)},
                        {"first_pts_us", static_cast<qint64>(firstPts)}, {"last_pts_us", static_cast<qint64>(lastPts)},
                        {"decode_span_us", static_cast<qint64>(span)}, {"timing_monotonic", timingMonotonic}};
                    waitForKeyframe = false;
                    failures = 0;
                    bool notify = false;
                    {
                        std::lock_guard lock(decoderMutex);
                        if (decoderStopping)
                            return;
                        pendingImage = std::move(image);
                        pendingNative = std::move(nativeImage);
                        pendingDecoderState = actualDecoder;
                        pendingDecodedTiming = timing;
                        if (!imageQueued) {
                            imageQueued = true;
                            notify = true;
                        }
                    }
                    if (notify) {
                        QMetaObject::invokeMethod(owner, [this, lifetime = callbacksAlive] {
                            if (!lifetime->load())
                                return;
                            NativeVideoFrame native;
                            {
                                std::lock_guard lock(decoderMutex);
                                native = std::move(pendingNative);
                                displayed = std::move(pendingImage);
                                displaySize = native ? QSize(native->width, native->height) : displayed.size();
                                gpuDisplayed = static_cast<bool>(native);
                                decoderState = std::move(pendingDecoderState);
                                decodedTiming = std::move(pendingDecodedTiming);
                                imageQueued = false;
                            }
                            layoutPresentation();
                            if (gpu) {
                                if (native) {
                                    gpu->show();
                                    gpu->present(std::move(native), imageRect());
                                } else {
                                    gpu->hide();
                                }
                            }
                            refreshCursor();
                            if (!closing)
                                owner->update();
                        }, Qt::QueuedConnection);
                    }
                } catch (const std::exception &error) {
                    const QString reason = QString::fromUtf8(error.what()).left(192);
                    if (decoderPreference == "hardware" && dynamic_cast<const std::invalid_argument *>(&error) == nullptr) {
                        const QString code = reason.contains("unavailable") ? "hardware-unavailable" : "hardware-failed";
                        QMetaObject::invokeMethod(owner, [this, lifetime = callbacksAlive, code] {
                            if (!lifetime->load())
                                return;
                            decoderFailureState = {{"code", code}};
                            fail(QStringLiteral("Hardware H264 decoding failed."));
                        }, Qt::QueuedConnection);
                        return;
                    }
                    decoder.reset();
                    waitForKeyframe = true;
                    if (++failures >= 4) {
                        QMetaObject::invokeMethod(owner, [this, lifetime = callbacksAlive] {
                            if (!lifetime->load())
                                return;
                            fail(owner->tr("The native H264 stream could not be decoded within its contract."));
                        }, Qt::QueuedConnection);
                        return;
                    }
                    requestKeyframe();
                }
            }
        });
    }

    void requestKeyframe()
    {
        {
            std::lock_guard lock(decoderMutex);
            if (keyframeQueued || decoderStopping)
                return;
            keyframeQueued = true;
        }
        QMetaObject::invokeMethod(owner, [this, lifetime = callbacksAlive] {
            if (!lifetime->load())
                return;
            {
                std::lock_guard lock(decoderMutex);
                keyframeQueued = false;
            }
            if (!closing && channel.ready())
                channel.send(3, QByteArray(1, '\1'));
            refreshClipboard();
        }, Qt::QueuedConnection);
    }

    void layoutPresentation()
    {
        imageViewport = owner->rect();
        if (gpu)
            gpu->setGeometry(owner->rect());
        if (closing || displaySize.isEmpty() || presentation.scale == "fit") {
            horizontalScroll.hide();
            verticalScroll.hide();
            horizontalScroll.setRange(0, 0);
            verticalScroll.setRange(0, 0);
            if (gpu)
                gpu->setDestination(imageRect());
            return;
        }
        const int extent = qBound(8, owner->style()->pixelMetric(QStyle::PM_ScrollBarExtent), 40);
        bool horizontal = displaySize.width() > owner->width();
        bool vertical = displaySize.height() > owner->height();
        for (int pass = 0; pass < 2; ++pass) {
            horizontal = displaySize.width() > owner->width() - (vertical ? extent : 0);
            vertical = displaySize.height() > owner->height() - (horizontal ? extent : 0);
        }
        const int width = qMax(0, owner->width() - (vertical ? extent : 0));
        const int height = qMax(0, owner->height() - (horizontal ? extent : 0));
        imageViewport = QRect(0, 0, width, height);
        if (gpu)
            gpu->setGeometry(imageViewport);
        horizontalScroll.raise();
        verticalScroll.raise();
        horizontalScroll.setGeometry(0, height, width, extent);
        verticalScroll.setGeometry(width, 0, extent, height);
        horizontalScroll.setPageStep(width);
        verticalScroll.setPageStep(height);
        horizontalScroll.setRange(0, qMax(0, displaySize.width() - width));
        verticalScroll.setRange(0, qMax(0, displaySize.height() - height));
        horizontalScroll.setVisible(horizontal);
        verticalScroll.setVisible(vertical);
        if (gpu)
            gpu->setDestination(imageRect());
    }

    QRect imageRect() const
    {
        if (displaySize.isEmpty())
            return {};
        if (presentation.scale == "actual") {
            const int left = displaySize.width() <= imageViewport.width()
                ? (imageViewport.width() - displaySize.width()) / 2 : -horizontalScroll.value();
            const int top = displaySize.height() <= imageViewport.height()
                ? (imageViewport.height() - displaySize.height()) / 2 : -verticalScroll.value();
            return QRect(QPoint(left, top), displaySize);
        }
        const auto size = displaySize.scaled(imageViewport.size(), Qt::KeepAspectRatio);
        return QRect(QPoint((imageViewport.width() - size.width()) / 2,
                           (imageViewport.height() - size.height()) / 2), size);
    }

    void refreshCursor()
    {
        const auto position = owner->mapFromGlobal(hoverGlobalPosition);
        const bool hide = presentation.hideCursor && pointerOverSurface && nativeReady && !closing
            && !displaySize.isEmpty() && imageViewport.contains(position) && imageRect().contains(position);
        if (hide && !localCursorHidden) {
            owner->setCursor(Qt::BlankCursor);
            localCursorHidden = true;
        } else if (!hide && localCursorHidden) {
            owner->unsetCursor();
            localCursorHidden = false;
        }
    }

    void hover(const QPointF &position)
    {
        hoverGlobalPosition = owner->mapToGlobal(position.toPoint());
        pointerOverSurface = owner->rect().contains(position.toPoint());
        refreshCursor();
    }

    void leave()
    {
        pointerOverSurface = false;
        refreshCursor();
    }

    bool movePointer(const QPointF &position)
    {
        const auto rectangle = imageRect();
        if (rectangle.isEmpty() || !imageViewport.contains(position.toPoint()) || !rectangle.contains(position.toPoint()))
            return false;
        pointer = QPointF((position.x() - rectangle.x()) / rectangle.width(),
                          (position.y() - rectangle.y()) / rectangle.height());
        return true;
    }

    bool sendClipboard(ClipboardOperation operation, quint64 revision = 0, const QByteArray &text = {})
    {
        const auto encoded = encodeClipboardRecord({operation, clipboardEpoch, revision,
            std::string(text.constData(), text.size())});
        if (channel.send(4, QByteArray(reinterpret_cast<const char *>(encoded.data()), encoded.size())))
            return true;
        fail(owner->tr("Clipboard synchronization was interrupted. Pending input was discarded."));
        return false;
    }

    void cancelClipboard()
    {
        clipboardDeadline.stop();
        clipboardPending = false;
        nextClipboard.reset();
        incomingClipboard.reset();
        blockedInput.clear();
        pendingMotion = {};
        motion.stop();
        const bool wasArmed = clipboardArmed;
        clipboardArmed = false;
        if (wasArmed && !closing && nativeReady)
            sendClipboard(ClipboardOperation::Pause);
    }

    void refreshClipboard()
    {
        if (richClipboard)
            richClipboard->refresh();
        const bool eligible = active && capture.clipboardText && mode == "control" && nativeReady
            && !closing && owner->isVisible() && owner->hasFocus() && owner->isActiveWindow();
        if (!eligible) {
            cancelClipboard();
            return;
        }
        if (clipboardArmed)
            return;
        clipboardArmed = true;
        ++clipboardEpoch;
        clipboardRevision = 0;
        remoteClipboardRevision = 0;
        // Watch establishes a baseline. Focusing must not upload existing text.
        sendClipboard(ClipboardOperation::Watch);
    }

    void writeClipboard(const QByteArray &text)
    {
        clipboardPending = true;
        if (!clipboardDeadline.isActive()) {
            clipboardElapsed.start();
            clipboardDeadline.start();
        }
        sendClipboard(ClipboardOperation::Write, ++clipboardRevision, text);
    }

    void offerClipboard(bool paste)
    {
        refreshClipboard();
        if (!clipboardArmed)
            return;
        const auto *mime = QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
        if (!mime || clipboardSelectionKind(*mime) != ClipboardSelectionKind::Text) {
            nextClipboard.reset();
            if (paste || clipboardPending)
                fail(owner->tr("The clipboard has no supported text. Pending input was discarded."));
            else
                setStatus(owner->tr("The clipboard has no supported text. The remote clipboard was preserved."));
            return;
        }
        const auto text = mime->text();
        const auto bytes = text.toUtf8();
        if (bytes.size() > qsizetype(ClipboardMaximumTextBytes) || text.contains(QChar(0))
            || QString::fromUtf8(bytes) != text) {
            nextClipboard.reset();
            if (paste || clipboardPending)
                fail(owner->tr("The clipboard text is unsupported or too large. Pending input was discarded."));
            else
                setStatus(owner->tr("The clipboard text is unsupported or too large. The remote clipboard was preserved."));
            return;
        }
        if (incomingClipboard && *incomingClipboard == bytes) {
            incomingClipboard.reset();
            if (!paste)
                return;
        }
        incomingClipboard.reset();
        if (clipboardPending)
            nextClipboard = bytes;
        else
            writeClipboard(bytes);
    }

    void receiveClipboard(const QByteArray &payload)
    {
        try {
            const auto record = decodeClipboardRecord(reinterpret_cast<const std::uint8_t *>(payload.constData()), payload.size());
            if (!capture.clipboardText || mode != "control" || !clipboardDirectionAllowed(record.operation, true)
                || record.epoch > clipboardEpoch || !clipboardEpoch)
                throw std::invalid_argument("Unnegotiated clipboard epoch.");
            refreshClipboard();
            if (record.epoch < clipboardEpoch || !clipboardArmed)
                return;
            if (clipboardPending && clipboardElapsed.elapsed() >= 3000)
                throw std::invalid_argument("Clipboard acknowledgment missed its deadline.");
            if (record.operation == ClipboardOperation::Changed) {
                if (record.revision != remoteClipboardRevision + 1)
                    throw std::invalid_argument("Unexpected clipboard revision.");
                remoteClipboardRevision = record.revision;
                // A Changed record emitted before this Write may already be in
                // transit. Consume its revision without replacing offered text.
                if (clipboardPending)
                    return;
                incomingClipboard = QByteArray(record.text.data(), record.text.size());
                if (richClipboard)
                    richClipboard->superseded();
                applyingClipboard = true;
                QGuiApplication::clipboard()->setText(QString::fromUtf8(record.text.data(), record.text.size()), QClipboard::Clipboard);
                applyingClipboard = false;
            } else if (record.operation == ClipboardOperation::Acknowledged) {
                if (!clipboardPending || record.revision != clipboardRevision)
                    throw std::invalid_argument("Unexpected clipboard acknowledgment.");
                clipboardPending = false;
                if (nextClipboard) {
                    const auto text = std::move(*nextClipboard);
                    nextClipboard.reset();
                    writeClipboard(text);
                    return;
                }
                clipboardDeadline.stop();
                auto queued = std::move(blockedInput);
                blockedInput.clear();
                for (auto &event : queued) {
                    if (closing)
                        break;
                    sendInput(std::move(event));
                }
            } else if (record.operation == ClipboardOperation::Notice) {
                // Notices carry no write revision. A delayed poll notice cannot
                // acknowledge or reject a newer outstanding Write.
                setStatus(owner->tr("Clipboard text could not be shared. The receiving clipboard was preserved."));
            }
        } catch (const std::exception &) {
            fail(owner->tr("Clipboard synchronization failed. Pending input was discarded."));
        }
    }

    void input(QJsonObject event)
    {
        if (closing || !nativeReady || mode != "control")
            return;
        if (clipboardPending) {
            // Preserve physical event ordering during the acknowledgment barrier,
            // including moves, and charge each event against the same 128 limit.
            flushMotion();
            sendInput(std::move(event));
            return;
        }
        if (event.value("kind") == "move") {
            // Keep one latest position. High polling-rate mice must not create
            // a remote input backlog or delay discrete key/button transitions.
            pendingMotion = std::move(event);
            if (!motion.isActive())
                motion.start();
            return;
        }
        flushMotion();
        sendInput(std::move(event));
    }

    void flushMotion()
    {
        motion.stop();
        auto event = std::move(pendingMotion);
        pendingMotion = {};
        if (!event.isEmpty())
            sendInput(std::move(event));
    }

    void sendInput(QJsonObject event)
    {
        if (closing || !nativeReady || mode != "control")
            return;
        if (clipboardPending) {
            if (clipboardElapsed.elapsed() >= 3000 || blockedInput.size() >= 128) {
                fail(owner->tr("Clipboard input queue overflowed. Pending input was discarded."));
                return;
            }
            blockedInput.push_back(std::move(event));
            return;
        }
        event.insert("sequence", static_cast<qint64>(++inputSequence));
        const auto bytes = QJsonDocument(event).toJson(QJsonDocument::Compact);
        if (!channel.send(2, bytes)) {
            fail(owner->tr("Desktop input was interrupted. The session was closed without replay."));
            return;
        }
        const auto kind = event.value("kind").toString();
        if (kind == "key" || kind == "button") {
            auto &held = kind == "key" ? sentKeys : sentButtons;
            const int code = event.value(kind == "key" ? "hid_usage" : "button").toInt();
            if (event.value("down").toBool())
                held.insert(code);
            else
                held.remove(code);
        }
    }

    void retireClipboardBarrier()
    {
        if (clipboardPending) {
            const bool heldV = keys.contains(25);
            const bool heldInsert = keys.contains(73);
            releaseHeldInput();
            if (heldV)
                suppressedPasteKeys.insert(Qt::Key_V);
            if (heldInsert)
                suppressedPasteKeys.insert(Qt::Key_Insert);
        } else {
            cancelClipboard();
        }
    }

    void releaseInput()
    {
        cancelAutomaticPaste();
        if (richClipboard)
            richClipboard->pause();
        releaseHeldInput();
    }

    void releaseHeldInput()
    {
        cancelClipboard();
        const auto heldKeys = sentKeys;
        sentKeys.clear();
        keys.clear();
        suppressedPasteKeys.clear();
        for (int key : heldKeys)
            sendInput({{"kind", "key"}, {"hid_usage", key}, {"down", false}});
        const auto heldButtons = sentButtons;
        sentButtons.clear();
        buttons.clear();
        for (int button : heldButtons)
            sendInput({{"kind", "button"}, {"button", button}, {"down", false}, {"x", pointer.x()}, {"y", pointer.y()}});
        keyboard.reset();
        owner->releaseKeyboard();
    }

    void beginClose()
    {
        if (closing)
            return;
        closing = true;
        if (richClipboard)
            richClipboard->stop();
        leave();
        callbacksAlive->store(false);
        // No more input or renewals. The worker releases owned input on revoke.
        releaseInput();
        startup.stop();
        motion.stop();
        pendingMotion = {};
        {
            std::lock_guard lock(decoderMutex);
            decoderStopping = true;
            pendingFrames.clear();
            pendingNative.reset();
            pendingImage = {};
        }
        decoderChanged.notify_one();
        if (control.state() != QProcess::NotRunning)
            control.write("{\"type\":\"close\"}\n");
        displayed = {};
        displaySize = {};
        gpuDisplayed = false;
        if (gpu)
            gpu->reset();
        layoutPresentation();
        owner->update();
    }

    void requestClose()
    {
        if (synchronousClosed)
            return;
        if (closeCompleted) {
            QTimer::singleShot(0, owner, [this] {
                if (!synchronousClosed)
                    emit owner->closeCompleted(closeResult);
            });
            return;
        }
        if (closeRequested)
            return;
        closeRequested = true;
        beginClose();
        closeElapsed.start();
        channel.closeAsync();
        if (identity.state() != QProcess::NotRunning)
            identity.kill();
        closePoll.start();
        advanceClose();
    }

    void advanceClose()
    {
        if (closeCompleted)
            return;
        const auto elapsed = closeElapsed.elapsed();
        if (elapsed >= 10000)
            closeTimedOut = true;
        if ((nativeClosed || closeRejected || closeTimedOut) && !controlTerminated) {
            controlTerminated = true;
            control.closeWriteChannel();
            if (!nativeClosed && control.state() != QProcess::NotRunning)
                control.terminate();
        }
        if (elapsed >= 11000 && control.state() != QProcess::NotRunning)
            control.kill();
        const bool localStopped = (!richClipboard || richClipboard->stopped())
            && channel.stopped() && decoderExited.load()
            && identity.state() == QProcess::NotRunning && control.state() == QProcess::NotRunning;
        if (!localStopped && elapsed < CloseCompletionMilliseconds)
            return;
        if (decoderExited.load() && decoderThread.joinable())
            decoderThread.join();
        closePoll.stop();
        closeCompleted = true;
        const bool verified = localStopped && nativeClosed && !closeRejected && !closeTimedOut
            && !reference.isEmpty();
        closeResult = {{"schema", "dockpipe.desktop.close/v1"}, {"reference", reference},
                       {"outcome", verified ? "closed" : "unknown"}};
        emit owner->closeCompleted(closeResult);
    }

    void close()
    {
        // Published DisplaySDK1.0 remains synchronous: no callbacks after return.
        synchronousClosed = true;
        closePoll.stop();
        beginClose();
        channel.close();
        if (richClipboard)
            richClipboard->wait();
        if (decoderThread.joinable())
            decoderThread.join();
        if (identity.state() != QProcess::NotRunning) {
            identity.kill();
            identity.waitForFinished(1000);
        }
        if (control.state() != QProcess::NotRunning) {
            // Legacy consumers receive a short bounded observation, retaining
            // exact uncertainty when they do not use requestClose beforehand.
            QElapsedTimer observation;
            observation.start();
            while (control.state() != QProcess::NotRunning && !nativeClosed
                && observation.elapsed() < CloseObservationMilliseconds) {
                const auto remaining = CloseObservationMilliseconds - observation.elapsed();
                control.waitForReadyRead(static_cast<int>(qMin<qint64>(100, remaining)));
                readControl();
            }
            control.closeWriteChannel();
            if (!control.waitForFinished(500)) {
                control.terminate();
                if (!control.waitForFinished(500)) {
                    control.kill();
                    control.waitForFinished(1000);
                }
            }
        }
        if (!preparedAck && !reference.isEmpty())
            remember(reference, true);
    }

};

DesktopViewerSurface::DesktopViewerSurface(const QJsonObject &options, const QString &helper, QWidget *parent)
    : LauncherDisplaySurface(parent), m_impl(std::make_unique<Impl>(this, options, helper))
{
    setObjectName("nativeDesktopViewer");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_InputMethodEnabled, false);
}

DesktopViewerSurface::~DesktopViewerSurface()
{
    closeDisplay();
}

void DesktopViewerSurface::setActive(bool active)
{
    m_impl->active = active;
    if (active) {
        m_impl->start();
        m_impl->refreshClipboard();
    } else {
        m_impl->close();
    }
}

void DesktopViewerSurface::closeDisplay()
{
    m_impl->close();
}

int DesktopViewerSurface::closeCompletionBudgetMs() const
{
    return CloseCompletionMilliseconds;
}

QJsonObject DesktopViewerSurface::closeReference() const
{
    return m_impl->reference;
}

void DesktopViewerSurface::requestClose()
{
    m_impl->requestClose();
}

void DesktopViewerSurface::setPresentationOptions(const QJsonObject &options)
{
    if (m_impl->closing)
        return;
    try {
        const auto presentation = ViewerPresentationPreferences::parse(options);
        const bool modifierPolicyChanged = presentation.swapControlCommand != m_impl->presentation.swapControlCommand;
        if (modifierPolicyChanged)
            m_impl->releaseInput();
        m_impl->presentation = presentation;
        if (modifierPolicyChanged)
            m_impl->refreshClipboard();
    } catch (const std::invalid_argument &) {
        return;
    }
    m_impl->layoutPresentation();
    m_impl->refreshCursor();
    update();
}

QJsonObject DesktopViewerSurface::presentationOptions() const
{
    return m_impl->presentation.json();
}

QJsonObject DesktopViewerSurface::captureObservation() const
{
    if (m_impl->closing || !m_impl->nativeReady || m_impl->displaySize.isEmpty())
        return {};
    auto result = m_impl->decodedTiming;
    result.insert("capture", m_impl->capture.json());
    result.insert("width", m_impl->displaySize.width());
    result.insert("height", m_impl->displaySize.height());
    result.insert("decoder", m_impl->decoderState);
    if (m_impl->gpuDisplayed && m_impl->gpu)
        result.insert("gpu_presentation", m_impl->gpu->observation());
    return result;
}

QJsonObject DesktopViewerSurface::decoderStatus() const
{
    if (m_impl->closing || m_impl->displaySize.isEmpty())
        return {};
    return m_impl->decoderState;
}

QJsonObject DesktopViewerSurface::decoderFailure() const
{
    // Failure diagnostics remain available to the failed callback during close.
    // A surface is single-use; disposal removes these fixed metadata only codes.
    return m_impl->decoderFailureState;
}

void DesktopViewerSurface::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12, 16, 22));
    if (!m_impl->displaySize.isEmpty()) {
        painter.setClipRect(m_impl->imageViewport);
        if (!m_impl->gpuDisplayed)
            painter.drawImage(m_impl->imageRect(), m_impl->displayed);
    } else {
        painter.setPen(Qt::white);
        painter.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, m_impl->status);
    }
}

void DesktopViewerSurface::enterEvent(QEnterEvent *event)
{
    m_impl->hover(event->position());
    QWidget::enterEvent(event);
}

void DesktopViewerSurface::leaveEvent(QEvent *event)
{
    m_impl->cancelAutomaticPaste();
    m_impl->leave();
    QWidget::leaveEvent(event);
}

void DesktopViewerSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Re-map the last hover event when layout/image fit changes without mouse
    // motion. Never move or globally override the operating-system cursor.
    m_impl->layoutPresentation();
    m_impl->layoutClipboardProgress();
    if (m_impl->gpu)
        m_impl->gpu->setDestination(m_impl->imageRect());
    m_impl->refreshCursor();
}

void DesktopViewerSurface::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    m_impl->refreshClipboard();
    if (underMouse()) {
        m_impl->pointerOverSurface = true;
        m_impl->refreshCursor();
    }
}

void DesktopViewerSurface::mouseMoveEvent(QMouseEvent *event)
{
    m_impl->hover(event->position());
    if (m_impl->movePointer(event->position()))
        m_impl->input({{"kind", "move"}, {"x", m_impl->pointer.x()}, {"y", m_impl->pointer.y()}});
}

void DesktopViewerSurface::mousePressEvent(QMouseEvent *event)
{
    m_impl->cancelAutomaticPaste();
    m_impl->hover(event->position());
    if (m_impl->mode != "control" || !m_impl->movePointer(event->position()))
        return;
    setFocus(Qt::MouseFocusReason);
    if (isVisible())
        grabKeyboard();
    const auto button = nativeButton(event->button());
    if (!button)
        return;
    m_impl->buttons.insert(*button);
    m_impl->input({{"kind", "button"}, {"button", *button}, {"down", true},
        {"x", m_impl->pointer.x()}, {"y", m_impl->pointer.y()}});
}

void DesktopViewerSurface::mouseReleaseEvent(QMouseEvent *event)
{
    m_impl->hover(event->position());
    const auto button = nativeButton(event->button());
    if (!button || !m_impl->buttons.remove(*button))
        return;
    m_impl->movePointer(event->position());
    m_impl->input({{"kind", "button"}, {"button", *button}, {"down", false},
        {"x", m_impl->pointer.x()}, {"y", m_impl->pointer.y()}});
}

void DesktopViewerSurface::wheelEvent(QWheelEvent *event)
{
    m_impl->cancelAutomaticPaste();
    m_impl->hover(event->position());
    if (!m_impl->movePointer(event->position()))
        return;
    const auto delta = event->pixelDelta().isNull() ? event->angleDelta() / 8 : event->pixelDelta();
    m_impl->input({{"kind", "scroll"}, {"x", m_impl->pointer.x()}, {"y", m_impl->pointer.y()},
        {"scroll_x", qBound(-32768, delta.x(), 32767)}, {"scroll_y", qBound(-32768, delta.y(), 32767)}});
}

void DesktopViewerSurface::dragEnterEvent(QDragEnterEvent *event)
{
    if (!m_impl->richClipboard || !m_impl->capture.clipboardFiles || m_impl->mode != "control"
        || !m_impl->active || !m_impl->nativeReady || m_impl->closing || !isVisible() || !isActiveWindow())
        return;
    try {
        clipboardLocalFiles(*event->mimeData());
        if (m_impl->clipboardStatus) {
            m_impl->clipboardStatus->setText(tr("Drop to transfer and paste into the active remote app."));
            m_impl->clipboardStatus->adjustSize();
            m_impl->clipboardStatus->move(12, 12);
            m_impl->clipboardStatus->show();
            m_impl->clipboardStatus->raise();
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } catch (const std::exception &) {
        event->ignore();
    }
}

void DesktopViewerSurface::dropEvent(QDropEvent *event)
{
    if (!m_impl->richClipboard || !m_impl->capture.clipboardFiles || m_impl->mode != "control"
        || !m_impl->active || !m_impl->nativeReady || m_impl->closing || !isVisible() || !isActiveWindow())
        return;
    m_impl->cancelAutomaticPaste();
    try {
        const auto paths = clipboardLocalFiles(*event->mimeData());
        setFocus(Qt::MouseFocusReason);
        m_impl->richClipboard->drop(paths);
        m_impl->queueAutomaticPaste(ClipboardPasteChord{{227}, 25});
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } catch (const std::exception &) {
        event->ignore();
    }
}

void DesktopViewerSurface::keyPressEvent(QKeyEvent *event)
{
    if (m_impl->mode != "control" || !m_impl->nativeReady || m_impl->closing)
        return;
    m_impl->cancelAutomaticPaste();
    if (m_impl->suppressedPasteKeys.contains(event->key())) {
        event->accept();
        return;
    }
    const auto release = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
    if (event->key() == Qt::Key_Escape && (event->modifiers() & release) == release) {
        m_impl->releaseInput();
        clearFocus();
        event->accept();
        return;
    }
    const auto modifiers = event->modifiers();
    const bool pasteModifier = modifiers == Qt::ControlModifier || modifiers == Qt::MetaModifier
        || modifiers == (Qt::ControlModifier | Qt::ShiftModifier)
        || modifiers == (Qt::MetaModifier | Qt::ShiftModifier);
    const bool paste = !event->isAutoRepeat()
        && ((event->key() == Qt::Key_V && pasteModifier)
            || (event->key() == Qt::Key_Insert && modifiers == Qt::ShiftModifier));
    if (paste && m_impl->richClipboard && m_impl->richClipboard->hasDropIntent()) {
        m_impl->retireClipboardBarrier();
        if (!m_impl->richClipboard->paste()) {
            m_impl->suppressedPasteKeys.insert(event->key());
            m_impl->queueAutomaticPaste(*event);
            event->accept();
            return;
        }
    } else if (paste && (m_impl->capture.clipboardText || m_impl->richClipboard)) {
        const auto *mime = QGuiApplication::clipboard()->mimeData();
        const auto kind = mime ? clipboardSelectionKind(*mime) : ClipboardSelectionKind::Unsupported;
        if (kind == ClipboardSelectionKind::Files || kind == ClipboardSelectionKind::Png) {
            m_impl->retireClipboardBarrier();
            if (!m_impl->richClipboard || !m_impl->richClipboard->paste()) {
                m_impl->suppressedPasteKeys.insert(event->key());
                if (m_impl->richClipboard)
                    m_impl->queueAutomaticPaste(*event);
                event->accept();
                return;
            }
        } else {
            m_impl->offerClipboard(true);
        }
        if (m_impl->closing)
            return;
    }
    if (const auto usage = m_impl->keyboard.hidForNativeKey(*event)) {
        m_impl->keys.insert(*usage);
        m_impl->input({{"kind", "key"}, {"hid_usage", mapControlCommandUsage(*usage, m_impl->presentation.swapControlCommand)}, {"down", true}});
        event->accept();
    }
}

void DesktopViewerSurface::keyReleaseEvent(QKeyEvent *event)
{
    if (m_impl->suppressedPasteKeys.contains(event->key())) {
        if (!event->isAutoRepeat()) {
            m_impl->suppressedPasteKeys.remove(event->key());
            m_impl->tryAutomaticPaste();
        }
        event->accept();
        return;
    }
    if (event->isAutoRepeat())
        return;
    if (const auto usage = m_impl->keyboard.hidForNativeKey(*event); usage && m_impl->keys.remove(*usage)) {
        m_impl->input({{"kind", "key"}, {"hid_usage", mapControlCommandUsage(*usage, m_impl->presentation.swapControlCommand)}, {"down", false}});
        m_impl->tryAutomaticPaste();
        event->accept();
    }
}

void DesktopViewerSurface::focusOutEvent(QFocusEvent *event)
{
    m_impl->leave();
    m_impl->releaseInput();
    QWidget::focusOutEvent(event);
}

bool DesktopViewerSurface::event(QEvent *event)
{
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate) {
        m_impl->leave();
        m_impl->releaseInput();
    }
    if (event->type() == QEvent::WindowActivate || event->type() == QEvent::Show)
        QTimer::singleShot(0, this, [this] { m_impl->refreshClipboard(); });
    if (event->type() == QEvent::ShortcutOverride && hasFocus() && m_impl->mode == "control") {
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

} // namespace dockpipe::desktop
