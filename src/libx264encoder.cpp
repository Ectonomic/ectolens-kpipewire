/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "libx264encoder_p.h"

#include <QSize>
#include <algorithm>
#include <cmath>
#include <QThread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
}

#include "logging_record.h"

#ifndef AV_PROFILE_H264_BASELINE // ffmpeg before 8.0
#define AV_PROFILE_H264_BASELINE FF_PROFILE_H264_BASELINE
#define AV_PROFILE_H264_MAIN FF_PROFILE_H264_MAIN
#define AV_PROFILE_H264_HIGH FF_PROFILE_H264_HIGH
#endif

using namespace Qt::StringLiterals;

LibX264Encoder::LibX264Encoder(H264Profile profile, PipeWireProduce *produce)
    : SoftwareEncoder(produce)
    , m_profile(profile)
{
    auto colorRange = m_colorRange == PipeWireBaseEncodedStream::ColorRange::Full ? u"full"_s : u"limited"_s;

    // Adjust the filter graph to ensure we are using an even frame size using a
    // pad filter. Otherwise the size adjustment below will insert a row/column
    // of garbage instead of black.
    m_filterGraphToParse = u"format=yuv420p,pad=ceil(iw/2)*2:ceil(ih/2)*2,scale=out_range=%1"_s.arg(colorRange);
}

bool LibX264Encoder::initialize(const QSize &size)
{
    // Built here, not in the constructor: the color range and the color detail
    // are only known once the produce has handed over its settings.
    const bool fullColor = m_settings.fullColor;
    const auto colorRange = m_colorRange == PipeWireBaseEncodedStream::ColorRange::Full ? u"full"_s : u"limited"_s;
    m_filterGraphToParse = u"format=%1,pad=ceil(iw/2)*2:ceil(ih/2)*2,scale=out_range=%2"_s.arg(fullColor ? u"yuv444p"_s : u"yuv420p"_s, colorRange);
    createFilterGraph(size);

    auto codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "libx264 codec not found";
        return false;
    }

    m_avCodecContext = avcodec_alloc_context3(codec);
    if (!m_avCodecContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate video codec context";
        return false;
    }

    Q_ASSERT(!size.isEmpty());
    // Important: libx264 rejects streams with sizes that are not even. So to
    // ensure we don't get errors, we need to ensure the size we set here is
    // even. We also insert a pad filter into the filter chain above to ensure
    // we don't end up padding with garbage.
    m_avCodecContext->width = std::ceil(size.width() / 2.0) * 2;
    m_avCodecContext->height = std::ceil(size.height() / 2.0) * 2;
    // With explicit settings, B-frames follow the setting or else the preset (-1);
    // without them, keep KPipeWire's original behavior.
    m_avCodecContext->max_b_frames = m_settings.bFrames.value_or(hasExplicitSettings() ? -1 : 0);
    m_avCodecContext->gop_size = 100;
    m_avCodecContext->pix_fmt = fullColor ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P;
    m_avCodecContext->time_base = AVRational{1, 1000};
    m_avCodecContext->color_range = m_colorRange == PipeWireBaseEncodedStream::ColorRange::Full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    const Fraction framerate = m_produce->maxFramerate();
    if (framerate.numerator > 0 && framerate.denominator > 0) {
        m_avCodecContext->framerate = AVRational{int(framerate.numerator), int(framerate.denominator)};
        if (m_settings.keyframeIntervalSeconds) {
            const qreal fps = qreal(framerate.numerator) / framerate.denominator;
            m_avCodecContext->gop_size = std::max(1, int(std::lround(*m_settings.keyframeIntervalSeconds * fps)));
        }
    }

    switch (fullColor ? H264Profile::High : (hasExplicitSettings() && m_profile == H264Profile::Main ? H264Profile::High : m_profile)) {
    case H264Profile::Baseline:
        m_avCodecContext->profile = AV_PROFILE_H264_BASELINE;
        break;
    case H264Profile::Main:
        m_avCodecContext->profile = AV_PROFILE_H264_MAIN;
        break;
    case H264Profile::High:
        m_avCodecContext->profile = fullColor ? AV_PROFILE_H264_HIGH_444_PREDICTIVE : AV_PROFILE_H264_HIGH;
        break;
    }

    setQuality(m_quality);
    AVDictionary *options = buildEncodingOptions();
    maybeLogOptions(options);

    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        return false;
    }

    return true;
}

void LibX264Encoder::setQuality(std::optional<quint8> quality)
{
    SoftwareEncoder::setQuality(quality);
    if (!m_avCodecContext) {
        return;
    }
    // libx264 ignores the AVCodecContext global_quality / qscale fields and
    // requires CRF to be passed as a private option for constant-quality mode.
    // Recommended values range is 17 - 28.
    const qreal crf = m_settings.qualityLevel ? *m_settings.qualityLevel : m_quality ? std::lerp(28, 17, quality.value() / 100.0) : 23;
    // libx264 crf takes a float
    av_opt_set_double(m_avCodecContext, "crf", crf, AV_OPT_SEARCH_CHILDREN);
}

bool LibX264Encoder::hasExplicitSettings() const
{
    return m_settings.qualityLevel || !m_settings.speedPreset.isEmpty() || m_settings.keyframeIntervalSeconds || m_settings.bFrames
        || m_settings.fullColor || !m_settings.extraOptions.isEmpty();
}

AVDictionary *LibX264Encoder::buildEncodingOptions()
{
    AVDictionary *options = SoftwareEncoder::buildEncodingOptions();

    switch (m_encodingPreference) {
    case PipeWireBaseEncodedStream::EncodingPreference::NoPreference:
        av_dict_set(&options, "preset", "veryfast", 0);
        break;
    case PipeWireBaseEncodedStream::EncodingPreference::Quality:
        av_dict_set(&options, "preset", "medium", 0);
        break;
    case PipeWireBaseEncodedStream::EncodingPreference::Speed:
        av_dict_set(&options, "preset", "ultrafast", 0);
        av_dict_set(&options, "tune", "zerolatency", 0);
        break;
    case PipeWireBaseEncodedStream::EncodingPreference::Size:
        av_dict_set(&options, "preset", "slow", 0);
        break;
    default: //  Same as NoPreference
        av_dict_set(&options, "preset", "veryfast", 0);
        break;
    }

    if (!m_settings.speedPreset.isEmpty()) {
        av_dict_set(&options, "preset", m_settings.speedPreset.toUtf8().constData(), 0);
    }
    if (!m_settings.extraOptions.isEmpty()) {
        av_dict_set(&options, "x264-params", m_settings.extraOptions.toUtf8().constData(), 0);
    }

    // Ectolens: the in-loop deblocking filter stays on. Upstream switched it off
    // for speed, which is a large part of why screen recordings looked blocky.
    av_dict_set(&options, "flags", "+mv4", 0);

    return options;
}
