#pragma once

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtEndian>
#include <functional>

#include "util/types.h"

namespace crate::test {

/// Writes a 16-bit PCM stereo WAV of `frames` frames; `sample(frame,
/// channel)` is in [-32768, 32767].
inline void writeWav(const QString& path,
        int sampleRate,
        SINT frames,
        const std::function<int(SINT frame, int channel)>& sample) {
    QByteArray data;
    data.reserve(static_cast<qsizetype>(4 * frames));
    for (SINT frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < 2; ++channel) {
            char bytes[2];
            qToLittleEndian(static_cast<qint16>(sample(frame, channel)), bytes);
            data.append(bytes, 2);
        }
    }
    QByteArray header;
    const auto put32 = [&header](quint32 value) {
        char bytes[4];
        qToLittleEndian(value, bytes);
        header.append(bytes, 4);
    };
    const auto put16 = [&header](quint16 value) {
        char bytes[2];
        qToLittleEndian(value, bytes);
        header.append(bytes, 2);
    };
    header.append("RIFF");
    put32(static_cast<quint32>(36 + data.size()));
    header.append("WAVEfmt ");
    put32(16);
    put16(1);
    put16(2);
    put32(static_cast<quint32>(sampleRate));
    put32(static_cast<quint32>(sampleRate * 4));
    put16(4);
    put16(16);
    header.append("data");
    put32(static_cast<quint32>(data.size()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(header);
    file.write(data);
}

} // namespace crate::test
