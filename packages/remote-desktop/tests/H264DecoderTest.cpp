#include "H264Decoder.h"
#include "DecoderAllocation.h"
#include "../native/TestSps.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

using dockpipe::desktop::H264Decoder;
using dockpipe::desktop::VideoFrame;

class H264DecoderTest final : public QObject {
    Q_OBJECT
private:
    QJsonObject m_fixture;

    VideoFrame frame(int index)
    {
        const auto record = m_fixture.value("frames").toArray().at(index).toObject();
        QFile input(QDir(FIXTURE_DIRECTORY).filePath(record.value("file").toString()));
        if (!input.open(QIODevice::ReadOnly) || input.size() > 2 * 1024 * 1024)
            throw std::runtime_error("Missing or oversized test fixture.");
        const auto bytes = input.readAll();
        if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != record.value("sha256").toString().toLatin1())
            throw std::runtime_error("The original synthetic fixture changed.");
        return {static_cast<std::uint64_t>(index + 1), static_cast<std::uint64_t>(index * 33333),
            static_cast<std::uint32_t>(m_fixture.value("width").toInt()),
            static_cast<std::uint32_t>(m_fixture.value("height").toInt()), true,
            std::vector<std::uint8_t>(bytes.begin(), bytes.end())};
    }

private slots:
    void initTestCase()
    {
        QFile input(QDir(FIXTURE_DIRECTORY).filePath("fixture.json"));
        QVERIFY(input.open(QIODevice::ReadOnly));
        m_fixture = QJsonDocument::fromJson(input.readAll()).object();
        QCOMPARE(m_fixture.value("width").toInt(), 64);
        QCOMPARE(m_fixture.value("height").toInt(), 64);
        QCOMPARE(m_fixture.value("frames").toArray().size(), 2);
    }

    void codedMacroblockPaddingIsDistinctFromVisibleAndGpuPoolLimits()
    {
        using dockpipe::desktop::permitsCodedAllocation;
        const dockpipe::desktop::H264ProfileBounds high{1920, 1080, 42};
        QVERIFY(permitsCodedAllocation(1920, 1080, high));
        QVERIFY(permitsCodedAllocation(1920, 1088, high));
        QVERIFY(!permitsCodedAllocation(1920, 1089, high));
        QVERIFY(!permitsCodedAllocation(1921, 1088, high));
        QVERIFY(!permitsCodedAllocation(1920, 1152, high)); // GPU padding is not SPS padding.
        QVERIFY(!permitsCodedAllocation(-1, 1088, high));
        QVERIFY(!permitsCodedAllocation(2147483647, 2147483647, high));
        const dockpipe::desktop::H264ProfileBounds ultra{3840, 2160, 52};
        QVERIFY(permitsCodedAllocation(3840, 2160, ultra));
        QVERIFY(!permitsCodedAllocation(3840, 2176, ultra));
    }

    void decodesOriginalColorFramesWithoutStartupDelay()
    {
        H264Decoder decoder;
        for (int index = 0; index < 2; ++index) {
            const auto image = decoder.decode(frame(index));
            QVERIFY(!image.isNull());
            QCOMPARE(image.size(), QSize(64, 64));
            const auto colors = m_fixture.value("frames").toArray().at(index).toObject().value("colors").toArray();
            QCOMPARE(colors.size(), 4);
            for (int band = 0; band < 4; ++band) {
                const auto pixel = image.pixelColor(band * 16 + 8, 32);
                const auto expected = colors[band].toArray();
                QCOMPARE(expected.size(), 3);
                QVERIFY(qAbs(pixel.red() - expected[0].toInt()) < 32);
                QVERIFY(qAbs(pixel.green() - expected[1].toInt()) < 32);
                QVERIFY(qAbs(pixel.blue() - expected[2].toInt()) < 32);
            }
        }
    }

    void healthyResynchronizationPreservesReplayAndSpsGuards()
    {
        H264Decoder decoder;
        QVERIFY(!decoder.decode(frame(0)).isNull());
        decoder.resynchronize();
        QVERIFY(decoder.requiresKeyframe());
        QVERIFY_EXCEPTION_THROWN(decoder.decode(frame(0)), std::invalid_argument);
        auto invalid = frame(1);
        invalid.annexB = dockpipe::desktop::test::annexB(3840, 2160, 52);
        QVERIFY_EXCEPTION_THROWN(decoder.decode(invalid), std::invalid_argument);
        QVERIFY(decoder.requiresKeyframe());
        QVERIFY(!decoder.decode(frame(1)).isNull());
        QVERIFY(!decoder.requiresKeyframe());
    }

    void decodedPictureCanSkipPresentationWithoutResettingTheDecoder()
    {
        H264Decoder decoder;
        H264Decoder::Timings timings;
        decoder.resynchronize();

        QVERIFY(decoder.decode(frame(0), [] { return false; }, &timings).isNull());
        QCOMPARE(timings.decodedFrames, 1u);
        QCOMPARE(timings.skippedOutputs, 1u);
        QCOMPARE(timings.downloads, 0u);
        QCOMPARE(timings.conversions, 0u);
        QVERIFY(!decoder.requiresKeyframe());
        QVERIFY_EXCEPTION_THROWN(decoder.decode(frame(0)), std::invalid_argument);

        QVERIFY(!decoder.decode(frame(1), {}, &timings).isNull());
        QCOMPARE(timings.decodedFrames, 2u);
        QCOMPARE(timings.conversions, 1u);
    }

    void recoverySkipsDependentFramesAndRejectsMislabeledIdr()
    {
        H264Decoder decoder;
        QVERIFY(!decoder.decode(frame(0)).isNull());
        decoder.resynchronize();
        auto dependent = frame(1);
        bool changed = false;
        for (std::size_t index = 0; index + 3 < dependent.annexB.size(); ++index) {
            if (dependent.annexB[index] == 0 && dependent.annexB[index + 1] == 0
                && dependent.annexB[index + 2] == 1 && (dependent.annexB[index + 3] & 0x1f) == 5) {
                dependent.annexB[index + 3] = (dependent.annexB[index + 3] & 0xe0) | 1;
                changed = true;
            }
        }
        QVERIFY(changed);
        QVERIFY_EXCEPTION_THROWN(decoder.decode(dependent), std::invalid_argument);
        QVERIFY(decoder.requiresKeyframe());
        dependent.keyframe = false;
        QVERIFY(decoder.decode(dependent).isNull());
        QVERIFY(decoder.requiresKeyframe());
        auto recovered = frame(1);
        recovered.sequence = 5;
        QVERIFY(!decoder.decode(recovered).isNull());
        QVERIFY(!decoder.requiresKeyframe());
    }

    void repeatedHealthyResynchronizationRetainsDecoderAndColor()
    {
        const bool hardware = qEnvironmentVariable("DOCKPIPE_TEST_H264_HARDWARE") == "1";
        H264Decoder decoder({}, hardware ? H264Decoder::Preference::Hardware : H264Decoder::Preference::Software);
        std::string selectedBackend;
        for (int iteration = 0; iteration < 20; ++iteration) {
            if (iteration)
                decoder.resynchronize();
            auto input = frame(iteration % 2);
            input.sequence = 1 + iteration * 3; // Two packets lost between IDRs.
            input.microseconds = iteration * 100000;
            const auto image = decoder.decode(input);
            QCOMPARE(image.size(), QSize(64, 64));
            if (iteration == 0)
                selectedBackend = decoder.backend();
            QCOMPARE(decoder.backend(), selectedBackend);
            QVERIFY(decoder.fallbackReason().empty());
            QVERIFY(!decoder.requiresKeyframe());
            const auto colors = m_fixture.value("frames").toArray().at(iteration % 2).toObject().value("colors").toArray();
            for (int band = 0; band < 4; ++band) {
                const auto pixel = image.pixelColor(band * 16 + 8, 32);
                const auto expected = colors[band].toArray();
                QVERIFY(qAbs(pixel.red() - expected[0].toInt()) < 32);
                QVERIFY(qAbs(pixel.green() - expected[1].toInt()) < 32);
                QVERIFY(qAbs(pixel.blue() - expected[2].toInt()) < 32);
            }
        }
    }

    void negotiatedLevelsDoNotGloballyWidenSoftwareDecode()
    {
        for (unsigned level : {42U, 51U, 52U}) {
            auto input = frame(0);
            bool changed = false;
            for (std::size_t index = 0; index + 7 < input.annexB.size(); ++index) {
                const auto prefix = input.annexB[index] == 0 && input.annexB[index + 1] == 0
                    && input.annexB[index + 2] == 0 && input.annexB[index + 3] == 1;
                if (prefix && (input.annexB[index + 4] & 0x1f) == 7) {
                    input.annexB[index + 7] = static_cast<std::uint8_t>(level);
                    changed = true;
                    break;
                }
            }
            QVERIFY(changed);
            H264Decoder ordinary;
            QVERIFY_EXCEPTION_THROWN(ordinary.decode(input), std::invalid_argument);
            const dockpipe::desktop::H264ProfileBounds bounds = level == 42
                ? dockpipe::desktop::H264ProfileBounds{1920, 1080, level}
                : dockpipe::desktop::H264ProfileBounds{3840, 2160, level};
            H264Decoder negotiated(bounds);
            QCOMPARE(negotiated.decode(input).size(), QSize(64, 64));
            QCOMPARE(negotiated.backend(), std::string("software"));
        }
    }

    void oversizedSpsIsRejectedBeforeDecoderAllocationDespiteSmallEnvelope()
    {
        auto input = frame(0);
        input.annexB = dockpipe::desktop::test::annexB(3840, 2160, 52);
        H264Decoder ordinary;
        QVERIFY_EXCEPTION_THROWN(ordinary.decode(input), std::invalid_argument);
        H264Decoder ultra({3840, 2160, 52});
        QVERIFY_EXCEPTION_THROWN(ultra.decode(input), std::invalid_argument);
    }

    void invalidSpsDoesNotBecomeAnAutomaticHardwareFallback()
    {
        H264Decoder decoder({}, H264Decoder::Preference::Automatic);
        const auto initialReason = decoder.fallbackReason();
        auto input = frame(0);
        input.annexB = dockpipe::desktop::test::annexB(3840, 2160, 52);
        QVERIFY_EXCEPTION_THROWN(decoder.decode(input), std::invalid_argument);
        QCOMPARE(decoder.fallbackReason(), initialReason);
        QCOMPARE(decoder.backend(), std::string("pending"));
    }

    void actualHardwareDecodeIsQualifiedOnlyWhenExplicitlyRequested()
    {
        if (qEnvironmentVariable("DOCKPIPE_TEST_H264_HARDWARE") != "1")
            QSKIP("Set DOCKPIPE_TEST_H264_HARDWARE=1 for actual local GPU qualification.");
        H264Decoder decoder({}, H264Decoder::Preference::Hardware);
        for (int index = 0; index < 2; ++index) {
            const auto image = decoder.decode(frame(index));
            QCOMPARE(image.size(), QSize(64, 64));
            QVERIFY(decoder.backend() == "cuda" || decoder.backend() == "vaapi");
            QVERIFY(decoder.fallbackReason().empty());
            const auto colors = m_fixture.value("frames").toArray().at(index).toObject().value("colors").toArray();
            for (int band = 0; band < 4; ++band) {
                const auto pixel = image.pixelColor(band * 16 + 8, 32);
                const auto expected = colors[band].toArray();
                QVERIFY(qAbs(pixel.red() - expected[0].toInt()) < 32);
                QVERIFY(qAbs(pixel.green() - expected[1].toInt()) < 32);
                QVERIFY(qAbs(pixel.blue() - expected[2].toInt()) < 32);
            }
        }
    }

    void rejectsReplayAndEnvelopeGeometryMismatch()
    {
        H264Decoder decoder;
        auto original = frame(0);
        QVERIFY(!decoder.decode(original).isNull());
        QVERIFY_EXCEPTION_THROWN(decoder.decode(original), std::invalid_argument);
        H264Decoder mismatch;
        original.width = 128;
        QVERIFY_EXCEPTION_THROWN(mismatch.decode(original), std::invalid_argument);
    }
};

QTEST_GUILESS_MAIN(H264DecoderTest)
#include "H264DecoderTest.moc"
