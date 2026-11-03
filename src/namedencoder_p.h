/*
    SPDX-FileCopyrightText: 2026 Ectolens contributors
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#pragma once

#include "encoder_p.h"

/**
 * Ectolens: an encoder chosen by its FFmpeg name (libx265, libsvtav1, h264_nvenc,
 * hevc_nvenc, av1_nvenc), fed frames from system memory. EncoderSettings values
 * are translated into each encoder family's own options.
 */
class NamedEncoder : public SoftwareEncoder
{
public:
    NamedEncoder(const QString &ffmpegName, PipeWireProduce *produce);

    bool initialize(const QSize &size) override;

    /// The encoders Ectolens knows how to drive, in order of preference within each kind.
    static QStringList knownEncoders();
    /// Whether full color detail (4:4:4) is possible with this encoder.
    static bool supportsFullColor(const QString &ffmpegName);

protected:
    AVDictionary *buildEncodingOptions() override;

private:
    enum class Family { X265, SvtAv1, Nvenc, Other };
    Family family() const;
    QString m_name;
};
