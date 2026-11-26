#include "GpuVideoWidget.h"
#include "H264Decoder.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

using namespace dockpipe::desktop;

class GpuVideoWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void cudaTexturesPreserveColorsAndReleaseAcrossResets()
    {
        if (qEnvironmentVariable("DOCKPIPE_TEST_H264_HARDWARE") != "1")
            QSKIP("CUDA/OpenGL qualification requires the explicit hardware-test environment.");
        QFile manifest(QDir(FIXTURE_DIRECTORY).filePath("fixture.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto fixtures = QJsonDocument::fromJson(manifest.readAll()).object().value("frames").toArray();
        GpuVideoWidget surface(nullptr);
        surface.setWindowFlag(Qt::BypassWindowManagerHint);
        surface.resize(256, 256);
        surface.show();
        QTRY_VERIFY_WITH_TIMEOUT(surface.available(), 5000);
        H264Decoder decoder({}, H264Decoder::Preference::Hardware);
        quint64 sequence = 0;
        for (int round = 0; round < 3; ++round) {
            for (const auto &value : fixtures) {
                const auto fixture = value.toObject();
                QFile input(QDir(FIXTURE_DIRECTORY).filePath(fixture.value("file").toString()));
                QVERIFY(input.open(QIODevice::ReadOnly));
                const auto bytes = input.readAll();
                QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                    fixture.value("sha256").toString().toLatin1());
                VideoFrame frame{++sequence, sequence * 33333, 64, 64, true,
                    std::vector<std::uint8_t>(bytes.begin(), bytes.end())};
                NativeVideoFrame native;
                H264Decoder::Timings timings;
                const auto image = decoder.decode(frame, {}, &timings, [&](NativeVideoFrame output) {
                    native = std::move(output);
                    return true;
                });
                QVERIFY(image.isNull());
                QVERIFY(native);
                QCOMPARE(timings.downloads, 0u);
                QCOMPARE(timings.conversions, 0u);
                const auto previous = surface.observation().value("painted_frames").toInteger();
                surface.present(std::move(native), surface.rect());
                QTRY_VERIFY_WITH_TIMEOUT(surface.observation().value("painted_frames").toInteger() > previous, 3000);
                QVERIFY(surface.available());
                const auto captured = surface.grabFramebuffer();
                const auto colors = fixture.value("colors").toArray();
                for (int index = 0; index < colors.size(); ++index) {
                    const auto expected = colors.at(index).toArray();
                    const auto pixel = captured.pixelColor((2 * index + 1) * captured.width() / 8, captured.height() / 2);
                    QVERIFY(qAbs(pixel.red() - expected.at(0).toInt()) < 32);
                    QVERIFY(qAbs(pixel.green() - expected.at(1).toInt()) < 32);
                    QVERIFY(qAbs(pixel.blue() - expected.at(2).toInt()) < 32);
                }
            }
            surface.reset();
            QCOMPARE(surface.observation().value("registered_resources").toInt(), 0);
            QVERIFY(!surface.observation().value("device_retained").toBool());
            surface.show();
        }
        QCOMPARE(QString::fromStdString(decoder.backend()), QString("cuda"));
    }
};

QTEST_MAIN(GpuVideoWidgetTest)
#include "GpuVideoWidgetTest.moc"
