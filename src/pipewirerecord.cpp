/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewirerecord.h"
#include "aacencoder_p.h"
#include "audioconstants_p.h"
#include "encoder_p.h"
#include "glhelpers.h"
#include "libopusencoder_p.h"
#include "pipewirerecord_p.h"
#include <logging_record.h>

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <KShell>

#include <unistd.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
}

#undef av_err2str

#ifdef av_ts2str
#undef av_ts2str
char buf[AV_TS_MAX_STRING_SIZE];
#define av_ts2str(ts) av_ts_make_string(buf, ts)
#endif // av_ts2str

#ifdef av_ts2timestr
#undef av_ts2timestr
char timebuf[AV_TS_MAX_STRING_SIZE];
#define av_ts2timestr(ts, tb) av_ts_make_time_string(timebuf, ts, tb)
#endif // av_ts2timestr

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    qCDebug(PIPEWIRERECORD_LOGGING,
            "pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s "
            "stream_index:%d",
            av_ts2str(pkt->pts),
            av_ts2timestr(pkt->pts, time_base),
            av_ts2str(pkt->dts),
            av_ts2timestr(pkt->dts, time_base),
            av_ts2str(pkt->duration),
            av_ts2timestr(pkt->duration, time_base),
            pkt->stream_index);
}

PipeWireRecord::PipeWireRecord(QObject *parent)
    : PipeWireBaseEncodedStream(parent)
    , d(new PipeWireRecordPrivate)
{
}

PipeWireRecord::~PipeWireRecord() = default;

void PipeWireRecord::setOutput(const QString &_output)
{
    const QString output = KShell::tildeExpand(_output);

    if (d->m_output == output)
        return;

    d->m_output = output;
    Q_EMIT outputChanged(output);
}

QString PipeWireRecord::output() const
{
    return d->m_output;
}

bool PipeWireRecord::recordSystemAudio() const
{
    return d->m_recordSystemAudio;
}

void PipeWireRecord::setRecordSystemAudio(bool recordSystemAudio)
{
    if (d->m_recordSystemAudio == recordSystemAudio) {
        return;
    }

    d->m_recordSystemAudio = recordSystemAudio;
    Q_EMIT recordSystemAudioChanged(recordSystemAudio);
}

bool PipeWireRecord::separateAudioTracks() const
{
    return d->m_separateAudioTracks;
}

void PipeWireRecord::setSeparateAudioTracks(bool separate)
{
    d->m_separateAudioTracks = separate;
}

bool PipeWireRecord::recordMicrophone() const
{
    return d->m_recordMicrophone;
}

void PipeWireRecord::setRecordMicrophone(bool recordMicrophone)
{
    if (d->m_recordMicrophone == recordMicrophone) {
        return;
    }

    d->m_recordMicrophone = recordMicrophone;
    Q_EMIT recordMicrophoneChanged(recordMicrophone);
}

QString PipeWireRecord::extension() const
{
    static QHash<PipeWireBaseEncodedStream::Encoder, QString> s_extensions = {
        {PipeWireBaseEncodedStream::H264Main, QStringLiteral("mp4")},
        {PipeWireBaseEncodedStream::H264Baseline, QStringLiteral("mp4")},
        {PipeWireBaseEncodedStream::VP8, QStringLiteral("webm")},
        {PipeWireBaseEncodedStream::VP9, QStringLiteral("webm")},
        {PipeWireBaseEncodedStream::WebP, QStringLiteral("webp")},
        {PipeWireBaseEncodedStream::Gif, QStringLiteral("gif")},
    };
    return s_extensions.value(encoder());
}

PipeWireRecordProduce::PipeWireRecordProduce(PipeWireBaseEncodedStream::Encoder encoder,
                                             uint nodeId,
                                             quint64 objectSerial,
                                             uint fd,
                                             const Fraction &framerate,
                                             const QString &output,
                                             AudioSources audioSources,
                                             bool separateAudioTracks)
    : PipeWireProduce(encoder, nodeId, objectSerial, fd, framerate)
    , m_output(output)
{
    m_audioSources = audioSources;
    m_separateAudioTracks = separateAudioTracks;
}

bool PipeWireRecordProduce::setupFormat()
{
    avformat_alloc_output_context2(&m_avFormatContext, nullptr, nullptr, m_output.toUtf8().constData());
    if (!m_avFormatContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not deduce output format from file: using WebM." << m_output;
        avformat_alloc_output_context2(&m_avFormatContext, nullptr, "webm", m_output.toUtf8().constData());
    }
    if (!m_avFormatContext) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "could not set stream up";
        return false;
    }

    const Fraction framerate = m_stream->framerate();
    int ret = avio_open(&m_avFormatContext->pb, QFile::encodeName(m_output).constData(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open" << m_output << av_err2str(ret);
        return false;
    }

    auto avStream = avformat_new_stream(m_avFormatContext, nullptr);
    avStream->start_time = 0;
    if (framerate) {
        avStream->r_frame_rate.num = framerate.numerator;
        avStream->r_frame_rate.den = framerate.denominator;
        avStream->avg_frame_rate.num = framerate.numerator;
        avStream->avg_frame_rate.den = framerate.denominator;
    }

    ret = avcodec_parameters_from_context(avStream->codecpar, m_encoder->avCodecContext());
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Error occurred when passing the codec:" << av_err2str(ret);
        return false;
    }

    if (m_audioSources) {
        if (!m_encoder->supportsAudio() || m_avFormatContext->oformat->audio_codec == AV_CODEC_ID_NONE) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Audio recording is not supported for this format, ignoring";
        } else {
            // Ectolens: the first track mixes every source, as upstream's only track did, so
            // every player plays both. Separate tracks add each source alone after it (system
            // sound, then the microphone), for editing.
            struct TrackPlan {
                const char *title;
                std::vector<int> sources;
            };
            const int inputCount = m_audioSources.testFlag(AudioSource::SystemAudio) + m_audioSources.testFlag(AudioSource::Microphone);
            std::vector<TrackPlan> plans{{nullptr, {}}};
            for (int i = 0; i < inputCount; ++i) {
                plans[0].sources.push_back(i);
            }
            if (m_separateAudioTracks && inputCount == 2) {
                plans[0].title = "Mix";
                plans.push_back({"System sound", {0}});
                plans.push_back({"Microphone", {1}});
            }
            for (const auto &plan : plans) {
                std::unique_ptr<AudioEncoder> audioEncoder;
                if (m_encoderType == PipeWireBaseEncodedStream::VP8 || m_encoderType == PipeWireBaseEncodedStream::VP9) {
                    audioEncoder = std::make_unique<LibOpusEncoder>(this);
                } else {
                    audioEncoder = std::make_unique<AacEncoder>(this);
                }
                audioEncoder->setQuality(m_quality);
                if (!audioEncoder->initialize(int(plan.sources.size()), m_avFormatContext->oformat->flags & AVFMT_GLOBALHEADER)) {
                    qCWarning(PIPEWIRERECORD_LOGGING) << "Could not initialize the audio encoder, recording without" << (plan.title ? plan.title : "audio");
                    continue;
                }
                // Copy the codec parameters into a temporary first: once a
                // stream has been added to the muxer it cannot be removed, and
                // a half-configured stream makes avformat_write_header() fail.
                AVCodecParameters *audioParameters = avcodec_parameters_alloc();
                if (!audioParameters) {
                    qFatal("Failed to allocate memory");
                }
                ret = avcodec_parameters_from_context(audioParameters, audioEncoder->avCodecContext());
                if (ret < 0) {
                    qCWarning(PIPEWIRERECORD_LOGGING) << "Error occurred when passing the audio codec, recording without audio:" << av_err2str(ret);
                } else if (AVStream *audioStream = avformat_new_stream(m_avFormatContext, nullptr); !audioStream) {
                    qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create an audio stream, recording without audio";
                } else if (ret = avcodec_parameters_copy(audioStream->codecpar, audioParameters); ret < 0) {
                    qCWarning(PIPEWIRERECORD_LOGGING) << "Error occurred when copying the audio codec parameters, recording without audio:"
                                                      << av_err2str(ret);
                } else {
                    audioStream->time_base = AVRational{1, AudioSampleRate};
                    if (plan.title) {
                        // the track's name: MP4 keeps it as the handler name, Matroska as the title
                        av_dict_set(&audioStream->metadata, "title", plan.title, 0);
                        av_dict_set(&audioStream->metadata, "handler_name", plan.title, 0);
                    }
                    audioStream->disposition = m_audioTracks.empty() ? AV_DISPOSITION_DEFAULT : 0;
                    // A static screen produces no video packets while audio keeps
                    // flowing, don't make the muxer wait for video to interleave.
                    m_avFormatContext->max_interleave_delta = 1000000;
                    audioEncoder->setTrack(int(m_audioTracks.size()));
                    m_audioTracks.push_back({std::move(audioEncoder), plan.sources});
                    m_audioTrackStreams.push_back(audioStream);
                }
                avcodec_parameters_free(&audioParameters);
            }
        }
    }

    AVDictionary *options = nullptr;
    const auto codecId = m_avFormatContext->oformat->video_codec;
    if (codecId == AV_CODEC_ID_GIF || codecId == AV_CODEC_ID_WEBP) {
        av_dict_set_int(&options, "loop", 0, 0);
    }
    // Ectolens: MP4 and MOV are written in fragments while recording, so a crash
    // still leaves a playable file, and FFmpeg turns them into a normal file when
    // the recording finishes (the method OBS calls Hybrid MP4). FFmpeg added that
    // in 7.1; an older one (Ubuntu 24.04's 6.1, under KDE neon) refuses the flag and
    // nothing gets recorded, so there the file is fragmented the classic way, and stays
    // fragmented when the recording ends. Its index waits for the first fragment
    // (delay_moov, not empty_moov): the encoders send H.264's parameter sets in the
    // stream, and an index written before the first frame had none, so nothing could
    // play the file. A crash in the first fragment (one keyframe interval) loses it.
    const QByteArray formatName = m_avFormatContext->oformat->name;
    if (formatName.contains("mp4") || formatName.contains("mov")) {
        const AVClass *muxerClass = m_avFormatContext->oformat->priv_class;
        const bool hybrid = muxerClass && av_opt_find(&muxerClass, "hybrid_fragmented", "movflags", 0, AV_OPT_SEARCH_FAKE_OBJ);
        av_dict_set(&options, "movflags", hybrid ? "+hybrid_fragmented" : "+frag_keyframe+delay_moov+default_base_moof", 0);
    }
    ret = avformat_write_header(m_avFormatContext, &options);
    av_dict_free(&options);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Error occurred when writing header:" << av_err2str(ret);
        return false;
    }
    m_headerWritten = true;

    return true;
}

void PipeWireRecordProduce::processFrame(const PipeWireFrame &frame)
{
    PipeWireProduce::processFrame(frame);
    if (frame.cursor && !frame.dmabuf && !frame.dataFrame && m_frameWithoutMetadataCursor.dataFrame) {
        m_encoder->filterFrame(m_frameWithoutMetadataCursor);
    }
}

void PipeWireRecordProduce::aboutToEncode(PipeWireFrame &frame)
{
    if (!frame.dataFrame) {
        return;
    }

    if (m_cursor.position && !m_cursor.texture.isNull()) {
        auto image = frame.dataFrame->toImage();
        // Do not copy the image if it's already ours
        if (m_frameWithoutMetadataCursor.dataFrame->cleanup != frame.dataFrame->cleanup) {
            m_frameWithoutMetadataCursor.dataFrame = frame.dataFrame->copy();
        }
        QPainter p(&image);
        p.drawImage(*m_cursor.position, m_cursor.texture);
    }
}

void PipeWireRecordProduce::processPacket(AVPacket *packet)
{
    packet->stream_index = (*m_avFormatContext->streams)->index;
    av_packet_rescale_ts(packet, m_encoder->avCodecContext()->time_base, (*m_avFormatContext->streams)->time_base);
    log_packet(m_avFormatContext, packet);
    auto ret = av_interleaved_write_frame(m_avFormatContext, packet);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Error while writing output packet:" << av_err2str(ret);
    }
}

void PipeWireRecordProduce::processAudioPacket(AVPacket *packet, int track)
{
    AVStream *stream = m_audioTrackStreams.at(track);
    packet->stream_index = stream->index;
    av_packet_rescale_ts(packet, m_audioTracks.at(track).encoder->avCodecContext()->time_base, stream->time_base);
    log_packet(m_avFormatContext, packet);
    auto ret = av_interleaved_write_frame(m_avFormatContext, packet);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Error while writing audio packet:" << av_err2str(ret);
    }
}

std::unique_ptr<PipeWireProduce> PipeWireRecord::makeProduce()
{
    AudioSources audioSources;
    audioSources.setFlag(AudioSource::SystemAudio, d->m_recordSystemAudio);
    audioSources.setFlag(AudioSource::Microphone, d->m_recordMicrophone);
    return std::make_unique<PipeWireRecordProduce>(encoder(), nodeId(), objectSerial(), fd(), maxFramerate(), d->m_output, audioSources, d->m_separateAudioTracks);
}

int64_t PipeWireRecordProduce::framePts(const std::optional<std::chrono::nanoseconds> &presentationTimestamp)
{
    if (!presentationTimestamp) {
        // No timestamp to place this frame on the timeline; treat it as the
        // very start rather than dereferencing an empty optional.
        return 0;
    }
    // A frame rendered before the recording started (the screencast stream
    // delivers the current screen content as its first frame, timestamped
    // with when it was originally rendered) belongs at the very start.
    const auto pts =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::time_point(*presentationTimestamp) - recordEpoch()).count();
    return std::max<int64_t>(pts, 0);
}

void PipeWireRecordProduce::cleanup()
{
    if (m_avFormatContext) {
        // Ectolens: a setup that failed before the header (the file couldn't be opened,
        // or the muxer refused an option) left no header, and writing a trailer then
        // crashed, as in upstream KPipeWire. The unusable file goes too.
        if (m_headerWritten) {
            if (auto result = av_write_trailer(m_avFormatContext); result < 0) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Could not write trailer";
            }
        }

        const bool opened = m_avFormatContext->pb;
        avio_closep(&m_avFormatContext->pb);
        avformat_free_context(m_avFormatContext);
        m_avFormatContext = nullptr;
        if (!m_headerWritten && opened) {
            QFile::remove(m_output);
        }
    }
}

#include "moc_pipewirerecord.cpp"

#include "moc_pipewirerecord_p.cpp"
