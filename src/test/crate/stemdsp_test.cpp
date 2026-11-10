#include "crate/stems/stemdsp.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include "crate/stems/resampler.h"
#include "test/mixxxtest.h"

namespace {

using namespace crate::stemdsp;
using crate::Resampler;

/// The DSP fixtures of tools/crate-stems/make_fixtures.py: raw little-endian
/// float32, described in index.json.
class StemDspTest : public MixxxTest {
  protected:
    QString fixturePath(const QString& fileName) const {
        return getTestDir().filePath(QStringLiteral("crate/data/htdemucs-dsp/") + fileName);
    }

    std::vector<float> readFixture(const QString& key) const {
        const QJsonObject file =
                index().value(QStringLiteral("files")).toObject().value(key).toObject();
        QFile data(fixturePath(file.value(QStringLiteral("file")).toString()));
        EXPECT_TRUE(data.open(QIODevice::ReadOnly)) << key.toStdString();
        const QByteArray bytes = data.readAll();
        EXPECT_EQ(file.value(QStringLiteral("bytes")).toInteger(), bytes.size());
        std::vector<float> values(bytes.size() / sizeof(float));
        for (size_t i = 0; i < values.size(); ++i) {
            const quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + i * sizeof(float));
            std::memcpy(&values[i], &bits, sizeof(float));
        }
        return values;
    }

    QJsonObject index() const {
        QFile file(fixturePath(QStringLiteral("index.json")));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    static void expectNear(const std::vector<float>& expected,
            std::span<const float> actual,
            double tolerance,
            const char* what) {
        ASSERT_EQ(expected.size(), actual.size()) << what;
        double maxDifference = 0;
        size_t worst = 0;
        for (size_t i = 0; i < expected.size(); ++i) {
            const double difference = std::abs(double(expected[i]) - actual[i]);
            if (difference > maxDifference) {
                maxDifference = difference;
                worst = i;
            }
        }
        EXPECT_LE(maxDifference, tolerance)
                << what << ": at " << worst << " expected " << expected[worst]
                << ", got " << actual[worst];
    }
};

TEST_F(StemDspTest, StftMatchesTheReferenceFixture) {
    const std::vector<float> input = readFixture(QStringLiteral("stft_input"));
    const std::vector<float> expected = readFixture(QStringLiteral("stft_cac"));
    constexpr SINT kLength = 10000;
    ASSERT_EQ(size_t(2 * kLength), input.size());
    ASSERT_EQ(10, spectrumFrames(kLength));
    std::vector<float> spectrum(4 * kBins * spectrumFrames(kLength));

    Stft stft;
    stft.forward(input, 2, kLength, spectrum);

    expectNear(expected, spectrum, 1e-5, "stft_cac");
}

TEST_F(StemDspTest, IstftMatchesTheReferenceFixture) {
    Stft stft;
    {
        const std::vector<float> spectrum = readFixture(QStringLiteral("stft_cac"));
        const std::vector<float> expected = readFixture(QStringLiteral("istft_roundtrip"));
        std::vector<float> output(expected.size());
        stft.inverse(spectrum, 2, 10, 10000, output);
        expectNear(expected, output, 1e-5, "istft_roundtrip");
    }
    {
        const std::vector<float> spectrum = readFixture(QStringLiteral("istft_random_cac"));
        const std::vector<float> expected = readFixture(QStringLiteral("istft_random_output"));
        std::vector<float> output(expected.size());
        stft.inverse(spectrum, 2, 4, 4000, output);
        expectNear(expected, output, 1e-5, "istft_random_output");
    }
}

TEST_F(StemDspTest, StftOfASegmentHasTheShapeOfTheGraphInput) {
    EXPECT_EQ(336, kSegmentSpectrumFrames);
    std::vector<float> segment(2 * kSegmentFrames);
    for (SINT i = 0; i < kSegmentFrames; ++i) {
        segment[i] = 0.5f * std::sin(0.01f * i);
        segment[kSegmentFrames + i] = 0.25f * std::cos(0.003f * i);
    }
    std::vector<float> spectrum(4 * kBins * kSegmentSpectrumFrames);
    std::vector<float> output(segment.size());
    Stft stft;

    stft.forward(segment, 2, kSegmentFrames, spectrum);
    stft.inverse(spectrum, 2, kSegmentSpectrumFrames, kSegmentFrames, output);

    // The round trip is exact away from the edges, whose frames are dropped.
    double maxDifference = 0;
    for (int channel = 0; channel < 2; ++channel) {
        for (SINT i = kFftSize; i < kSegmentFrames - kFftSize; ++i) {
            const SINT j = channel * kSegmentFrames + i;
            maxDifference = std::max(maxDifference, double(std::abs(segment[j] - output[j])));
        }
    }
    EXPECT_LT(maxDifference, 1e-6);
}

TEST_F(StemDspTest, NormalisationMatchesTheReferenceFixture) {
    const std::vector<float> input = readFixture(QStringLiteral("ola_input"));
    const QJsonObject expected = index().value(QStringLiteral("normalisation")).toObject();

    const Normalisation normalised = normalisation(input, 2, SINT(input.size() / 2));

    EXPECT_NEAR(expected.value(QStringLiteral("mean")).toDouble(), normalised.mean, 1e-9);
    EXPECT_NEAR(expected.value(QStringLiteral("scale")).toDouble(), normalised.scale, 1e-9);
}

TEST_F(StemDspTest, OverlapAddMatchesTheReferenceFixture) {
    const std::vector<float> input = readFixture(QStringLiteral("ola_input"));
    const std::vector<float> expected = readFixture(QStringLiteral("ola_output"));
    const QJsonObject ola = index().value(QStringLiteral("overlap_add")).toObject();
    OverlapAddShape shape;
    shape.channels = 2;
    shape.sources = 2;
    shape.segment = ola.value(QStringLiteral("segment")).toInteger();
    shape.stride = ola.value(QStringLiteral("stride")).toInteger();
    const SINT length = ola.value(QStringLiteral("length")).toInteger();
    ASSERT_EQ(size_t(2 * length), input.size());
    expectNear(readFixture(QStringLiteral("ola_weight")),
            overlapAddWeight(shape.segment),
            1e-7,
            "ola_weight");
    // The fixture's mock model: stem 0 = segment × ramp, stem 1 = segment ×
    // (1.5 - ramp), ramp[n] = 0.5 + n / segment.
    const auto model = [&](std::span<const float> segment, std::span<float> output) {
        for (int channel = 0; channel < 2; ++channel) {
            for (SINT n = 0; n < shape.segment; ++n) {
                const float ramp = 0.5f + float(n) / float(shape.segment);
                const float value = segment[channel * shape.segment + n];
                output[channel * shape.segment + n] = value * ramp;
                output[(2 + channel) * shape.segment + n] = value * (1.5f - ramp);
            }
        }
        return true;
    };
    std::vector<float> output(expected.size());
    SINT nextFrame = 0;
    const auto collect = [&](SINT firstFrame, SINT frameCount, std::span<float> chunk) {
        EXPECT_EQ(nextFrame, firstFrame);
        nextFrame = firstFrame + frameCount;
        for (int row = 0; row < 4; ++row) {
            std::copy_n(chunk.begin() + row * frameCount,
                    frameCount,
                    output.begin() + row * length + firstFrame);
        }
        return true;
    };

    ASSERT_TRUE(overlapAdd(input, length, shape, model, collect));

    EXPECT_EQ(length, nextFrame);
    expectNear(expected, output, 1e-5, "ola_output");
}

TEST_F(StemDspTest, OverlapAddWeightOfARealSegmentMatchesTheReference) {
    const QJsonObject values = index()
                                       .value(QStringLiteral("overlap_add"))
                                       .toObject()
                                       .value(QStringLiteral("real_weight"))
                                       .toObject()
                                       .value(QStringLiteral("values"))
                                       .toObject();
    const std::vector<float> weight = overlapAddWeight(kSegmentFrames);
    ASSERT_EQ(size_t(kSegmentFrames), weight.size());
    for (auto it = values.begin(); it != values.end(); ++it) {
        EXPECT_NEAR(it.value().toDouble(), weight[it.key().toInt()], 1e-9)
                << it.key().toStdString();
    }
}

TEST_F(StemDspTest, OverlapAddStopsWhenTheModelFails) {
    const std::vector<float> input(2 * 5000, 0.1f);
    OverlapAddShape shape;
    shape.sources = 1;
    shape.segment = 2000;
    shape.stride = 1500;
    int runs = 0;

    EXPECT_FALSE(overlapAdd(
            input,
            5000,
            shape,
            [&](std::span<const float>, std::span<float>) { return ++runs < 2; },
            [](SINT, SINT, std::span<float>) { return true; }));
    EXPECT_EQ(2, runs);
}

// Resampler

std::vector<float> stereoSine(int rate, SINT frames, double frequency) {
    std::vector<float> samples(2 * frames);
    for (SINT i = 0; i < frames; ++i) {
        const double phase = 2 * std::numbers::pi * frequency * double(i) / rate;
        samples[2 * i] = float(0.5 * std::sin(phase));
        samples[2 * i + 1] = float(0.25 * std::cos(phase));
    }
    return samples;
}

TEST(ResamplerTest, PassesTheSameRateThrough) {
    const std::vector<float> input = stereoSine(44100, 1000, 440);
    Resampler resampler(44100, 44100, 2);
    std::vector<float> output;

    resampler.process(input, &output);
    resampler.finish(&output);

    EXPECT_EQ(input, output);
}

TEST(ResamplerTest, Converts48kHzTo44100HzWithTheFloorOfTheRatio) {
    constexpr SINT kFrames = 48000;
    const std::vector<float> input = stereoSine(48000, kFrames, 1000);
    Resampler resampler(48000, 44100, 2);
    std::vector<float> output;

    resampler.process(input, &output);
    resampler.finish(&output);

    ASSERT_EQ(size_t(2 * 44100), output.size());
    EXPECT_EQ(44100, resampler.outputFrames(kFrames));
    EXPECT_EQ(44099, resampler.outputFrames(kFrames - 1));
    // Away from the edges, a 1 kHz tone is the same tone at the new rate.
    const std::vector<float> expected = stereoSine(44100, 44100, 1000);
    double maxDifference = 0;
    for (size_t i = 2 * 100; i < output.size() - 2 * 100; ++i) {
        maxDifference = std::max(maxDifference, double(std::abs(expected[i] - output[i])));
    }
    EXPECT_LT(maxDifference, 1e-4);
}

TEST(ResamplerTest, RemovesWhatIsAboveTheNewNyquistFrequency) {
    // 23 kHz fits at 48 kHz and not at 44.1 kHz.
    const std::vector<float> input = stereoSine(48000, 48000, 23000);
    Resampler resampler(48000, 44100, 2);
    std::vector<float> output;

    resampler.process(input, &output);
    resampler.finish(&output);

    float peak = 0;
    for (size_t i = 2 * 1000; i < output.size() - 2 * 1000; ++i) {
        peak = std::max(peak, std::abs(output[i]));
    }
    // julius.resample_frac leaves 0.00083 of the input's 0.5 here (−56 dB).
    EXPECT_LT(peak, 0.001f);
}

TEST(ResamplerTest, StreamsTheSameAsOneBlock) {
    for (const auto& [inputRate, outputRate] : {std::pair{48000, 44100},
                 std::pair{22050, 44100},
                 std::pair{96000, 44100},
                 std::pair{44100, 48000},
                 std::pair{8000, 44100}}) {
        const std::vector<float> input = stereoSine(inputRate, 9001, 330);
        Resampler whole(inputRate, outputRate, 2);
        std::vector<float> expected;
        whole.process(input, &expected);
        whole.finish(&expected);

        Resampler blocks(inputRate, outputRate, 2);
        std::vector<float> output;
        SINT frame = 0;
        for (const SINT block : {SINT(1), SINT(7), SINT(500), SINT(3), SINT(4096)}) {
            blocks.process(std::span<const float>(input).subspan(2 * frame, 2 * block), &output);
            frame += block;
        }
        blocks.process(std::span<const float>(input).subspan(2 * frame), &output);
        blocks.finish(&output);

        EXPECT_EQ(size_t(2 * whole.outputFrames(9001)), expected.size())
                << inputRate << " -> " << outputRate;
        EXPECT_EQ(expected, output) << inputRate << " -> " << outputRate;
    }
}

TEST(ResamplerTest, KeepsAConstantSignal) {
    const std::vector<float> input(2 * 3000, 0.3f);
    Resampler resampler(32000, 44100, 2);
    std::vector<float> output;

    resampler.process(input, &output);
    resampler.finish(&output);

    ASSERT_EQ(size_t(2 * resampler.outputFrames(3000)), output.size());
    for (const float sample : output) {
        ASSERT_NEAR(0.3f, sample, 1e-6f);
    }
}

} // namespace
