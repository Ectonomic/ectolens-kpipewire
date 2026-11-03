/*
    SPDX-FileCopyrightText: 2026 Ectolens contributors
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

    Ectolens: how fast this computer encodes with given settings. The encoder is
    set up exactly as a recording would set it up, then fed a moving test picture
    straight from memory (no screen involved) for a fixed number of frames.
*/
#include "libx264encoder_p.h"
#include "namedencoder_p.h"
#include "pipewirebaseencodedstream.h"
#include "pipewireproduce_p.h"

#include <QElapsedTimer>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

using namespace Qt::StringLiterals;

namespace
{
class SpeedProduce : public PipeWireProduce
{
public:
    explicit SpeedProduce(const Fraction &fps)
        : PipeWireProduce(PipeWireBaseEncodedStream::Encoder::NoEncoder, 0, 0, 0, fps)
    {
    }
    void processPacket([[maybe_unused]] AVPacket *packet) override
    {
    }
};

// Text-like rows that scroll and a block that moves, in 4:2:0 or 4:4:4.
void paint(AVFrame *frame, int n)
{
    const bool full = frame->format == AV_PIX_FMT_YUV444P;
    for (int y = 0; y < frame->height; ++y) {
        uint8_t *row = frame->data[0] + y * frame->linesize[0];
        const int line = (y + n * 3) / 18, inLine = (y + n * 3) % 18;
        for (int x = 0; x < frame->width; ++x) {
            const bool ink = inLine > 4 && inLine < 13 && x > 40 && x < 40 + (line * 97) % std::max(1, frame->width - 80) && ((x / 7 + line) % 5) != 0;
            const bool block = x >= (n * 7) % std::max(1, frame->width - 120) && x < (n * 7) % std::max(1, frame->width - 120) + 120 && y >= 300 % frame->height
                && y < 300 % frame->height + 80;
            row[x] = block ? 120 : ink ? 40 : 235;
        }
    }
    const int cw = full ? frame->width : frame->width / 2, ch = full ? frame->height : frame->height / 2;
    for (int plane = 1; plane < 3; ++plane) {
        for (int y = 0; y < ch; ++y) {
            uint8_t *row = frame->data[plane] + y * frame->linesize[plane];
            for (int x = 0; x < cw; ++x) {
                row[x] = uint8_t(128 + ((x + y + n) % 64 == 0 ? 40 : 0));
            }
        }
    }
}
}

PipeWireBaseEncodedStream::SpeedCheck PipeWireBaseEncodedStream::checkEncodingSpeed(const EncoderSettings &settings, const QSize &size, const Fraction &fps, int frames)
{
    SpeedCheck result;
    SpeedProduce produce(fps);
    std::unique_ptr<::Encoder> encoder;
    const QString name = settings.encoderName.isEmpty() ? u"libx264"_s : settings.encoderName;
    if (name == u"libx264") {
        encoder = std::make_unique<LibX264Encoder>(::Encoder::H264Profile::High, &produce);
    } else {
        encoder = std::make_unique<NamedEncoder>(name, &produce);
    }
    encoder->setEncoderSettings(settings);
    if (!encoder->initialize(size)) {
        result.error = u"%1 could not be opened with these settings."_s.arg(name);
        return result;
    }
    AVCodecContext *context = encoder->avCodecContext();
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    if (av_frame_get_buffer(frame, 0) < 0) {
        result.error = u"Out of memory."_s;
        av_frame_free(&frame);
        av_packet_free(&packet);
        return result;
    }
    const double frameMs = 1000.0 * fps.denominator / std::max<quint32>(fps.numerator, 1);
    QElapsedTimer timer;
    qint64 encodingNs = 0;
    auto drain = [&] {
        while (avcodec_receive_packet(context, packet) == 0) {
            result.bytes += packet->size;
            av_packet_unref(packet);
        }
    };
    for (int n = 0; n < frames; ++n) {
        av_frame_make_writable(frame);
        paint(frame, n);
        frame->pts = int64_t(n * frameMs);
        timer.start();
        if (avcodec_send_frame(context, frame) < 0) {
            result.error = u"%1 stopped encoding."_s.arg(name);
            break;
        }
        drain();
        encodingNs += timer.nsecsElapsed();
    }
    timer.start();
    avcodec_send_frame(context, nullptr);
    drain();
    encodingNs += timer.nsecsElapsed();
    av_frame_free(&frame);
    av_packet_free(&packet);

    const double seconds = encodingNs / 1e9;
    const double videoSeconds = frames * frameMs / 1000.0;
    result.realtimeFactor = seconds > 0 ? videoSeconds / seconds : 0;
    result.ok = result.error.isEmpty();
    return result;
}
