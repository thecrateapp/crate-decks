// The lossless stem sidecar of STEM-D9 (plan ST5.PR2): an 8-channel FLAC
// whose four stems add up to Decks' decode of the original.

#include "crate/stems/stemsidecar.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>
#include <functional>
#include <numbers>
#include <vector>

#include "sources/soundsourceproxy.h"
#include "test/crate/wavwriter.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"

namespace {

using crate::StemSidecar;
using crate::StemSidecarWriter;
using crate::test::writeWav;

/// Music-like enough: a few partials and a pseudo-random part.
int musicSample(SINT frame, int channel, int sampleRate) {
    const double t = static_cast<double>(frame) / sampleRate;
    const double tone = 0.3 * std::sin(2.0 * std::numbers::pi * 110.0 * t) +
            0.2 * std::sin(2.0 * std::numbers::pi * (440.0 + 3.0 * channel) * t);
    const quint32 noise = static_cast<quint32>(frame) * 2654435761u ^
            static_cast<quint32>(channel) * 40503u;
    return static_cast<int>(std::lround(tone * 32767.0)) +
            static_cast<int>(noise % 4001u) - 2000;
}

/// The stems a separator would write for `frames` frames at 44.1 kHz:
/// three tones at whatever level, `other` arbitrary (the writer ignores it).
std::vector<float> fakeStems(SINT first, SINT frames) {
    std::vector<float> stems(static_cast<std::size_t>(8 * frames));
    for (int row = 0; row < 8; ++row) {
        for (SINT i = 0; i < frames; ++i) {
            const double t = static_cast<double>(first + i) / 44100.0;
            stems[static_cast<std::size_t>(row * frames + i)] =
                    static_cast<float>(0.25 *
                            std::sin(2.0 * std::numbers::pi * 60.0 * (row + 1) *
                                    t));
        }
    }
    return stems;
}

class StemSidecarTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
    }

    /// Writes the sidecar of `original` from fake stems, in uneven blocks.
    void writeSidecar(const QString& original) {
        const TrackPointer pTrack = Track::newTemporary(original);
        StemSidecarWriter::Options options;
        options.pTrack = pTrack;
        options.sourceRevision = QString(64, QLatin1Char('a'));
        StemSidecarWriter writer(options);
        // The stems' frames at 44.1 kHz, as the separator gives them
        mixxx::AudioSource::OpenParams params;
        const auto pSource = SoundSourceProxy(pTrack).openAudioSource(params);
        ASSERT_NE(nullptr, pSource);
        crate::StemSeparationSource source;
        source.originalSampleRate = static_cast<int>(pSource->getSignalInfo().getSampleRate());
        source.originalFrames = pSource->frameIndexRange().length();
        source.frames = source.originalFrames * 44100 / source.originalSampleRate;
        pSource->close();
        QString error;
        ASSERT_TRUE(writer.begin(source, &error)) << error.toStdString();
        for (SINT first = 0; first < source.frames;) {
            const SINT count = std::min<SINT>(first % 3 == 0 ? 3001 : 1777, source.frames - first);
            ASSERT_TRUE(writer.write(first, count, fakeStems(first, count), &error))
                    << error.toStdString();
            first += count;
        }
        ASSERT_TRUE(writer.finish(&error)) << error.toStdString();
        EXPECT_EQ(0, writer.clippedSamples());
        EXPECT_FALSE(QFile::exists(StemSidecar::pathFor(original) + QStringLiteral(".part")));
    }

    /// Decks' stereo decode of a file.
    std::vector<float> decodeOriginal(const QString& path) {
        mixxx::AudioSource::OpenParams params;
        params.setChannelCount(mixxx::audio::ChannelCount::stereo());
        const auto pSource = SoundSourceProxy(Track::newTemporary(path)).openAudioSource(params);
        EXPECT_NE(nullptr, pSource);
        if (!pSource) {
            return {};
        }
        const mixxx::IndexRange range = pSource->frameIndexRange();
        mixxx::SampleBuffer buffer(2 * range.length());
        const auto readable = pSource->readSampleFrames(mixxx::WritableSampleFrames(
                range, mixxx::SampleBuffer::WritableSlice(buffer)));
        std::vector<float> samples(readable.readableData(),
                readable.readableData() + readable.readableLength());
        pSource->close();
        return samples;
    }

    /// The sidecar decodes to stems that add up to the original's decode.
    void expectStemsAddUp(const QString& original) {
        std::vector<float> stems;
        SINT frames = 0;
        QString error;
        ASSERT_TRUE(StemSidecar::decode(StemSidecar::pathFor(original), &stems, &frames, &error))
                << error.toStdString();
        const std::vector<float> decoded = decodeOriginal(original);
        ASSERT_EQ(static_cast<SINT>(decoded.size()) / 2, frames);
        // As integers, so that adding floats does not round
        const double fullScale = 8388608.0 * StemSidecar::kScale;
        bool stemsSoundAtAll = false;
        for (SINT frame = 0; frame < frames; ++frame) {
            for (int channel = 0; channel < 2; ++channel) {
                const auto at = [&](int stem) {
                    return std::llround(
                            stems[static_cast<std::size_t>(8 * frame + 2 * stem + channel)] *
                            fullScale);
                };
                const long long sum = at(0) + at(1) + at(2) + at(3);
                stemsSoundAtAll = stemsSoundAtAll || at(0) != 0;
                ASSERT_EQ(static_cast<double>(decoded[static_cast<std::size_t>(
                                  2 * frame + channel)]),
                        static_cast<double>(sum) / fullScale)
                        << "frame " << frame << " channel " << channel;
            }
        }
        EXPECT_TRUE(stemsSoundAtAll);
    }

    QTemporaryDir m_dir;
};

} // namespace

TEST_F(StemSidecarTest, StemsAddUpToTheOriginalExactly) {
    const QString original = m_dir.filePath(QStringLiteral("track.wav"));
    writeWav(original, 44100, 3 * 44100 + 123, [](SINT frame, int channel) {
        return musicSample(frame, channel, 44100);
    });
    writeSidecar(original);
    expectStemsAddUp(original);
    QString error;
    const auto info = StemSidecar::readInfo(StemSidecar::pathFor(original), &error);
    ASSERT_TRUE(info) << error.toStdString();
    EXPECT_EQ(44100, info->sampleRate);
    EXPECT_EQ(3 * 44100 + 123, info->frames);
    EXPECT_EQ(QStringLiteral("htdemucs-onnx-1"), info->separator);
    EXPECT_EQ(QString(64, QLatin1Char('a')), info->sourceRevision);
    EXPECT_TRUE(StemSidecar::isValidFor(Track::newTemporary(original), &error))
            << error.toStdString();
}

TEST_F(StemSidecarTest, A48kHzOriginalGetsA48kHzSidecar) {
    const QString original = m_dir.filePath(QStringLiteral("track48.wav"));
    writeWav(original, 48000, 2 * 48000 + 7, [](SINT frame, int channel) {
        return musicSample(frame, channel, 48000);
    });
    writeSidecar(original);
    QString error;
    const auto info = StemSidecar::readInfo(StemSidecar::pathFor(original), &error);
    ASSERT_TRUE(info) << error.toStdString();
    EXPECT_EQ(48000, info->sampleRate);
    EXPECT_EQ(2 * 48000 + 7, info->frames);
    expectStemsAddUp(original);
    EXPECT_TRUE(StemSidecar::isValidFor(Track::newTemporary(original), &error))
            << error.toStdString();
}

TEST_F(StemSidecarTest, ASidecarWithAnotherFingerprintIsNotValid) {
    const QString original = m_dir.filePath(QStringLiteral("track.wav"));
    const SINT frames = 3 * 44100;
    writeWav(original, 44100, frames, [](SINT frame, int channel) {
        return musicSample(frame, channel, 44100);
    });
    writeSidecar(original);
    QString reason;
    ASSERT_TRUE(StemSidecar::isValidFor(Track::newTemporary(original), &reason))
            << reason.toStdString();

    // The same audio one sample later, as another decoder delay would give
    writeWav(original, 44100, frames, [](SINT frame, int channel) {
        return frame == 0 ? 0 : musicSample(frame - 1, channel, 44100);
    });
    EXPECT_FALSE(StemSidecar::isValidFor(Track::newTemporary(original), &reason));
    EXPECT_FALSE(reason.isEmpty());

    // Another length, or another rate
    writeWav(original, 44100, frames + 1, [](SINT frame, int channel) {
        return musicSample(frame, channel, 44100);
    });
    EXPECT_FALSE(StemSidecar::isValidFor(Track::newTemporary(original), &reason));
    writeWav(original, 48000, frames, [](SINT frame, int channel) {
        return musicSample(frame, channel, 44100);
    });
    EXPECT_FALSE(StemSidecar::isValidFor(Track::newTemporary(original), &reason));
    // No sidecar at all
    EXPECT_FALSE(StemSidecar::isValidFor(
            Track::newTemporary(m_dir.filePath(QStringLiteral("other.wav"))), &reason));
}

// A writer that does not finish, as a cancelled separation, leaves nothing.
TEST_F(StemSidecarTest, AnUnfinishedSidecarLeavesNothing) {
    const QString original = m_dir.filePath(QStringLiteral("track.wav"));
    writeWav(original, 44100, 44100, [](SINT frame, int channel) {
        return musicSample(frame, channel, 44100);
    });
    {
        StemSidecarWriter::Options options;
        options.pTrack = Track::newTemporary(original);
        StemSidecarWriter writer(options);
        crate::StemSeparationSource source;
        source.originalSampleRate = 44100;
        QString error;
        ASSERT_TRUE(writer.begin(source, &error)) << error.toStdString();
        ASSERT_TRUE(writer.write(0, 1000, fakeStems(0, 1000), &error));
        EXPECT_TRUE(QFile::exists(StemSidecar::pathFor(original) + QStringLiteral(".part")));
    }
    EXPECT_FALSE(QFile::exists(StemSidecar::pathFor(original) + QStringLiteral(".part")));
    EXPECT_FALSE(QFile::exists(StemSidecar::pathFor(original)));
}
