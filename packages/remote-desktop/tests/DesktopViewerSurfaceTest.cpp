#include "DesktopViewerSurface.h"
#include "ViewerPreferences.h"
#include "InputMapping.h"
#include "RichClipboardTransfer.h"

#include <QApplication>
#include <QColor>
#include <QClipboard>
#include <QMimeData>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSettings>
#include <QResizeEvent>
#include <QProgressBar>
#include <QLabel>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

using dockpipe::desktop::DesktopViewerSurface;
using dockpipe::desktop::ViewerCapturePreferences;

class DesktopViewerSurfaceTest final : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_root;

    QJsonObject options(const QString &mode)
    {
        return {{"schema", "dockpipe.remote.desktop-display/v1"}, {"cli", QString(FIXTURE_SCRIPT)},
            {"state_directory", m_root.path()}, {"mode", mode},
            {"catalog", QJsonObject{{"schema", "dockpipe.remote.desktop/v1"}, {"view", true}, {"input", true},
                {"epoch", QString(64, 'e')},
                {"target", QJsonObject{{"node", "fixture"}, {"fingerprint", QString(64, 'a')},
                    {"generation", QString(64, 'f')}}},
                {"provider", QJsonObject{{"id", "dockpipe.desktop.native"}, {"version", "0.1.0"},
                    {"artifact", QString(64, '1')}}}}}};
    }

    void behavior(const QByteArray &value)
    {
        QFile output(m_root.filePath("behavior"));
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(value), value.size());
        QFile::remove(m_root.filePath("events.jsonl"));
        QFile::remove(m_root.filePath("channel-attached"));
        QFile::remove(m_root.filePath("asset-commit"));
        for (const auto &name : {"ack", "changed", "old", "future", "notice"})
            QFile::remove(m_root.filePath("clipboard-" + QString::fromLatin1(name)));
    }

    QJsonArray events()
    {
        QFile input(m_root.filePath("events.jsonl"));
        QJsonArray result;
        if (input.open(QIODevice::ReadOnly)) {
            for (const auto &line : input.readAll().split('\n')) {
                if (!line.isEmpty())
                    result.append(QJsonDocument::fromJson(line).object());
            }
        }
        return result;
    }

    int count(const QString &event, int kind = -1)
    {
        int result = 0;
        for (const auto &entry : events()) {
            const auto object = entry.toObject();
            if (object.value("event") == event && (kind < 0 || object.value("kind").toInt() == kind))
                ++result;
        }
        return result;
    }

#ifdef VIDEO_FIXTURE_DIRECTORY
    bool prepareMediaFixture()
    {
        QFile manifest(QDir(VIDEO_FIXTURE_DIRECTORY).filePath("fixture.json"));
        if (!manifest.open(QIODevice::ReadOnly))
            return false;
        const auto first = QJsonDocument::fromJson(manifest.readAll()).object().value("frames").toArray().first().toObject();
        QFile::remove(m_root.filePath("media.h264"));
        if (!QFile::copy(QDir(VIDEO_FIXTURE_DIRECTORY).filePath(first.value("file").toString()), m_root.filePath("media.h264")))
            return false;
        behavior("media");
        return true;
    }
#endif

    int keyDownCount(int usage)
    {
        int result = 0;
        for (const auto &entry : events()) {
            const auto object = entry.toObject();
            const auto input = object.value("input").toObject();
            if (object.value("kind") == 2 && input.value("hid_usage").toInt() == usage
                && input.value("down").toBool())
                ++result;
        }
        return result;
    }

    void allowAssetCommit()
    {
        QFile trigger(m_root.filePath("asset-commit"));
        QVERIFY(trigger.open(QIODevice::WriteOnly));
    }

    QJsonObject automaticAssetOptions(bool files = false)
    {
        auto selected = options("control");
        QJsonObject capture{{"quality", "high"}, {"frame_rate", 30}};
        capture.insert(files ? "clipboard_files" : "clipboard_images", true);
        selected.insert("capture", capture);
        return selected;
    }

    void generatedImageClipboard()
    {
        QImage image(3, 4, QImage::Format_RGBA8888);
        image.fill(Qt::cyan);
        auto *mime = new QMimeData;
        mime->setImageData(image);
        QApplication::clipboard()->setMimeData(mime);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        qputenv("DOCKPIPE_VIEWER_FIXTURE", m_root.path().toUtf8());
        QCoreApplication::setOrganizationName("DockpipeFixture");
        QCoreApplication::setApplicationName("NativeViewer");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.path());
    }

    void init()
    {
        QSettings().clear();
        behavior("normal");
    }

    QJsonObject clipboardOptions()
    {
        auto selected = options("control");
        selected.insert("capture", QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"clipboard_text", true}});
        return selected;
    }

    int clipboardCount(int operation)
    {
        int result = 0;
        for (const auto &entry : events()) {
            if (entry.toObject().value("kind").toInt() == 4
                && entry.toObject().value("operation").toInt() == operation)
                ++result;
        }
        return result;
    }

    void clipboardTrigger(const QString &name)
    {
        QFile file(m_root.filePath("clipboard-" + name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    void launchEnvelopeCompatibility_data()
    {
        QTest::addColumn<QString>("schema");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("generic host") << QStringLiteral("dockpipe.remote.desktop-display/v1") << true;
        QTest::newRow("legacy host") << QStringLiteral("dockpipe.desktop.native/v1") << true;
        QTest::newRow("unknown contract") << QStringLiteral("example.unrelated/v1") << false;
    }

    void launchEnvelopeCompatibility()
    {
        QFETCH(QString, schema);
        QFETCH(bool, accepted);
        auto selected = options("view");
        selected.insert("schema", schema);
        if (accepted) {
            DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
            surface.closeDisplay();
        } else {
            QVERIFY_EXCEPTION_THROWN(DesktopViewerSurface(selected, FIXTURE_SCRIPT, nullptr), std::invalid_argument);
        }
        QCOMPARE(count("open"), 0);
    }

    void assetPasteConsumesOnlyPasteKeyUntilExplicitReady()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Clipboard fixture requires its isolated X11 display.");
        behavior("assets");
        QImage image(3, 4, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        auto *mime = new QMimeData;
        mime->setImageData(image);
        QApplication::clipboard()->setMimeData(mime);
        auto selected = options("control");
        selected.insert("capture", QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"clipboard_images", true}});
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(count("packet", 5) >= 1, 5000);
        QTest::qWait(100);
        int offers = 0;
        for (const auto &entry : events()) {
            if (entry.toObject().value("kind") == 5 && entry.toObject().value("operation") == 3)
                ++offers;
        }
        QCOMPARE(offers, 0); // focus establishes a baseline, never exports the existing image
        QTest::keyPress(&surface, Qt::Key_V, Qt::ControlModifier);
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier, "v", true);
        QApplication::sendEvent(&surface, &repeat);
        QTest::keyRelease(&surface, Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(count("packet", 2) >= 2);
        bool controlDown = false;
        bool controlUp = false;
        for (const auto &entry : events()) {
            const auto object = entry.toObject();
            if (object.value("kind") != 2)
                continue;
            const auto input = object.value("input").toObject();
            QVERIFY(input.value("hid_usage") != 25); // V down AND up consumed; no queued automatic paste
            if (input.value("hid_usage") == 224) {
                controlDown |= input.value("down").toBool();
                controlUp |= !input.value("down").toBool();
            }
        }
        QVERIFY(controlDown && controlUp);
        QCOMPARE(count("packet", 4), 0); // image never falls back to kind4 text
        surface.closeDisplay();
    }

    void richCopyRetiresTextBarrierAndReleasesActualModifier()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Clipboard fixture requires its isolated X11 display.");
        behavior("clipboard-assets");
        QApplication::clipboard()->setText("pending text");
        auto selected = clipboardOptions();
        auto capture = selected.value("capture").toObject();
        capture.insert("clipboard_images", true);
        selected.insert("capture", capture);
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(clipboardCount(1), 1, 5000);
        QTest::keyClick(&surface, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(clipboardCount(3), 1);
        QImage image(2, 2, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        auto *mime = new QMimeData;
        mime->setImageData(image);
        QApplication::clipboard()->setMimeData(mime);
        const auto released = [&] {
            for (const auto &entry : events()) {
                const auto input = entry.toObject().value("input").toObject();
                if (input.value("hid_usage") == 224 && input.value("down") == false)
                    return true;
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(released(), 3000);
        for (const auto &entry : events()) {
            const auto input = entry.toObject().value("input").toObject();
            QVERIFY(input.value("hid_usage") != 25);
        }
        surface.closeDisplay();
    }

    void eligibleFileDropIsCopyOnlyAndDoesNotPaste()
    {
        behavior("assets");
        QFile source(m_root.filePath("drop-source.txt"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("not read before acceptance");
        source.close();
        auto selected = options("control");
        selected.insert("capture", QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"clipboard_files", true}});
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(count("packet", 5) >= 1, 5000);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(source.fileName())});
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&surface, &enter);
        QVERIFY(enter.isAccepted());
        QCOMPARE(enter.dropAction(), Qt::CopyAction);
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&surface, &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(drop.dropAction(), Qt::CopyAction);
        const auto offerCount = [&] {
            int result = 0;
            for (const auto &entry : events()) {
                if (entry.toObject().value("kind") == 5 && entry.toObject().value("operation") == 3)
                    ++result;
            }
            return result;
        };
        QTRY_COMPARE_WITH_TIMEOUT(offerCount(), 1, 3000);
        for (const auto &entry : events()) {
            if (entry.toObject().value("kind") == 5)
                QVERIFY(entry.toObject().value("operation") != 5); // no source bytes without Accept
        }
        QCOMPARE(count("packet", 2), 0);
        QVERIFY(QFileInfo(source.fileName()).isFile());
        surface.closeDisplay();
    }

    void pendingRichPasteWaitsForCommitAndPhysicalRelease_data()
    {
        QTest::addColumn<bool>("commitFirst");
        QTest::addColumn<bool>("staleCachedControl");
        QTest::newRow("commit-before-key-release") << true << false;
        QTest::newRow("release-before-commit") << false << false;
        QTest::newRow("cached-control-stale-native-released") << false << true;
    }

    void pendingRichPasteWaitsForCommitAndPhysicalRelease()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Requires the isolated X11 fixture.");
        QFETCH(bool, commitFirst);
        QFETCH(bool, staleCachedControl);
        behavior("assets-auto");
        generatedImageClipboard();
        DesktopViewerSurface surface(automaticAssetOptions(), FIXTURE_SCRIPT, nullptr);
        auto *rich = surface.findChild<dockpipe::desktop::RichClipboardTransfer *>();
        QVERIFY(rich);
        QSignalSpy committed(rich, &dockpipe::desktop::RichClipboardTransfer::outgoingCommitted);
        QStringList statusHistory;
        surface.statusChanged = [&](const QString &text) { statusHistory.append(text); };
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(count("packet", 5) >= 1, 5000);
        QTest::keyPress(&surface, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(count("asset-ready-to-commit"), 1, 5000);
        auto *progress = surface.findChild<QProgressBar *>("nativeDesktopClipboardProgress");
        QVERIFY(progress && progress->isVisible());
        QCOMPARE(keyDownCount(25), 0);
        if (commitFirst) {
            allowAssetCommit();
            QTRY_COMPARE(count("asset-committed"), 1);
            QTest::qWait(100);
            QCOMPARE(keyDownCount(25), 0);
            // QTest's modifier convenience retains Ctrl in global modifier state.
            // Deliver the two physical releases with an explicit final clear state.
            QTest::keyRelease(&surface, Qt::Key_V);
            QTest::keyRelease(&surface, Qt::Key_Control);
        } else {
            if (staleCachedControl) {
                // Both tracked keys release, but QtTest retains cached Ctrl.
                // The native query must remain authoritative for this gate.
                QTest::keyRelease(&surface, Qt::Key_V, Qt::ControlModifier);
                QVERIFY(QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier));
                QCOMPARE(QGuiApplication::queryKeyboardModifiers(), Qt::NoModifier);
                QVERIFY(QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier));
            } else {
                QTest::keyRelease(&surface, Qt::Key_V);
                QTest::keyRelease(&surface, Qt::Key_Control);
            }
            QTest::qWait(100);
            QCOMPARE(keyDownCount(25), 0);
            allowAssetCommit();
        }
        QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 3000);
        QTest::qWait(200);
        const auto releaseDiagnostics = QString(" globals=%1/%2 overlay=%3")
            .arg(int(QGuiApplication::keyboardModifiers()))
            .arg(int(QGuiApplication::mouseButtons()))
            .arg(surface.findChild<QLabel *>("nativeDesktopClipboardStatus")->text());
        QVERIFY2(keyDownCount(25) == 1, qPrintable(statusHistory.join(" | ") + releaseDiagnostics));
        QTRY_VERIFY(!progress->isVisible());
        QTest::qWait(150);
        QCOMPARE(keyDownCount(25), 1);
        QCOMPARE(count("packet", 4), 0);
        surface.closeDisplay();
    }

    void fileDropAutomaticallyPastesCommandVIndependentOfMapping()
    {
        behavior("assets-auto");
        QFile source(m_root.filePath("auto-drop-generated.bin"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("generated drop fixture"), qint64(22));
        source.close();
        DesktopViewerSurface surface(automaticAssetOptions(true), FIXTURE_SCRIPT, nullptr);
        surface.setPresentationOptions(swappedDisplay(true));
        QVERIFY(surface.presentationOptions().value("swap_control_command").toBool());
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(count("packet", 5) >= 1, 5000);
        QTest::keyRelease(&surface, Qt::Key_Control);
        QCOMPARE(QGuiApplication::keyboardModifiers(), Qt::NoModifier);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(source.fileName())});
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&surface, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&surface, &drop);
        QVERIFY(drop.isAccepted());
        QTRY_COMPARE_WITH_TIMEOUT(count("asset-ready-to-commit"), 1, 5000);
        auto *progress = surface.findChild<QProgressBar *>("nativeDesktopClipboardProgress");
        QVERIFY(progress && progress->isVisible());
        QCOMPARE(keyDownCount(25), 0);
        allowAssetCommit();
        QTRY_COMPARE_WITH_TIMEOUT(keyDownCount(25), 1, 3000);
        QCOMPARE(keyDownCount(227), 1);
        QCOMPARE(keyDownCount(224), 0);
        QTRY_VERIFY(!progress->isVisible());
        QTest::qWait(150);
        QCOMPARE(keyDownCount(25), 1);
        surface.closeDisplay();
    }

    void interveningInputCancelsPendingAutomaticPaste_data()
    {
        QTest::addColumn<bool>("focusLoss");
        QTest::newRow("click") << false;
        QTest::newRow("focus-loss") << true;
    }

    void interveningInputCancelsPendingAutomaticPaste()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Requires the isolated X11 fixture.");
        QFETCH(bool, focusLoss);
        behavior("assets-auto");
        generatedImageClipboard();
        DesktopViewerSurface surface(automaticAssetOptions(), FIXTURE_SCRIPT, nullptr);
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(count("packet", 5) >= 1, 5000);
        QTest::keyClick(&surface, Qt::Key_V, Qt::ControlModifier);
        QTest::keyRelease(&surface, Qt::Key_Control);
        QTRY_COMPARE_WITH_TIMEOUT(count("asset-ready-to-commit"), 1, 5000);
        if (focusLoss)
            surface.clearFocus();
        else
            QTest::mouseClick(&surface, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        allowAssetCommit();
        QTest::qWait(250);
        QCOMPARE(keyDownCount(25), 0);
        surface.closeDisplay();
    }

    void clipboardPreferencesRequireCanonicalControlOptIn()
    {
        const QJsonObject normal{{"quality", "high"}, {"frame_rate", 30}};
        QVERIFY(!ViewerCapturePreferences::parse(normal).clipboardText);
        auto selected = normal;
        selected.insert("clipboard_text", true);
        QVERIFY(ViewerCapturePreferences::parse(selected).clipboardText);
        QCOMPARE(ViewerCapturePreferences::parse(selected).json(), selected);
        for (const auto &invalid : {QJsonValue(false), QJsonValue(), QJsonValue(1), QJsonValue("true")}) {
            selected.insert("clipboard_text", invalid);
            QVERIFY_EXCEPTION_THROWN(ViewerCapturePreferences::parse(selected), std::invalid_argument);
        }
        auto view = clipboardOptions();
        view.insert("mode", "view");
        QVERIFY_EXCEPTION_THROWN(DesktopViewerSurface(view, FIXTURE_SCRIPT, nullptr), std::invalid_argument);
    }

    void clipboardBarrierOrdersCoalescedWritesAndPhysicalInput()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Clipboard fixture requires its isolated X11 display.");
        behavior("clipboard");
        QApplication::clipboard()->setText("existing local");
        DesktopViewerSurface surface(clipboardOptions(), FIXTURE_SCRIPT, nullptr);
        QString failure;
        surface.failed = [&](const QString &message) { failure = message; };
        surface.resize(640, 480);
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(clipboardCount(1), 1, 5000);
        QCOMPARE(clipboardCount(3), 0); // Activation does not upload existing contents.
        QTest::keyPress(&surface, Qt::Key_Control);
        QTRY_COMPARE(count("packet", 2), 1);
        QTest::keyPress(&surface, Qt::Key_V, Qt::ControlModifier);
        QTest::keyRelease(&surface, Qt::Key_V, Qt::ControlModifier);
        QTest::keyRelease(&surface, Qt::Key_Control);
        QTRY_COMPARE(clipboardCount(3), 1);
        clipboardTrigger("changed"); // An older snapshot can already be in flight.
        QTest::qWait(100);
        QCOMPARE(QApplication::clipboard()->text(), QString("existing local"));
        const int beforeAck = count("packet", 2);
        QApplication::clipboard()->setText(QString::fromUtf8("latest λ 👋"));
        QTest::qWait(50);
        QCOMPARE(count("packet", 2), beforeAck);
        clipboardTrigger("ack");
        QTRY_COMPARE(clipboardCount(3), 2);
        QCOMPARE(count("packet", 2), beforeAck);
        clipboardTrigger("notice"); // Old poll notice after ACK cannot reject the coalesced Write.
        QTest::qWait(100);
        QVERIFY(failure.isEmpty());
        QCOMPARE(count("packet", 2), beforeAck);
        clipboardTrigger("ack");
        QTRY_VERIFY(count("packet", 2) > beforeAck);
        quint64 sequence = 0;
        for (const auto &entry : events()) {
            if (entry.toObject().value("kind").toInt() == 2)
                QCOMPARE(entry.toObject().value("input").toObject().value("sequence").toInteger(), qint64(++sequence));
        }
        clipboardTrigger("changed");
        QTRY_COMPARE(QApplication::clipboard()->text(), QString::fromUtf8("remote λ"));
        QTest::qWait(100);
        QCOMPARE(clipboardCount(3), 2); // Applying remote text must not echo.
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        surface.closeDisplay();
    }

    void clipboardPastePreservesPhysicalModifiers_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("modifier");
        QTest::addColumn<int>("modifierUsage");
        QTest::newRow("control-v") << int(Qt::Key_V) << int(Qt::ControlModifier) << 224;
        QTest::newRow("command-v") << int(Qt::Key_V) << int(Qt::MetaModifier) << 227;
        QTest::newRow("shift-insert") << int(Qt::Key_Insert) << int(Qt::ShiftModifier) << 225;
    }

    void clipboardPastePreservesPhysicalModifiers()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Clipboard fixture requires its isolated X11 display.");
        QFETCH(int, key);
        QFETCH(int, modifier);
        QFETCH(int, modifierUsage);
        behavior("clipboard");
        QApplication::clipboard()->setText("paste chord text");
        DesktopViewerSurface surface(clipboardOptions(), FIXTURE_SCRIPT, nullptr);
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(clipboardCount(1), 1, 5000);
        QTest::keyClick(&surface, Qt::Key(key), Qt::KeyboardModifiers(modifier));
        QTRY_COMPARE(clipboardCount(3), 1);
        int before = count("packet", 2);
        clipboardTrigger("ack");
        QTRY_VERIFY(count("packet", 2) > before);
        QSet<int> usages;
        for (const auto &entry : events()) {
            if (entry.toObject().value("kind").toInt() == 2)
                usages.insert(entry.toObject().value("input").toObject().value("hid_usage").toInt());
        }
        QVERIFY(usages.contains(modifierUsage));
        if (modifierUsage == 224)
            QVERIFY(!usages.contains(227));
        if (modifierUsage == 227)
            QVERIFY(!usages.contains(224));
        surface.closeDisplay();
    }

    void clipboardFocusLossDiscardsQueuedKeysAndOldEpochReplies()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Clipboard fixture requires its isolated X11 display.");
        behavior("clipboard");
        DesktopViewerSurface surface(clipboardOptions(), FIXTURE_SCRIPT, nullptr);
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(clipboardCount(1), 1, 5000);
        QTest::keyPress(&surface, Qt::Key_A);
        QTRY_COMPARE(count("packet", 2), 1);
        QApplication::clipboard()->setText("pending");
        QTRY_COMPARE(clipboardCount(3), 1);
        QTest::keyPress(&surface, Qt::Key_B);
        QFocusEvent leave(QEvent::FocusOut);
        QApplication::sendEvent(&surface, &leave);
        QTRY_COMPARE(clipboardCount(2), 1);
        QTRY_COMPARE(count("packet", 2), 2); // Only actually-sent A is released.
        const auto audit = events();
        for (const auto &entry : audit) {
            if (entry.toObject().value("kind").toInt() == 2)
                QCOMPARE(entry.toObject().value("input").toObject().value("hid_usage").toInt(), 4);
        }
        QFocusEvent enter(QEvent::FocusIn);
        QApplication::sendEvent(&surface, &enter);
        QTRY_COMPARE(clipboardCount(1), 2);
        clipboardTrigger("ack"); // Previous epoch acknowledgment cannot release B.
        clipboardTrigger("old");
        QTest::qWait(100);
        QCOMPARE(QApplication::clipboard()->text(), QString("pending"));
        QCOMPARE(count("packet", 2), 2);
        surface.closeDisplay();
    }

    void unsupportedClipboardSupersedesCoalescedText_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("nontext") << QString("nontext");
        QTest::newRow("oversize") << QString("oversize");
        QTest::newRow("nul") << QString("nul");
    }

    void unsupportedClipboardSupersedesCoalescedText()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Clipboard fixture requires its isolated X11 display.");
        QFETCH(QString, kind);
        behavior("clipboard");
        DesktopViewerSurface surface(clipboardOptions(), FIXTURE_SCRIPT, nullptr);
        QString failure;
        surface.failed = [&](const QString &message) { failure = message; };
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(clipboardCount(1), 1, 5000);
        QApplication::clipboard()->setText("first pending");
        QTRY_COMPARE(clipboardCount(3), 1);
        QApplication::clipboard()->setText("coalesced stale");
        QTest::keyClick(&surface, Qt::Key_V, Qt::ControlModifier);
        if (kind == "nontext") {
            auto *mime = new QMimeData;
            mime->setData("application/x-dockpipe-fixture", "fixture");
            QApplication::clipboard()->setMimeData(mime);
        } else if (kind == "oversize") {
            QApplication::clipboard()->setText(QString(65537, 'x'));
        } else {
            QApplication::clipboard()->setText(QString("invalid") + QChar(0));
        }
        QTRY_VERIFY(!failure.isEmpty());
        clipboardTrigger("ack");
        QTest::qWait(100);
        QCOMPARE(clipboardCount(3), 1);
        QCOMPARE(count("packet", 2), 0);
        surface.closeDisplay();
    }

    void clipboardFailureDiscardsQueuedInput_data()
    {
        QTest::addColumn<QString>("failureKind");
        QTest::newRow("overflow") << QString("overflow");
        QTest::newRow("future-epoch") << QString("future");
        QTest::newRow("absolute-deadline") << QString("deadline");
    }

    void clipboardFailureDiscardsQueuedInput()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Clipboard fixture requires its isolated X11 display.");
        QFETCH(QString, failureKind);
        behavior("clipboard");
        DesktopViewerSurface surface(clipboardOptions(), FIXTURE_SCRIPT, nullptr);
        QString failure;
        surface.failed = [&](const QString &message) { failure = message; };
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(clipboardCount(1), 1, 5000);
        QApplication::clipboard()->setText("first");
        QTRY_COMPARE(clipboardCount(3), 1);
        QElapsedTimer deadline;
        deadline.start();
        QTest::keyPress(&surface, Qt::Key_A);
        if (failureKind == "overflow") {
            for (int index = 0; index < 128; ++index)
                QTest::keyPress(&surface, Qt::Key_B);
        } else if (failureKind == "deadline") {
            QTest::qWait(1600);
            QApplication::clipboard()->setText("coalesced");
            clipboardTrigger("ack");
            QTRY_COMPARE(clipboardCount(3), 2);
        } else {
            clipboardTrigger(failureKind);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 3500);
        if (failureKind == "deadline")
            QVERIFY(deadline.elapsed() < 4000); // ACK/coalescing cannot renew the original deadline.
        QCOMPARE(count("packet", 2), 0);
        surface.closeDisplay();
    }

    void clipboardTimeoutDiscardsPasteAndCloses()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Clipboard fixture requires its isolated X11 display.");
        behavior("clipboard");
        DesktopViewerSurface surface(clipboardOptions(), FIXTURE_SCRIPT, nullptr);
        QString failure;
        surface.failed = [&](const QString &message) { failure = message; };
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(clipboardCount(1), 1, 5000);
        QApplication::clipboard()->setText("pending timeout");
        QTRY_COMPARE(clipboardCount(3), 1);
        QTest::keyClick(&surface, Qt::Key_V, Qt::ControlModifier);
        clipboardTrigger("notice");
        QTest::qWait(100);
        QVERIFY(failure.isEmpty()); // An uncorrelated Notice is not an ACK or a Write rejection.
        QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 4000);
        QCOMPARE(count("packet", 2), 0);
        surface.closeDisplay();
    }

    QJsonArray keyEvents()
    {
        QJsonArray result;
        for (const auto &entry : events()) {
            const auto object = entry.toObject();
            const auto input = object.value("input").toObject();
            if (object.value("kind").toInt() == 2 && input.value("kind") == "key")
                result.append(input);
        }
        return result;
    }

    QJsonObject swappedDisplay(bool enabled)
    {
        QJsonObject value{{"scale", "fit"}, {"hide_cursor", true}};
        if (enabled)
            value.insert("swap_control_command", true);
        return value;
    }

    void modifierPreferenceIsOptionalAndTyped()
    {
        using dockpipe::desktop::ViewerPresentationPreferences;
        const auto normal = swappedDisplay(false);
        QVERIFY(!ViewerPresentationPreferences::parse(normal).swapControlCommand);
        QCOMPARE(ViewerPresentationPreferences::parse(normal).json(), normal);
        const auto swapped = swappedDisplay(true);
        QVERIFY(ViewerPresentationPreferences::parse(swapped).swapControlCommand);
        QCOMPARE(ViewerPresentationPreferences::parse(swapped).json(), swapped);
        auto explicitFalse = normal;
        explicitFalse.insert("swap_control_command", false);
        QCOMPARE(ViewerPresentationPreferences::parse(explicitFalse).json(), normal);
        for (const auto &invalid : {QJsonValue(), QJsonValue(1), QJsonValue("true"), QJsonValue(QJsonObject{})}) {
            auto value = normal;
            value.insert("swap_control_command", invalid);
            QVERIFY_EXCEPTION_THROWN(ViewerPresentationPreferences::parse(value), std::invalid_argument);
        }
        auto unknown = swapped;
        unknown.insert("other", true);
        QVERIFY_EXCEPTION_THROWN(ViewerPresentationPreferences::parse(unknown), std::invalid_argument);
    }

    void swappedControlCopyAndFocusReleaseUseMappedUsages()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Modifier fixture requires isolated X11.");
        auto selected = options("control");
        selected.insert("display", swappedDisplay(true));
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        QString status;
        surface.statusChanged = [&](const QString &message) { status = message; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        QTest::keyClick(&surface, Qt::Key_C, Qt::ControlModifier);
        QTRY_COMPARE(keyEvents().size(), 4);
        const auto copy = keyEvents();
        QCOMPARE(copy[0].toObject().value("hid_usage").toInt(), 227);
        QCOMPARE(copy[1].toObject().value("hid_usage").toInt(), 6);
        QVERIFY(copy[1].toObject().value("down").toBool());
        QCOMPARE(copy[2].toObject().value("hid_usage").toInt(), 6);
        QCOMPARE(copy[3].toObject().value("hid_usage").toInt(), 227);
        QVERIFY(!copy[3].toObject().value("down").toBool());
        QTest::keyPress(&surface, Qt::Key_Control);
        QFocusEvent leave(QEvent::FocusOut);
        QApplication::sendEvent(&surface, &leave);
        QTRY_COMPARE(keyEvents().size(), 6);
        QCOMPARE(keyEvents().last().toObject().value("hid_usage").toInt(), 227);
        QVERIFY(!keyEvents().last().toObject().value("down").toBool());
        surface.closeDisplay();
    }

    void liveModifierChangeReleasesMappedHeldKeysBeforeNewPolicy()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Modifier fixture requires isolated X11.");
        auto selected = options("control");
        selected.insert("display", swappedDisplay(true));
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        QString status;
        surface.statusChanged = [&](const QString &message) { status = message; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        QTest::keyPress(&surface, Qt::Key_Control);
        QTest::keyPress(&surface, Qt::Key_A);
        QTRY_COMPARE(keyEvents().size(), 2);
        auto invalid = swappedDisplay(true);
        invalid.insert("swap_control_command", "invalid");
        surface.setPresentationOptions(invalid);
        QCOMPARE(surface.presentationOptions(), swappedDisplay(true));
        QCOMPARE(keyEvents().size(), 2);
        surface.setPresentationOptions(swappedDisplay(false));
        QTRY_COMPARE(keyEvents().size(), 4);
        QSet<int> released;
        for (int index = 2; index < 4; ++index) {
            const auto event = keyEvents()[index].toObject();
            QVERIFY(!event.value("down").toBool());
            released.insert(event.value("hid_usage").toInt());
        }
        QCOMPARE(released, (QSet<int>{227, 4}));
        QTest::keyRelease(&surface, Qt::Key_Control);
        QTest::keyRelease(&surface, Qt::Key_A);
        QTest::qWait(50);
        QCOMPARE(keyEvents().size(), 4); // Old physical keyups cannot create new-policy releases.
        QTest::keyClick(&surface, Qt::Key_Control);
        QTRY_COMPARE(keyEvents().size(), 6);
        QCOMPARE(keyEvents()[4].toObject().value("hid_usage").toInt(), 224);
        QCOMPARE(keyEvents()[5].toObject().value("hid_usage").toInt(), 224);
        QVERIFY(!keyEvents()[5].toObject().value("down").toBool());
        surface.closeDisplay();
    }

    void physicalReleaseShortcutSurvivesModifierSwap()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Modifier fixture requires isolated X11.");
        auto selected = options("control");
        selected.insert("display", swappedDisplay(true));
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        QString status;
        surface.statusChanged = [&](const QString &message) { status = message; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        QTest::keyPress(&surface, Qt::Key_Control);
        QTest::keyPress(&surface, Qt::Key_Alt);
        QTest::keyPress(&surface, Qt::Key_Shift);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
        QApplication::sendEvent(&surface, &escape);
        QTRY_COMPARE(keyEvents().size(), 6);
        QSet<int> released;
        for (const auto &entry : keyEvents()) {
            const auto value = entry.toObject();
            QVERIFY(value.value("hid_usage").toInt() != 41);
            if (!value.value("down").toBool())
                released.insert(value.value("hid_usage").toInt());
        }
        QCOMPARE(released, (QSet<int>{227, 226, 225}));
        surface.closeDisplay();
    }

    void modifierChangeCancelsOldClipboardBarrierBeforeReleasingHeldKeys()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Modifier fixture requires isolated X11.");
        behavior("clipboard");
        QApplication::clipboard()->setText("pending old-policy paste");
        auto selected = clipboardOptions();
        selected.insert("display", swappedDisplay(true));
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(clipboardCount(1), 1, 5000);
        QTest::keyPress(&surface, Qt::Key_Control);
        QTest::keyPress(&surface, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(clipboardCount(3), 1);
        const int sentBeforeChange = keyEvents().size();
        surface.setPresentationOptions(swappedDisplay(false));
        QTRY_COMPARE(clipboardCount(2), 1);
        QTRY_COMPARE(clipboardCount(1), 2);
        QTRY_COMPARE(keyEvents().size(), sentBeforeChange + 1);
        const auto release = keyEvents().last().toObject();
        QCOMPARE(release.value("hid_usage").toInt(), 227);
        QVERIFY(!release.value("down").toBool());
        clipboardTrigger("ack"); // Old-epoch ACK cannot release old mapped V input.
        QTest::qWait(100);
        QCOMPARE(keyEvents().size(), sentBeforeChange + 1);
        for (const auto &entry : keyEvents())
            QVERIFY(entry.toObject().value("hid_usage").toInt() != 25);
        QTest::keyRelease(&surface, Qt::Key_V, Qt::ControlModifier);
        QTest::keyRelease(&surface, Qt::Key_Control);
        QTest::qWait(50);
        QCOMPARE(keyEvents().size(), sentBeforeChange + 1);
        surface.closeDisplay();
    }

    void swappedPasteUsesPhysicalChordAndWaitsForClipboardAck_data()
    {
        QTest::addColumn<int>("modifier");
        QTest::addColumn<int>("remoteModifier");
        QTest::newRow("physical-control-to-command") << int(Qt::ControlModifier) << 227;
        QTest::newRow("physical-command-to-control") << int(Qt::MetaModifier) << 224;
    }

    void swappedPasteUsesPhysicalChordAndWaitsForClipboardAck()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Modifier fixture requires isolated X11.");
        QFETCH(int, modifier);
        QFETCH(int, remoteModifier);
        behavior("clipboard");
        QApplication::clipboard()->setText("modifier fixture paste");
        auto selected = clipboardOptions();
        selected.insert("display", swappedDisplay(true));
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        surface.show();
        surface.activateWindow();
        surface.setFocus();
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(clipboardCount(1), 1, 5000);
        QTest::keyClick(&surface, Qt::Key_V, Qt::KeyboardModifiers(modifier));
        QTRY_COMPARE(clipboardCount(3), 1);
        QCOMPARE(keyEvents().size(), 1);
        QCOMPARE(keyEvents().first().toObject().value("hid_usage").toInt(), remoteModifier);
        clipboardTrigger("ack");
        QTRY_COMPARE(keyEvents().size(), 4);
        QCOMPARE(keyEvents()[1].toObject().value("hid_usage").toInt(), 25);
        QCOMPARE(keyEvents().last().toObject().value("hid_usage").toInt(), remoteModifier);
        QVERIFY(!keyEvents().last().toObject().value("down").toBool());
        surface.closeDisplay();
    }

    void controlUsesPreparedReferenceAndReleasesHeldKeys()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Physical keyboard control requires an X11 display; view-only fixtures remain available.");
        DesktopViewerSurface surface(options("control"), FIXTURE_SCRIPT, nullptr);
        QString status;
        QString failure;
        surface.statusChanged = [&](const QString &value) { status = value; };
        surface.failed = [&](const QString &value) { failure = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(count("ack"), 1);
        QCOMPARE(count("channel"), 1);
        int authorized = -1;
        int attached = -1;
        const auto audit = events();
        for (int index = 0; index < audit.size(); ++index) {
            const auto event = audit[index].toObject().value("event").toString();
            if (event == "binding-authorized")
                authorized = index;
            if (event == "channel")
                attached = index;
        }
        QVERIFY(authorized >= 0 && attached > authorized);
        QTRY_COMPARE_WITH_TIMEOUT(count("packet", 3), 1, 2000);
        QTest::keyPress(&surface, Qt::Key_A);
        QFocusEvent leave(QEvent::FocusOut);
        QApplication::sendEvent(&surface, &leave);
        QTRY_COMPARE_WITH_TIMEOUT(count("packet", 2), 2, 2000);
        QJsonArray input;
        for (const auto &entry : events()) {
            if (entry.toObject().value("kind").toInt() == 2)
                input.append(entry.toObject().value("input"));
        }
        QCOMPARE(input[0].toObject().value("hid_usage").toInt(), 4);
        QCOMPARE(input[0].toObject().value("sequence").toInteger(), 1);
        QVERIFY(input[0].toObject().value("down").toBool());
        QCOMPARE(input[1].toObject().value("sequence").toInteger(), 2);
        QVERIFY(!input[1].toObject().value("down").toBool());
        surface.closeDisplay();
        surface.closeDisplay();
        QVERIFY(QJsonDocument::fromJson(QSettings().value("remote/uncertainDesktopSessions").toByteArray()).array().isEmpty());
        surface.setActive(true);
        QTest::qWait(100);
        QCOMPARE(count("channel"), 1);
    }

    void viewOnlyRejectsKeyboardInput()
    {
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        QString status;
        surface.statusChanged = [&](const QString &value) { status = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        QTest::keyClick(&surface, Qt::Key_A);
        QTest::qWait(100);
        QCOMPARE(count("packet", 2), 0);
        surface.closeDisplay();
    }

    void changedPreparedTargetNeverAcknowledged()
    {
        behavior("wrong-target");
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        QString failure;
        surface.failed = [&](const QString &value) { failure = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 5000);
        QCOMPARE(count("ack"), 1); // Only close was received, never a prepared-ack.
        const auto first = events().first().toObject().value("ack").toObject();
        QCOMPARE(first.value("type").toString(), QString("close"));
        QCOMPARE(count("channel"), 0);
    }

    void invalidMediaClosesWithoutReopening()
    {
        behavior("invalid-media");
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        QString failure;
        surface.failed = [&](const QString &value) { failure = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 5000);
        QCOMPARE(count("channel"), 1);
        surface.setActive(true);
        QTest::qWait(100);
        QCOMPARE(count("channel"), 1);
    }

    void controlLossRetainsExactRecoveryReference()
    {
        behavior("control-loss");
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        QString failure;
        surface.failed = [&](const QString &value) { failure = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 5000);
        const auto retained = QJsonDocument::fromJson(
            QSettings().value("remote/uncertainDesktopSessions").toByteArray()).array();
        QVERIFY2(retained.size() == 1, qPrintable(failure));
        const auto reference = retained.first().toObject();
        QCOMPARE(reference.value("id").toString(), QString(64, 'c'));
        QCOMPARE(reference.value("target"), options("view").value("catalog").toObject().value("target"));
        QVERIFY(!reference.contains("nonce"));
        QVERIFY(!reference.contains("local_fingerprint"));
        QCOMPARE(count("channel"), 1);
    }

    void delayedRemoteProofDoesNotHoldNavigationForALeaseInterval()
    {
        behavior("late-close");
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        QString status;
        surface.statusChanged = [&](const QString &value) { status = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        QElapsedTimer elapsed;
        elapsed.start();
        surface.closeDisplay();
        QVERIFY(elapsed.elapsed() < 2500);
        const auto retained = QJsonDocument::fromJson(
            QSettings().value("remote/uncertainDesktopSessions").toByteArray()).array();
        QCOMPARE(retained.size(), 1);
        QCOMPARE(retained.first().toObject().value("id").toString(), QString(64, 'c'));
    }

    void asynchronousCloseObservesDelayedExactProofWithoutBlockingEvents()
    {
        behavior("late-close");
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        QString status;
        surface.statusChanged = [&](const QString &value) { status = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        const auto reference = surface.closeReference();
        QVERIFY(!reference.isEmpty());
        QCOMPARE(surface.property("closeCompletionBudgetMs").toInt(), 12000);
        QSignalSpy completion(&surface, &DesktopViewerSurface::closeCompleted);
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(20);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(QMetaObject::invokeMethod(&surface, "requestClose", Qt::DirectConnection));
        QVERIFY(elapsed.elapsed() < 100);
        QTRY_COMPARE_WITH_TIMEOUT(completion.size(), 1, 6000);
        QVERIFY(elapsed.elapsed() >= 2800);
        QVERIFY(ticks >= 30);
        const auto result = completion.first().first().toJsonObject();
        QCOMPARE(result, (QJsonObject{{"schema", "dockpipe.desktop.close/v1"},
            {"reference", reference}, {"outcome", "closed"}}));
        QVERIFY(QJsonDocument::fromJson(QSettings().value("remote/uncertainDesktopSessions").toByteArray()).array().isEmpty());
        QCOMPARE(count("channel"), 1);
        surface.closeDisplay();
    }

    void asynchronousCloseCannotConvertUnknownOrRevocationToProof_data()
    {
        QTest::addColumn<QByteArray>("scenario");
        QTest::newRow("unknown") << QByteArray("close-unknown");
        QTest::newRow("revoked") << QByteArray("close-revoke");
        QTest::newRow("foreign") << QByteArray("close-foreign");
        QTest::newRow("timeout") << QByteArray("close-timeout");
    }

    void asynchronousCloseCannotConvertUnknownOrRevocationToProof()
    {
        QFETCH(QByteArray, scenario);
        behavior(scenario);
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        QString status;
        surface.statusChanged = [&](const QString &value) { status = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        const auto reference = surface.closeReference();
        QSignalSpy completion(&surface, &DesktopViewerSurface::closeCompleted);
        surface.requestClose();
        QTRY_COMPARE_WITH_TIMEOUT(completion.size(), 1, 13000);
        QCOMPARE(completion.first().first().toJsonObject(), (QJsonObject{
            {"schema", "dockpipe.desktop.close/v1"}, {"reference", reference}, {"outcome", "unknown"}}));
        const auto retained = QJsonDocument::fromJson(
            QSettings().value("remote/uncertainDesktopSessions").toByteArray()).array();
        QCOMPARE(retained, QJsonArray{reference});
        surface.closeDisplay();
    }

    void synchronousCancellationStopsAsynchronousCallbacks()
    {
        behavior("late-close");
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        QString status;
        surface.statusChanged = [&](const QString &value) { status = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        QSignalSpy completion(&surface, &DesktopViewerSurface::closeCompleted);
        surface.requestClose();
        surface.closeDisplay();
        QTest::qWait(100);
        QCOMPARE(completion.size(), 0);
        QCOMPARE(QJsonDocument::fromJson(QSettings().value("remote/uncertainDesktopSessions").toByteArray()).array().size(), 1);
    }

    void capturePreferencesAreFrozenIntoChannelConfiguration_data()
    {
        QTest::addColumn<QString>("quality");
        QTest::addColumn<int>("rate");
        QTest::addColumn<bool>("virtualDisplay");
        for (const auto *quality : {"low", "balanced", "high", "ultra"}) {
            for (int rate : {10, 15, 24, 30, 60}) {
                for (bool virtualDisplay : {false, true}) {
                    const auto name = QByteArray(quality) + '-' + QByteArray::number(rate) + (virtualDisplay ? "-virtual" : "-existing");
                    QTest::newRow(name.constData()) << QString::fromLatin1(quality) << rate << virtualDisplay;
                }
            }
        }
    }

    void capturePreferencesAreFrozenIntoChannelConfiguration()
    {
        QFETCH(QString, quality);
        QFETCH(int, rate);
        QFETCH(bool, virtualDisplay);
        auto selected = options("view");
        QJsonObject capture{{"quality", quality}, {"frame_rate", rate}};
        if (virtualDisplay)
            capture.insert("display_source", "virtual");
        selected.insert("capture", capture);
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        QString status;
        surface.statusChanged = [&](const QString &value) { status = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        QVERIFY(surface.captureObservation().isEmpty()); // Ready alone has no decoded dimensions.
        QCOMPARE(count("channel"), 1);
        for (const auto &entry : events()) {
            if (entry.toObject().value("event") != "channel")
                continue;
            const auto configuration = entry.toObject().value("configuration").toObject();
            QCOMPARE(configuration.value("schema").toString(), QString("dockpipe.desktop.channel/v2"));
            QCOMPARE(configuration.value("capture").toObject(), capture);
        }
        const QJsonObject display{{"scale", "actual"}, {"hide_cursor", false}};
        QVERIFY(QMetaObject::invokeMethod(&surface, "setPresentationOptions", Qt::DirectConnection, Q_ARG(QJsonObject, display)));
        QCOMPARE(surface.property("presentationOptions").toJsonObject(), display);
        surface.setPresentationOptions({{"scale", "actual"}, {"hide_cursor", false}, {"capture", capture}});
        QCOMPARE(surface.presentationOptions(), display); // Reject without broadening/reconnecting.
        QCOMPARE(count("channel"), 1);
        surface.closeDisplay();
    }

    void invalidCapturePreferencesFailBeforeEffects_data()
    {
        QTest::addColumn<QJsonObject>("capture");
        QTest::newRow("source-null") << QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"display_source", QJsonValue::Null}};
        QTest::newRow("source-bool") << QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"display_source", true}};
        QTest::newRow("source-empty") << QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"display_source", ""}};
        QTest::newRow("source-existing") << QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"display_source", "existing"}};
        QTest::newRow("source-unknown") << QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"display_source", "mirror"}};
        QTest::newRow("missing") << QJsonObject{{"quality", "low"}};
        QTest::newRow("quality") << QJsonObject{{"quality", "cinema"}, {"frame_rate", 30}};
        QTest::newRow("fps") << QJsonObject{{"quality", "high"}, {"frame_rate", 120}};
        QTest::newRow("fractional") << QJsonObject{{"quality", "high"}, {"frame_rate", 15.5}};
        QTest::newRow("extra") << QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"width", 3840}};
    }

    void invalidCapturePreferencesFailBeforeEffects()
    {
        QFETCH(QJsonObject, capture);
        auto selected = options("view");
        selected.insert("capture", capture);
        QVERIFY_EXCEPTION_THROWN(DesktopViewerSurface(selected, FIXTURE_SCRIPT, nullptr), std::invalid_argument);
        QCOMPARE(count("channel"), 0);
        QCOMPARE(count("ack"), 0);
    }

    void changedDisplaySourceReadyCannotStartMedia()
    {
        QFile behavior(m_root.filePath("behavior"));
        QVERIFY(behavior.open(QIODevice::WriteOnly | QIODevice::Truncate));
        behavior.write("wrong-display-source-ready");
        behavior.close();
        auto selected = options("view");
        selected.insert("capture", QJsonObject{{"quality", "high"}, {"frame_rate", 30}, {"display_source", "virtual"}});
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        QString failure;
        surface.failed = [&](const QString &message) { failure = message; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 5000);
        QVERIFY(surface.captureObservation().isEmpty());
        QVERIFY(surface.decoderStatus().isEmpty());
        surface.closeDisplay();
    }

    void virtualCaptureRequiresExactPresetGeometry()
    {
        for (const auto *quality : {"low", "balanced", "high", "ultra"}) {
            const QJsonObject selected{{"quality", quality}, {"frame_rate", 30}, {"display_source", "virtual"}};
            const auto profile = ViewerCapturePreferences::parse(selected);
            QCOMPARE(profile.json(), selected);
            QVERIFY(profile.acceptsFrameGeometry(profile.maximumWidth(), profile.maximumHeight()));
            QVERIFY(!profile.acceptsFrameGeometry(64, 64));
            QVERIFY(!profile.acceptsFrameGeometry(profile.maximumWidth() + 2, profile.maximumHeight()));
            const auto existing = ViewerCapturePreferences::parse({{"quality", quality}, {"frame_rate", 30}});
            QVERIFY(existing.acceptsFrameGeometry(64, 64));
            QVERIFY(!existing.json().contains("display_source"));
        }
    }

    void deniedCatalogCannotCreateAViewer()
    {
        auto denied = options("control");
        auto catalog = denied.value("catalog").toObject();
        catalog.insert("input", false);
        denied.insert("catalog", catalog);
        QVERIFY_EXCEPTION_THROWN(DesktopViewerSurface(denied, FIXTURE_SCRIPT, nullptr), std::invalid_argument);
        QCOMPARE(count("ack"), 0);
        QCOMPARE(count("channel"), 0);
    }

#ifdef VIDEO_FIXTURE_DIRECTORY
    void cursorTracksLiveImageLetterboxAndLeave()
    {
        QVERIFY(prepareMediaFixture());
        QWidget parent;
        parent.setCursor(Qt::CrossCursor);
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, &parent);
        surface.resize(200, 100);
        const auto *globalCursor = QApplication::overrideCursor();
        QEnterEvent enter(QPointF(100, 50), QPointF(100, 50), surface.mapToGlobal(QPoint(100, 50)));
        QApplication::sendEvent(&surface, &enter);
        QCOMPARE(surface.cursor().shape(), Qt::CrossCursor);
        QVERIFY(!surface.testAttribute(Qt::WA_SetCursor));
        surface.setActive(true);
        // Pointer was stationary before the first decoded frame arrived.
        QTRY_COMPARE_WITH_TIMEOUT(surface.cursor().shape(), Qt::BlankCursor, 5000);
        QCOMPARE(QApplication::overrideCursor(), globalCursor);
        QMouseEvent letterbox(QEvent::MouseMove, QPointF(10, 50), QPointF(10, 50),
            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&surface, &letterbox);
        QCOMPARE(surface.cursor().shape(), Qt::CrossCursor);
        QVERIFY(!surface.testAttribute(Qt::WA_SetCursor));
        QMouseEvent image(QEvent::MouseMove, QPointF(100, 50), QPointF(100, 50),
            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&surface, &image);
        QCOMPARE(surface.cursor().shape(), Qt::BlankCursor);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&surface, &leave);
        QCOMPARE(surface.cursor().shape(), Qt::CrossCursor);
        QVERIFY(!surface.testAttribute(Qt::WA_SetCursor));
        QCOMPARE(count("packet", 2), 0); // Hover never adds view-only input.
        surface.closeDisplay();
        QCOMPARE(QApplication::overrideCursor(), globalCursor);
    }

    void cursorRefreshesOnResizeAndRestoresOnFocusLossAndClose()
    {
        QVERIFY(prepareMediaFixture());
        QWidget parent;
        parent.setCursor(Qt::IBeamCursor);
        auto surface = std::make_unique<DesktopViewerSurface>(options("view"), FIXTURE_SCRIPT, &parent);
        surface->resize(200, 100);
        QEnterEvent enter(QPointF(25, 50), QPointF(25, 50), surface->mapToGlobal(QPoint(25, 50)));
        QApplication::sendEvent(surface.get(), &enter);
        surface->setActive(true);
        const auto decoded = [&] {
            const auto color = surface->grab().toImage().pixelColor(62, 50);
            return color.red() > 200 && color.green() < 40;
        };
        QTRY_VERIFY_WITH_TIMEOUT(decoded(), 5000);
        QCOMPARE(surface->cursor().shape(), Qt::IBeamCursor); // Letterbox.
        surface->resize(100, 100);
        QResizeEvent resize(QSize(100, 100), QSize(200, 100));
        QApplication::sendEvent(surface.get(), &resize);
        QCOMPARE(surface->cursor().shape(), Qt::BlankCursor); // No mouse movement.
        QFocusEvent focusOut(QEvent::FocusOut);
        QApplication::sendEvent(surface.get(), &focusOut);
        QCOMPARE(surface->cursor().shape(), Qt::IBeamCursor);
        QApplication::sendEvent(surface.get(), &resize);
        QCOMPARE(surface->cursor().shape(), Qt::IBeamCursor); // Focus loss stays restored.
        QApplication::sendEvent(surface.get(), &enter);
        QCOMPARE(surface->cursor().shape(), Qt::BlankCursor);
        surface->requestClose();
        QCOMPARE(surface->cursor().shape(), Qt::IBeamCursor);
        QVERIFY(!surface->testAttribute(Qt::WA_SetCursor));
        surface.reset();
        QCOMPARE(parent.cursor().shape(), Qt::IBeamCursor);
        QVERIFY(!QApplication::overrideCursor());
    }

    void actualSizeScrollsAllPixelsWithExactPointerMapping()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Physical keyboard control requires an X11 display.");
        QVERIFY(prepareMediaFixture());
        auto selected = options("control");
        selected.insert("capture", QJsonObject{{"quality", "low"}, {"frame_rate", 10}});
        selected.insert("display", QJsonObject{{"scale", "actual"}, {"hide_cursor", true}});
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        surface.resize(32, 32);
        const auto red = [&] {
            const auto color = surface.grab().toImage().pixelColor(4, 4);
            return color.red() > 200 && color.green() < 40;
        };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(red(), 5000);
        auto *horizontal = surface.findChild<QScrollBar *>("nativeDesktopHorizontalScroll");
        auto *vertical = surface.findChild<QScrollBar *>("nativeDesktopVerticalScroll");
        QVERIFY(horizontal && vertical);
        QVERIFY(horizontal->maximum() > 0 && vertical->maximum() > 0);
        QVERIFY(!horizontal->isHidden() && !vertical->isHidden());
        horizontal->setValue(horizontal->maximum());
        vertical->setValue(vertical->maximum());
        QCOMPARE(count("packet", 2), 0); // Local scrollbars never post remote input.
        const QPointF lastPixel(horizontal->pageStep() - 1, vertical->pageStep() - 1);
        const auto mouse = [&](QEvent::Type type, QPointF position) {
            QMouseEvent event(type, position, position, Qt::LeftButton,
                type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&surface, &event);
        };
        mouse(QEvent::MouseButtonPress, lastPixel);
        mouse(QEvent::MouseButtonRelease, lastPixel);
        QTRY_COMPARE_WITH_TIMEOUT(count("packet", 2), 2, 2000);
        QJsonArray input;
        for (const auto &entry : events()) {
            if (entry.toObject().value("kind").toInt() == 2)
                input.append(entry.toObject().value("input"));
        }
        QCOMPARE(input.first().toObject().value("x").toDouble(), 63.0 / 64.0);
        QCOMPARE(input.first().toObject().value("y").toDouble(), 63.0 / 64.0);
        mouse(QEvent::MouseButtonPress, QPointF(horizontal->pageStep() + 1, 4));
        mouse(QEvent::MouseButtonRelease, QPointF(horizontal->pageStep() + 1, 4));
        QTest::qWait(50);
        QCOMPARE(count("packet", 2), 2); // Scrollbar/gutter excluded from remote image.
        const QJsonObject fit{{"scale", "fit"}, {"hide_cursor", false}};
        QVERIFY(QMetaObject::invokeMethod(&surface, "setPresentationOptions", Qt::DirectConnection, Q_ARG(QJsonObject, fit)));
        QVERIFY(horizontal->isHidden() && vertical->isHidden());
        QCOMPARE(surface.presentationOptions(), fit);
        QMouseEvent movement(QEvent::MouseMove, QPointF(16, 16), QPointF(16, 16),
            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&surface, &movement);
        QVERIFY(surface.cursor().shape() != Qt::BlankCursor);
        surface.setPresentationOptions({{"scale", "fit"}, {"hide_cursor", true}});
        QCOMPARE(surface.cursor().shape(), Qt::BlankCursor);
        const auto before = surface.presentationOptions();
        surface.setPresentationOptions({{"scale", "stretch"}, {"hide_cursor", true}});
        QCOMPARE(surface.presentationOptions(), before);
        surface.setPresentationOptions({{"scale", "actual"}, {"hide_cursor", "yes"}});
        QCOMPARE(surface.presentationOptions(), before);
        QCOMPARE(count("channel"), 1);
        surface.requestClose();
        QVERIFY(horizontal->isHidden() && vertical->isHidden());
        QVERIFY(surface.cursor().shape() != Qt::BlankCursor);
        surface.closeDisplay();
    }

    void actualSizeUsesNaturalPixelsRatherThanFitScaling()
    {
        QVERIFY(prepareMediaFixture());
        auto selected = options("view");
        selected.insert("display", QJsonObject{{"scale", "actual"}, {"hide_cursor", false}});
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        surface.resize(200, 100);
        surface.setActive(true);
        const auto natural = [&] {
            const auto image = surface.grab().toImage();
            return image.pixelColor(76, 50).red() > 200 && image.pixelColor(62, 50).red() < 40;
        };
        QTRY_VERIFY_WITH_TIMEOUT(natural(), 5000);
        surface.setPresentationOptions({{"scale", "fit"}, {"hide_cursor", false}});
        QTRY_VERIFY_WITH_TIMEOUT(surface.grab().toImage().pixelColor(62, 50).red() > 200, 2000);
        surface.closeDisplay();
    }

    void captureObservationRequiresReadyAndDecodedFrameAndClearsOnClose()
    {
        QVERIFY(prepareMediaFixture());
        auto selected = options("view");
        const QJsonObject profile{{"quality", "low"}, {"frame_rate", 10}};
        selected.insert("capture", profile);
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        QVERIFY(surface.captureObservation().isEmpty());
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(surface.captureObservation().value("width").toInt(), 64, 5000);
        const auto observed = surface.captureObservation();
        QCOMPARE(observed.value("capture").toObject(), profile);
        QCOMPARE(observed.value("height").toInt(), 64);
        QCOMPARE(observed.value("decoded_frames").toInteger(), 1);
        QCOMPARE(surface.decoderStatus().value("requested").toString(), QString("software"));
        QCOMPARE(surface.decoderStatus().value("backend").toString(), QString("software"));
        surface.requestClose();
        QVERIFY(surface.captureObservation().isEmpty());
        QVERIFY(surface.decoderStatus().isEmpty());
        surface.closeDisplay();
        QVERIFY(surface.captureObservation().isEmpty());
    }

    void captureObservationRejectsUnverifiedOrOutOfProfileFrames_data()
    {
        QTest::addColumn<QByteArray>("scenario");
        QTest::newRow("wrong-profile-ready") << QByteArray("wrong-capture-ready");
        QTest::newRow("frame-exceeds-low-cap") << QByteArray("profile-overflow");
    }

    void captureObservationRejectsUnverifiedOrOutOfProfileFrames()
    {
        QFETCH(QByteArray, scenario);
        QVERIFY(prepareMediaFixture());
        behavior(scenario);
        auto selected = options("view");
        if (scenario == "profile-overflow")
            selected.insert("capture", QJsonObject{{"quality", "low"}, {"frame_rate", 10}});
        DesktopViewerSurface surface(selected, FIXTURE_SCRIPT, nullptr);
        QString failure;
        surface.failed = [&](const QString &value) { failure = value; };
        surface.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 5000);
        QVERIFY(surface.captureObservation().isEmpty());
        surface.closeDisplay();
    }

    void adjacentFramesInOneRelayBurstAreBothDecoded()
    {
        QVERIFY(prepareMediaFixture());
        behavior("media-burst");
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(surface.captureObservation().value("decoded_frames").toInteger(), 2, 5000);
        QCOMPARE(surface.captureObservation().value("last_pts_us").toInteger(), 16667);
        surface.closeDisplay();
    }

    void encodedSequenceGapDropsDependentFrameAndRecoversAtKeyframe()
    {
        QVERIFY(prepareMediaFixture());
        QFile::remove(m_root.filePath("gap-trigger"));
        QFile::remove(m_root.filePath("recovered.h264"));
        QFile manifest(QDir(VIDEO_FIXTURE_DIRECTORY).filePath("fixture.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto frames = QJsonDocument::fromJson(manifest.readAll()).object().value("frames").toArray();
        QVERIFY(QFile::copy(QDir(VIDEO_FIXTURE_DIRECTORY).filePath(frames.at(1).toObject().value("file").toString()),
            m_root.filePath("recovered.h264")));
        behavior("media-gap");
        DesktopViewerSurface surface(options("view"), FIXTURE_SCRIPT, nullptr);
        surface.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(surface.captureObservation().value("decoded_frames").toInteger(), 1, 5000);
        const int requested = count("packet", 3);
        QFile trigger(m_root.filePath("gap-trigger"));
        QVERIFY(trigger.open(QIODevice::WriteOnly));
        trigger.close();
        QTRY_COMPARE_WITH_TIMEOUT(surface.captureObservation().value("decoded_frames").toInteger(), 2, 5000);
        QVERIFY(count("packet", 3) > requested);
        QCOMPARE(surface.captureObservation().value("last_pts_us").toInteger(), 200000);
        QCOMPARE(surface.decoderStatus().value("backend").toString(), QString("software"));
        QCOMPARE(count("channel"), 1);
        surface.closeDisplay();
    }

    void decodedFrameUsesLetterboxCoordinatesAndReleasesOutside()
    {
        if (!dockpipe::desktop::InputMapping().nativeMappingAvailable())
            QSKIP("Physical keyboard control requires an X11 display; decoder fixtures remain available.");
        QFile manifest(QDir(VIDEO_FIXTURE_DIRECTORY).filePath("fixture.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto fixture = QJsonDocument::fromJson(manifest.readAll()).object();
        const auto first = fixture.value("frames").toArray().first().toObject();
        QFile::remove(m_root.filePath("media.h264"));
        QVERIFY(QFile::copy(QDir(VIDEO_FIXTURE_DIRECTORY).filePath(first.value("file").toString()),
            m_root.filePath("media.h264")));
        behavior("media");
        DesktopViewerSurface surface(options("control"), FIXTURE_SCRIPT, nullptr);
        surface.resize(200, 100);
        QString status;
        QString failure;
        surface.statusChanged = [&](const QString &value) { status = value; };
        surface.failed = [&](const QString &value) { failure = value; };
        surface.setActive(true);
        const auto isRed = [&] {
            const auto pixel = surface.grab().toImage().pixelColor(62, 50);
            return pixel.red() > 200 && pixel.green() < 40 && pixel.blue() < 40;
        };
        QTRY_VERIFY_WITH_TIMEOUT(isRed(), 5000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const auto mouse = [&](QEvent::Type type, QPointF position) {
            QMouseEvent event(type, position, position, Qt::LeftButton,
                type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&surface, &event);
        };
        mouse(QEvent::MouseButtonPress, QPointF(10, 50));
        mouse(QEvent::MouseButtonRelease, QPointF(10, 50));
        mouse(QEvent::MouseButtonPress, QPointF(100, 50));
        mouse(QEvent::MouseButtonRelease, QPointF(10, 50));
        QTRY_COMPARE_WITH_TIMEOUT(count("packet", 2), 2, 2000);
        QJsonArray input;
        for (const auto &entry : events()) {
            if (entry.toObject().value("kind").toInt() == 2)
                input.append(entry.toObject().value("input"));
        }
        QCOMPARE(input[0].toObject().value("kind").toString(), QString("button"));
        QCOMPARE(input[0].toObject().value("button").toInt(), 0);
        QCOMPARE(input[0].toObject().value("x").toDouble(), 0.5);
        QCOMPARE(input[0].toObject().value("y").toDouble(), 0.5);
        QVERIFY(input[0].toObject().value("down").toBool());
        QVERIFY(!input[1].toObject().value("down").toBool());
        QCOMPARE(input[1].toObject().value("x").toDouble(), 0.5);
        for (int index = 0; index < 1000; ++index) {
            const QPointF position(50 + index % 100, 50);
            QMouseEvent movement(QEvent::MouseMove, position, position,
                Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&surface, &movement);
        }
        QTRY_COMPARE_WITH_TIMEOUT(count("packet", 2), 3, 2000);
        const auto latest = events().last().toObject().value("input").toObject();
        QCOMPARE(latest.value("kind").toString(), QString("move"));
        QCOMPARE(latest.value("x").toDouble(), 0.99);
        surface.closeDisplay();
    }
#endif

    void activeDestructorStopsBothProcesses()
    {
        auto surface = std::make_unique<DesktopViewerSurface>(options("view"), FIXTURE_SCRIPT, nullptr);
        QString status;
        surface->statusChanged = [&](const QString &value) { status = value; };
        surface->setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(status.startsWith("Connected"), 5000);
        surface.reset();
        QVERIFY(QJsonDocument::fromJson(QSettings().value("remote/uncertainDesktopSessions").toByteArray()).array().isEmpty());
    }
};

QTEST_MAIN(DesktopViewerSurfaceTest)
#include "DesktopViewerSurfaceTest.moc"
