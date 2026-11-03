/*
    SPDX-FileCopyrightText: 2026 Ectolens contributors
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "namedencoder_p.h"

#include <QSize>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include "logging_record.h"

using namespace Qt::StringLiterals;

NamedEncoder::NamedEncoder(const QString &ffmpegName, PipeWireProduce *produce)
    : SoftwareEncoder(produce)
    , m_name(ffmpegName)
{
}

QStringList NamedEncoder::knownEncoders()
{
    return {u"libx264"_s, u"libx265"_s, u"libsvtav1"_s, u"h264_nvenc"_s, u"hevc_nvenc"_s, u"av1_nvenc"_s};
}

bool NamedEncoder::supportsFullColor(const QString &ffmpegName)
{
    // SVT-AV1 takes 4:2:0 only, and NVIDIA's AV1 encoder has no 4:4:4 mode.
    return ffmpegName == u"libx264" || ffmpegName == u"libx265" || ffmpegName == u"h264_nvenc" || ffmpegName == u"hevc_nvenc";
}

NamedEncoder::Family NamedEncoder::family() const
{
    if (m_name == u"libx265") {
        return Family::X265;
    }
    if (m_name == u"libsvtav1") {
        return Family::SvtAv1;
    }
    if (m_name.endsWith(u"_nvenc")) {
        return Family::Nvenc;
    }
    return Family::Other;
}

bool NamedEncoder::initialize(const QSize &size)
{
    const bool fullColor = m_settings.fullColor && supportsFullColor(m_name);
    if (m_settings.fullColor && !fullColor) {
        qCWarning(PIPEWIRERECORD_LOGGING) << m_name << "cannot record full color detail; using 4:2:0";
    }
    const auto colorRange = m_colorRange == PipeWireBaseEncodedStream::ColorRange::Full ? u"full"_s : u"limited"_s;
    m_filterGraphToParse = u"format=%1,pad=ceil(iw/2)*2:ceil(ih/2)*2,scale=out_range=%2"_s.arg(fullColor ? u"yuv444p"_s : u"yuv420p"_s, colorRange);
    if (!createFilterGraph(size)) {
        return false;
    }

    auto codec = avcodec_find_encoder_by_name(m_name.toUtf8().constData());
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << m_name << "is not in this FFmpeg build";
        return false;
    }
    m_avCodecContext = avcodec_alloc_context3(codec);
    if (!m_avCodecContext) {
        return false;
    }
    m_avCodecContext->width = std::ceil(size.width() / 2.0) * 2;
    m_avCodecContext->height = std::ceil(size.height() / 2.0) * 2;
    m_avCodecContext->pix_fmt = fullColor ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P;
    m_avCodecContext->time_base = AVRational{1, 1000};
    m_avCodecContext->color_range = m_colorRange == PipeWireBaseEncodedStream::ColorRange::Full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    m_avCodecContext->gop_size = 120;
    if (m_settings.bFrames) {
        m_avCodecContext->max_b_frames = *m_settings.bFrames;
    }
    const Fraction framerate = m_produce->maxFramerate();
    if (framerate.numerator > 0 && framerate.denominator > 0) {
        m_avCodecContext->framerate = AVRational{int(framerate.numerator), int(framerate.denominator)};
        const qreal fps = qreal(framerate.numerator) / framerate.denominator;
        m_avCodecContext->gop_size = std::max(1, int(std::lround(m_settings.keyframeIntervalSeconds.value_or(2) * fps)));
    }
    if (fullColor && m_name == u"h264_nvenc") {
        m_avCodecContext->profile = AV_PROFILE_H264_HIGH_444_PREDICTIVE;
    }

    AVDictionary *options = buildEncodingOptions();
    maybeLogOptions(options);
    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open" << m_name << av_err2str(result);
        av_dict_free(&options);
        return false;
    }
    av_dict_free(&options);
    return true;
}

AVDictionary *NamedEncoder::buildEncodingOptions()
{
    AVDictionary *options = SoftwareEncoder::buildEncodingOptions();
    // The base class sets an x264-style preset from the encoding preference; each
    // family below sets its own, so drop it.
    av_dict_set(&options, "preset", nullptr, 0);
    const QByteArray preset = m_settings.speedPreset.toUtf8();
    const QByteArray extra = m_settings.extraOptions.toUtf8();

    switch (family()) {
    case Family::X265:
        av_dict_set(&options, "preset", preset.isEmpty() ? "veryfast" : preset.constData(), 0);
        av_dict_set_int(&options, "crf", std::lround(m_settings.qualityLevel.value_or(21)), 0);
        if (!extra.isEmpty()) {
            av_dict_set(&options, "x265-params", extra.constData(), 0);
        }
        break;
    case Family::SvtAv1:
        av_dict_set(&options, "preset", preset.isEmpty() ? "10" : preset.constData(), 0);
        av_dict_set_int(&options, "crf", std::lround(m_settings.qualityLevel.value_or(30)), 0);
        if (!extra.isEmpty()) {
            av_dict_set(&options, "svtav1-params", extra.constData(), 0);
        }
        break;
    case Family::Nvenc:
        // Variable bitrate held to a target quality, the way OBS drives NVENC for recording.
        av_dict_set(&options, "preset", preset.isEmpty() ? "p5" : preset.constData(), 0);
        av_dict_set(&options, "tune", "hq", 0);
        av_dict_set(&options, "rc", "vbr", 0);
        av_dict_set_int(&options, "cq", std::lround(m_settings.qualityLevel.value_or(16)), 0);
        av_dict_set(&options, "b", "0", 0);
        av_dict_set(&options, "spatial-aq", "1", 0);
        if (!extra.isEmpty()) {
            av_dict_parse_string(&options, extra.constData(), "=", ":", 0);
        }
        break;
    case Family::Other:
        if (!preset.isEmpty()) {
            av_dict_set(&options, "preset", preset.constData(), 0);
        }
        if (!extra.isEmpty()) {
            av_dict_parse_string(&options, extra.constData(), "=", ":", 0);
        }
        break;
    }
    return options;
}
