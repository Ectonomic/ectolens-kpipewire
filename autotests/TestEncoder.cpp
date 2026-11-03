// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2026 Arjen Hiemstra <ahiemstra@heimr.nl>

#include <QtTest>

#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include "encoder_p.h"
#include "gifencoder_p.h"
#include "h264vaapiencoder_p.h"
#include "libopenh264encoder_p.h"
#include "libvpxencoder_p.h"
#include "libvpxvp9encoder_p.h"
#include "libwebpencoder_p.h"
#include "libx264encoder_p.h"
#include "namedencoder_p.h"
#include "pipewirebaseencodedstream.h"
#include "pipewireproduce_p.h"
#include "vaapiutils_p.h"

using namespace Qt::StringLiterals;

class TestProduce : public PipeWireProduce
{
public:
    TestProduce()
        : PipeWireProduce(PipeWireBaseEncodedStream::Encoder::NoEncoder, 0, 0, 0, Fraction{.numerator = 1, .denominator = 24})
    {
    }

    void processPacket([[maybe_unused]] AVPacket *packet) override
    {
    }

    // Expose the resize state reset and let the test drive the queues it
    // clears. In production these are filled while the workers are running.
    using PipeWireProduce::discardFrameState;

    void primeFrameState()
    {
        m_pendingFilterFrames = 3;
        m_pendingEncodeFrames = 2;
    }

    bool frameStateCleared() const
    {
        return m_pendingFilterFrames == 0 && m_pendingEncodeFrames == 0;
    }
};

// A produce at 60 frames per second, for checking settings that depend on the rate.
class SixtyProduce : public PipeWireProduce
{
public:
    SixtyProduce()
        : PipeWireProduce(PipeWireBaseEncodedStream::Encoder::NoEncoder, 0, 0, 0, Fraction{.numerator = 60, .denominator = 1})
    {
    }
    void processPacket([[maybe_unused]] AVPacket *packet) override
    {
    }
};

// This is a pretty simple smoke test that verifies all the encoders can
// initialize properly. This verifies that things like filter chains are correct.
class TestEncoder : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        m_produce = std::make_unique<TestProduce>();
    }

    void testEncoder_data()
    {
        QTest::addColumn<std::shared_ptr<Encoder>>("encoder");
        QTest::addColumn<QByteArray>("avcodecEncoder");

        QTest::addRow("h264_vaapi_main") << std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::Main, m_produce.get())) << "h264_vaapi"_ba;
        QTest::addRow("h264_vaapi_baseline") << std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::Baseline, m_produce.get()))
                                             << "h264_vaapi"_ba;
        QTest::addRow("h264_vaapi_high") << std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::High, m_produce.get())) << "h264_vaapi"_ba;
        //
        QTest::addRow("x264_main") << std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::Main, m_produce.get())) << "libx264"_ba;
        QTest::addRow("x264_baseline") << std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::Baseline, m_produce.get())) << "libx264"_ba;
        QTest::addRow("x264_high") << std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::High, m_produce.get())) << "libx264"_ba;

        QTest::addRow("openh264_main") << std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::Main, m_produce.get())) << "libopenh264"_ba;
        QTest::addRow("openh264_baseline") << std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::Baseline, m_produce.get()))
                                           << "libopenh264"_ba;
        QTest::addRow("openh264_high") << std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::High, m_produce.get())) << "libopenh264"_ba;

        QTest::addRow("vp8") << std::shared_ptr<Encoder>(new LibVpxEncoder(m_produce.get())) << "libvpx"_ba;
        QTest::addRow("vp9") << std::shared_ptr<Encoder>(new LibVpxVp9Encoder(m_produce.get())) << "libvpx-vp9"_ba;

        QTest::addRow("gif") << std::shared_ptr<Encoder>(new GifEncoder(m_produce.get())) << "gif"_ba;
        QTest::addRow("webp") << std::shared_ptr<Encoder>(new LibWebPEncoder(m_produce.get())) << "libwebp"_ba;
    }

    void testEncoder()
    {
        QFETCH(std::shared_ptr<Encoder>, encoder);
        QFETCH(QByteArray, avcodecEncoder);

        if (!avcodec_find_encoder_by_name(avcodecEncoder.data())) {
            QSKIP("Skipping because the encoder was not found");
        }

        if (avcodecEncoder.contains("vaapi") && VaapiUtils::instance()->devicePath().isEmpty()) {
            QSKIP("Skipping because hardware encoding is not supported on this device");
        }

        QVERIFY(encoder->initialize(QSize(512, 512)));
    }

    // Ectolens: every explicit setting reaches x264 as given.
    void testX264ExplicitSettings_data()
    {
        QTest::addColumn<bool>("fullColor");
        QTest::addRow("4:2:0") << false;
        QTest::addRow("4:4:4") << true;
    }

    void testX264ExplicitSettings()
    {
        QFETCH(bool, fullColor);
        if (!avcodec_find_encoder_by_name("libx264")) {
            QSKIP("Skipping because libx264 was not found");
        }
        SixtyProduce produce;
        PipeWireBaseEncodedStream::EncoderSettings settings;
        settings.qualityLevel = 16;
        settings.speedPreset = u"faster"_s;
        settings.keyframeIntervalSeconds = 2;
        settings.bFrames = 2;
        settings.fullColor = fullColor;
        settings.extraOptions = u"aq-mode=3"_s;
        LibX264Encoder encoder(Encoder::H264Profile::Main, &produce);
        encoder.setEncoderSettings(settings);
        QVERIFY(encoder.initialize(QSize(640, 360)));

        AVCodecContext *context = encoder.avCodecContext();
        QCOMPARE(context->gop_size, 120);
        QCOMPARE(context->max_b_frames, 2);
        QCOMPARE(context->pix_fmt, fullColor ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P);
        QCOMPARE(context->profile, fullColor ? AV_PROFILE_H264_HIGH_444_PREDICTIVE : AV_PROFILE_H264_HIGH);

        double crf = 0;
        QCOMPARE(av_opt_get_double(context, "crf", AV_OPT_SEARCH_CHILDREN, &crf), 0);
        QCOMPARE(crf, 16.0);
        uint8_t *value = nullptr;
        QCOMPARE(av_opt_get(context, "preset", AV_OPT_SEARCH_CHILDREN, &value), 0);
        QCOMPARE(QByteArray(reinterpret_cast<char *>(value)), "faster"_ba);
        av_freep(&value);
        QCOMPARE(av_opt_get(context, "x264-params", AV_OPT_SEARCH_CHILDREN, &value), 0);
        QVERIFY(QByteArray(reinterpret_cast<char *>(value)).contains("aq-mode=3"));
        av_freep(&value);
    }

    // Ectolens: encoders chosen by name open with the settings translated into
    // their own options. NVENC rows touch the graphics card, so they only run
    // when ECTOLENS_GPU_TESTS=1 (the owner's GPU waits for his word).
    void testNamedEncoder_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("qualityOption");
        QTest::addColumn<bool>("fullColor");
        QTest::addColumn<bool>("gpu");
        QTest::addRow("libx265") << u"libx265"_s << "crf"_ba << false << false;
        QTest::addRow("libx265 4:4:4") << u"libx265"_s << "crf"_ba << true << false;
        QTest::addRow("libsvtav1") << u"libsvtav1"_s << "crf"_ba << false << false;
        QTest::addRow("h264_nvenc") << u"h264_nvenc"_s << "cq"_ba << false << true;
        QTest::addRow("h264_nvenc 4:4:4") << u"h264_nvenc"_s << "cq"_ba << true << true;
        QTest::addRow("hevc_nvenc") << u"hevc_nvenc"_s << "cq"_ba << false << true;
    }

    void testNamedEncoder()
    {
        QFETCH(QString, name);
        QFETCH(QByteArray, qualityOption);
        QFETCH(bool, fullColor);
        QFETCH(bool, gpu);
        if (gpu && qEnvironmentVariable("ECTOLENS_GPU_TESTS") != u"1") {
            QSKIP("GPU test: set ECTOLENS_GPU_TESTS=1 to run it");
        }
        if (!avcodec_find_encoder_by_name(name.toUtf8().constData())) {
            QSKIP("Skipping because the encoder is not in this FFmpeg build");
        }
        SixtyProduce produce;
        PipeWireBaseEncodedStream::EncoderSettings settings;
        settings.qualityLevel = 20;
        settings.keyframeIntervalSeconds = 2;
        settings.fullColor = fullColor;
        NamedEncoder encoder(name, &produce);
        encoder.setEncoderSettings(settings);
        QVERIFY(encoder.initialize(QSize(640, 360)));
        AVCodecContext *context = encoder.avCodecContext();
        QCOMPARE(context->gop_size, 120);
        QCOMPARE(context->pix_fmt, fullColor ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P);
        int64_t quality = -1;
        QCOMPARE(av_opt_get_int(context, qualityOption.constData(), AV_OPT_SEARCH_CHILDREN, &quality), 0);
        QCOMPARE(quality, 20);
    }

    void testFullColorFallsBackWhereUnsupported()
    {
        if (!avcodec_find_encoder_by_name("libsvtav1")) {
            QSKIP("Skipping because libsvtav1 is not in this FFmpeg build");
        }
        SixtyProduce produce;
        PipeWireBaseEncodedStream::EncoderSettings settings;
        settings.fullColor = true;
        NamedEncoder encoder(u"libsvtav1"_s, &produce);
        encoder.setEncoderSettings(settings);
        QVERIFY(encoder.initialize(QSize(640, 360)));
        QCOMPARE(encoder.avCodecContext()->pix_fmt, AV_PIX_FMT_YUV420P);
    }

    // Ectolens: the speed check encodes with the real settings and reports a factor.
    void testSpeedCheck()
    {
        if (!avcodec_find_encoder_by_name("libx264")) {
            QSKIP("Skipping because libx264 was not found");
        }
        PipeWireBaseEncodedStream::EncoderSettings settings;
        settings.encoderName = u"libx264"_s;
        settings.qualityLevel = 16;
        settings.speedPreset = u"veryfast"_s;
        const auto check = PipeWireBaseEncodedStream::checkEncodingSpeed(settings, QSize(640, 360), Fraction{60, 1}, 60);
        QVERIFY2(check.ok, qPrintable(check.error));
        QVERIFY(check.realtimeFactor > 0);
        QVERIFY(check.bytes > 0);

        settings.encoderName = u"no_such_encoder"_s;
        const auto bad = PipeWireBaseEncodedStream::checkEncodingSpeed(settings, QSize(640, 360), Fraction{60, 1}, 10);
        QVERIFY(!bad.ok);
        QVERIFY(!bad.error.isEmpty());
    }

    // Without explicit settings the encoder keeps KPipeWire's own behavior.
    void testX264WithoutSettings()
    {
        if (!avcodec_find_encoder_by_name("libx264")) {
            QSKIP("Skipping because libx264 was not found");
        }
        SixtyProduce produce;
        LibX264Encoder encoder(Encoder::H264Profile::Main, &produce);
        QVERIFY(encoder.initialize(QSize(640, 360)));
        QCOMPARE(encoder.avCodecContext()->gop_size, 100);
        QCOMPARE(encoder.avCodecContext()->max_b_frames, 0);
        QCOMPARE(encoder.avCodecContext()->profile, AV_PROFILE_H264_MAIN);
    }

    // A mid-stream source resize is handled by PipeWireProduce::reconfigureStream(),
    // which discards the encoder and creates a fresh one for the new size. Verify
    // every encoder sets up cleanly when (re)created at a second, different size.
    void testReinitializeAtNewSize()
    {
        struct Case {
            const char *name;
            QByteArray avcodecEncoder;
            std::function<std::shared_ptr<Encoder>()> make;
        };
        const std::vector<Case> cases = {
            {"x264",
             "libx264"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::Main, m_produce.get()));
             }},
            {"openh264",
             "libopenh264"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::Main, m_produce.get()));
             }},
            {"vp8",
             "libvpx"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new LibVpxEncoder(m_produce.get()));
             }},
            {"vp9",
             "libvpx-vp9"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new LibVpxVp9Encoder(m_produce.get()));
             }},
            {"h264_vaapi",
             "h264_vaapi"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::Main, m_produce.get()));
             }},
        };

        bool ranAny = false;
        for (const auto &testCase : cases) {
            if (!avcodec_find_encoder_by_name(testCase.avcodecEncoder.data())) {
                continue;
            }
            if (testCase.avcodecEncoder.contains("vaapi") && VaapiUtils::instance()->devicePath().isEmpty()) {
                continue;
            }
            // Build at one size, then build a fresh encoder at a different size,
            // exactly as the mid-stream rebuild does.
            QVERIFY2(testCase.make()->initialize(QSize(512, 512)), testCase.name);
            QVERIFY2(testCase.make()->initialize(QSize(1280, 720)), testCase.name);
            ranAny = true;
        }

        if (!ranAny) {
            QSKIP("No usable encoders available to test");
        }
    }

    // A mid-stream resize replaces the encoder. discardFrameState() must drop
    // the queue accounting for work that belonged to the old encoder.
    void testDiscardFrameStateOnResize()
    {
        m_produce->primeFrameState();
        QVERIFY(!m_produce->frameStateCleared());

        m_produce->discardFrameState();
        QVERIFY(m_produce->frameStateCleared());
    }

private:
    std::unique_ptr<TestProduce> m_produce;
};

QTEST_MAIN(TestEncoder)

#include "TestEncoder.moc"
