#include "QemuDisplayTransport.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QDateTime>

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {{"b", "bus-address"}, "Explicit task-private D-Bus Unix address", "address"},
        {"console", "QEMU console ID", "id", "0"},
        {"input-address", "Explicit private dedicated QMP input address", "address"},
        {"expected-qemu-pid", "Expected fixture child PID", "pid"},
        {"expected-qemu-start-token", "Expected Linux kernel start token", "token"},
        {"proof-directory", "Write fixed proof receipt/frame then exit after 8 seconds", "directory"},
        {"proof-input", "Send fixed Shift press/release and pointer to disposable firmware guest only"},
        {"raw-only", "Qualify the raw local software protocol without shared-memory capabilities"}
    });
    parser.process(application);
    const auto address = parser.value("bus-address");
    bool validConsole = false;
    const unsigned console = parser.value("console").toUInt(&validConsole);
    if (!address.startsWith("unix:path=") || address.contains(';') ||
        address.size() > 256 || !validConsole || console > 15)
        parser.showHelp(2);
    FrameStore frames;
    QemuDisplayTransport transport(frames, address, console, parser.value("expected-qemu-pid").toInt(),
                                   parser.value("expected-qemu-start-token"), !parser.isSet("raw-only"), parser.value("input-address"));
    DisplayWidget widget(frames);
    widget.setWindowTitle(QStringLiteral("Dockpipe local QEMU display proof"));
    widget.sendInput = [&transport](DisplayInput input) { transport.submit(input); };
    widget.show();
    transport.start();
    bool inputSent = false;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &application, [&] {
        widget.setPointerState(transport.pointerState());
        if (!transport.error().isEmpty())
            widget.setWindowTitle(transport.error());
        if (parser.isSet("proof-input") && transport.connected() &&
            !frames.snapshot().isNull() && !inputSent) {
            inputSent = true;
            transport.submit({DisplayInput::Kind::KeyPress, 42});
            transport.submit({DisplayInput::Kind::KeyRelease, 42});
            transport.submit({widget.absolutePointer ? DisplayInput::Kind::Absolute :
                              DisplayInput::Kind::Relative, 10, 10, frames.snapshot().size()});
        }
    });
    poll.start(50);
    if (parser.isSet("proof-directory")) {
        QTimer::singleShot(8000, &application, [&] {
            const auto image = frames.snapshot();
            const auto output = parser.value("proof-directory");
            const bool saved = !image.isNull() && image.save(output + "/frame.png");
            const bool widgetSaved = widget.grab().save(output + "/widget.png");
            bool nonuniform = false;
            if (!image.isNull()) {
                const auto first = image.pixel(0, 0);
                for (int y = 0; y < image.height() && !nonuniform; ++y)
                    for (int x = 0; x < image.width(); ++x)
                        if (image.pixel(x, y) != first) {
                            nonuniform = true;
                            break;
                        }
            }
            const bool success = saved && nonuniform && frames.revisions() > 0 && transport.error().isEmpty() &&
                (!parser.isSet("proof-input") || transport.inputAcknowledgements() >= 3);
            QJsonObject receipt{
                {"schema", 1}, {"success", success}, {"qt_version", qVersion()},
                {"width", image.width()}, {"height", image.height()},
                {"software_revisions", qint64(frames.revisions())},
                {"shared_scanouts", qint64(transport.sharedScanouts())},
                {"shared_updates", qint64(transport.sharedUpdates())},
                {"raw_scanouts", qint64(transport.rawScanouts())},
                {"raw_batches", qint64(transport.rawBatches())},
                {"delivery_kind", transport.deliveryKind()},
                {"input_acknowledgements", qint64(transport.inputAcknowledgements())},
                {"nonuniform_pixels", nonuniform}, {"frame_saved", saved}, {"widget_saved", widgetSaved},
                {"error", transport.error()}, {"guest_kind", "disposable-firmware-only"}
            };
            transport.stop();
            receipt.insert("worker_joined", true);
            QFile file(output + "/receipt.json");
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(QJsonDocument(receipt).toJson()) < 0)
                application.exit(3);
            else
                application.exit(success ? 0 : 1);
        });
    }
    const int result = application.exec();
    transport.stop();
    return result;
}
