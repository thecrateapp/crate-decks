#include "crate/stems/stemseparator.h"

#include <gtest/gtest.h>

#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

#include "crate/inference/inferenceruntime.h"
#include "crate/stems/stemdsp.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"

namespace {

using crate::InferenceProvider;
using crate::InferenceRuntime;
using crate::StemBuffer;
using crate::StemModel;
using crate::StemSeparationResult;
using crate::StemSeparator;
using crate::StemSink;
namespace stemdsp = crate::stemdsp;

/// The gains of the identity mock model per source: they add up to 1.
constexpr float kGains[stemdsp::kSources] = {0.1f, 0.2f, 0.3f, 0.4f};

/// What the tests see of the mock model, which the separator owns.
struct MockModelState {
    std::atomic<int> runs = 0;
    std::atomic<bool> aborted = false;
    /// run() waits until abort() instead of returning.
    std::atomic<bool> blockUntilAborted = false;
    /// Released when a run starts.
    QSemaphore running;
};

/// An identity model: every source is the segment times its gain, from the
/// time branch; the spectral branch is zero.
class MockStemModel : public StemModel {
  public:
    explicit MockStemModel(std::shared_ptr<MockModelState> pState)
            : m_pState(std::move(pState)) {
    }

    bool run(std::span<const float> mix,
            std::span<const float> spec,
            std::vector<float>* pSpecOut,
            std::vector<float>* pWaveOut,
            QString* pError) override {
        EXPECT_EQ(size_t(2 * stemdsp::kSegmentFrames), mix.size());
        EXPECT_EQ(size_t(4 * stemdsp::kBins * stemdsp::kSegmentSpectrumFrames), spec.size());
        ++m_pState->runs;
        m_pState->running.release();
        QElapsedTimer timer;
        timer.start();
        while (m_pState->blockUntilAborted && !m_pState->aborted && timer.elapsed() < 30000) {
            QThread::msleep(1);
        }
        if (m_pState->aborted) {
            *pError = QStringLiteral("aborted");
            return false;
        }
        pSpecOut->assign(stemdsp::kSources * spec.size(), 0.0f);
        pWaveOut->resize(stemdsp::kSources * mix.size());
        for (int source = 0; source < stemdsp::kSources; ++source) {
            for (size_t i = 0; i < mix.size(); ++i) {
                (*pWaveOut)[source * mix.size() + i] = mix[i] * kGains[source];
            }
        }
        return true;
    }

    void abort() override {
        m_pState->aborted = true;
    }
    void clearAbort() override {
        m_pState->aborted = false;
    }
    QString description() const override {
        return QStringLiteral("mock");
    }

  private:
    const std::shared_ptr<MockModelState> m_pState;
};

/// Writes a 16-bit PCM WAV file of interleaved samples.
void writeWav(const QString& path,
        int sampleRate,
        const std::vector<qint16>& samples,
        int channels = 2) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataBytes = quint32(samples.size() * sizeof(qint16));
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + dataBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(channels) << quint32(sampleRate)
           << quint32(sampleRate * 2 * channels) << quint16(2 * channels) << quint16(16);
    stream.writeRawData("data", 4);
    stream << dataBytes;
    for (const qint16 sample : samples) {
        stream << sample;
    }
}

/// Tones and noise, never music.
std::vector<qint16> testSignal(int sampleRate, SINT frames) {
    std::mt19937 generator(5);
    std::normal_distribution<double> noise(0.0, 0.02);
    std::vector<qint16> samples(2 * frames);
    for (SINT i = 0; i < frames; ++i) {
        const double t = double(i) / sampleRate;
        const double left = 0.3 * std::sin(2 * std::numbers::pi * 220 * t) +
                noise(generator) + 0.01;
        const double right = 0.2 * std::sin(2 * std::numbers::pi * 1000 * t) + noise(generator);
        samples[2 * i] = qint16(std::lround(left * 32767));
        samples[2 * i + 1] = qint16(std::lround(right * 32767));
    }
    return samples;
}

class StemSeparatorTest : public MixxxTest, public SoundSourceProviderRegistration {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_directory.isValid());
        m_pState = std::make_shared<MockModelState>();
    }

    StemSeparator::ModelFactory mockFactory() const {
        return [pState = m_pState](QString*) { return std::make_unique<MockStemModel>(pState); };
    }

    QString writeTestFile(const QString& name, int sampleRate, SINT frames) {
        const QString path = m_directory.filePath(name);
        writeWav(path, sampleRate, testSignal(sampleRate, frames));
        return path;
    }

    static StemSeparationResult waitForFinished(
            QSignalSpy* pSpy, StemSeparator::JobId id, int timeoutMs = 60000) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            for (const QList<QVariant>& arguments : std::as_const(*pSpy)) {
                if (arguments.at(0).value<StemSeparator::JobId>() == id) {
                    return arguments.at(1).value<StemSeparationResult>();
                }
            }
            pSpy->wait(50);
        }
        ADD_FAILURE() << "Job " << id << " did not finish";
        return StemSeparationResult();
    }

    QTemporaryDir m_directory;
    std::shared_ptr<MockModelState> m_pState;
};

TEST_F(StemSeparatorTest, OverlapAddReconstructsTheInput) {
    // 20 s: four segments, the last one short.
    constexpr SINT kFrames = 20 * 44100;
    const QString path = writeTestFile(QStringLiteral("input.wav"), 44100, kFrames);
    const std::vector<qint16> samples = testSignal(44100, kFrames);
    StemSeparator separator(mockFactory());
    QSignalSpy progress(&separator, &StemSeparator::progress);
    QSignalSpy finished(&separator, &StemSeparator::finished);
    const auto pStems = std::make_shared<StemBuffer>();

    const auto id = separator.separate(Track::newTemporary(path), pStems);
    const StemSeparationResult result = waitForFinished(&finished, id);

    ASSERT_EQ(StemSeparationResult::Status::Done, result.status) << result.error.toStdString();
    EXPECT_EQ(44100, result.source.originalSampleRate);
    EXPECT_EQ(2, result.source.originalChannelCount);
    EXPECT_EQ(kFrames, result.source.originalFrames);
    EXPECT_EQ(kFrames, result.source.frames);
    EXPECT_EQ(QStringLiteral("mock"), result.model);
    EXPECT_EQ(4, m_pState->runs);
    ASSERT_EQ(kFrames, pStems->frames());
    ASSERT_FALSE(progress.isEmpty());
    double lastFraction = 0;
    for (const QList<QVariant>& arguments : std::as_const(progress)) {
        EXPECT_EQ(id, arguments.at(0).value<StemSeparator::JobId>());
        EXPECT_GE(arguments.at(1).toDouble(), lastFraction);
        lastFraction = arguments.at(1).toDouble();
    }
    EXPECT_EQ(1.0, lastFraction);
    // Demucs de-normalises every source with the track's mean, so each stem
    // is gain × (input - mean) + mean.
    double mean = 0;
    for (SINT i = 0; i < kFrames; ++i) {
        mean += (double(samples[2 * i]) + samples[2 * i + 1]) / 2 / 32768;
    }
    mean /= kFrames;
    for (int source = 0; source < stemdsp::kSources; ++source) {
        for (int channel = 0; channel < 2; ++channel) {
            const std::span<const float> stem = pStems->channel(source, channel);
            double maxDifference = 0;
            for (SINT i = 0; i < kFrames; ++i) {
                const double input = double(samples[2 * i + channel]) / 32768;
                const double expected = kGains[source] * (input - mean) + mean;
                maxDifference = std::max(maxDifference, std::abs(expected - stem[i]));
            }
            EXPECT_LT(maxDifference, 1e-5) << "source " << source << ", channel " << channel;
        }
    }
}

TEST_F(StemSeparatorTest, ResamplesTheOriginalTo44100Hz) {
    constexpr SINT kFrames = 10 * 48000;
    const QString path = writeTestFile(QStringLiteral("input48.wav"), 48000, kFrames);
    StemSeparator separator(mockFactory());
    QSignalSpy finished(&separator, &StemSeparator::finished);
    const auto pStems = std::make_shared<StemBuffer>();

    const auto id = separator.separate(Track::newTemporary(path), pStems);
    const StemSeparationResult result = waitForFinished(&finished, id);

    ASSERT_EQ(StemSeparationResult::Status::Done, result.status) << result.error.toStdString();
    EXPECT_EQ(48000, result.source.originalSampleRate);
    EXPECT_EQ(kFrames, result.source.originalFrames);
    EXPECT_EQ(441000, result.source.frames);
    ASSERT_EQ(441000, pStems->frames());
    // The stems add up to the input plus three times its mean (see above),
    // and the right channel is a 1 kHz tone plus noise.
    const std::vector<qint16> samples = testSignal(48000, kFrames);
    double mean = 0;
    for (SINT i = 0; i < kFrames; ++i) {
        mean += (double(samples[2 * i]) + samples[2 * i + 1]) / 2 / 32768;
    }
    mean /= kFrames;
    double sumOfSquares = 0;
    double maxDifference = 0;
    for (SINT i = 1000; i < 440000; ++i) {
        const double expected = 0.2 * std::sin(2 * std::numbers::pi * 1000 * double(i) / 44100);
        double sum = -3 * mean;
        for (int source = 0; source < 4; ++source) {
            sum += pStems->channel(source, 1)[i];
        }
        sumOfSquares += (sum - expected) * (sum - expected);
        maxDifference = std::max(maxDifference, std::abs(sum - expected));
    }
    // The test signal has noise with a standard deviation of 0.02.
    EXPECT_LT(std::sqrt(sumOfSquares / 439000), 0.025);
    EXPECT_LT(maxDifference, 0.15);
}

TEST_F(StemSeparatorTest, TakesAMonoOriginalAsBothChannels) {
    constexpr SINT kFrames = 3 * 44100;
    std::vector<qint16> samples(kFrames);
    for (SINT i = 0; i < kFrames; ++i) {
        samples[i] = qint16(std::lround(9000 * std::sin(0.05 * double(i))));
    }
    const QString path = m_directory.filePath(QStringLiteral("mono.wav"));
    writeWav(path, 44100, samples, 1);
    StemSeparator separator(mockFactory());
    QSignalSpy finished(&separator, &StemSeparator::finished);
    const auto pStems = std::make_shared<StemBuffer>();

    const auto id = separator.separate(Track::newTemporary(path), pStems);
    const StemSeparationResult result = waitForFinished(&finished, id);

    ASSERT_EQ(StemSeparationResult::Status::Done, result.status) << result.error.toStdString();
    EXPECT_EQ(1, result.source.originalChannelCount);
    ASSERT_EQ(kFrames, pStems->frames());
    double mean = 0;
    for (const qint16 sample : samples) {
        mean += sample / 32768.0;
    }
    mean /= kFrames;
    double maxDifference = 0;
    for (int channel = 0; channel < 2; ++channel) {
        const std::span<const float> vocals = pStems->channel(3, channel);
        for (SINT i = 0; i < kFrames; ++i) {
            const double expected = kGains[3] * (samples[i] / 32768.0 - mean) + mean;
            maxDifference = std::max(maxDifference, std::abs(expected - vocals[i]));
        }
    }
    EXPECT_LT(maxDifference, 1e-5);
}

TEST_F(StemSeparatorTest, CancelsPromptly) {
    const QString path = writeTestFile(QStringLiteral("input.wav"), 44100, 20 * 44100);
    m_pState->blockUntilAborted = true;
    StemSeparator separator(mockFactory());
    QSignalSpy finished(&separator, &StemSeparator::finished);

    const auto id = separator.separate(Track::newTemporary(path), std::make_shared<StemBuffer>());
    ASSERT_TRUE(m_pState->running.tryAcquire(1, 30000));
    QElapsedTimer timer;
    timer.start();
    separator.cancel(id);
    const StemSeparationResult result = waitForFinished(&finished, id);

    EXPECT_EQ(StemSeparationResult::Status::Canceled, result.status);
    EXPECT_LT(timer.elapsed(), 500);
    EXPECT_EQ(1, m_pState->runs);

    // The next job runs to the end.
    m_pState->blockUntilAborted = false;
    const auto nextId =
            separator.separate(Track::newTemporary(path), std::make_shared<StemBuffer>());
    EXPECT_EQ(StemSeparationResult::Status::Done, waitForFinished(&finished, nextId).status);
}

TEST_F(StemSeparatorTest, CancelsAQueuedJobWithoutRunningIt) {
    const QString path = writeTestFile(QStringLiteral("input.wav"), 44100, 5 * 44100);
    m_pState->blockUntilAborted = true;
    StemSeparator separator(mockFactory());
    QSignalSpy finished(&separator, &StemSeparator::finished);
    const auto firstId =
            separator.separate(Track::newTemporary(path), std::make_shared<StemBuffer>());
    const auto secondId =
            separator.separate(Track::newTemporary(path), std::make_shared<StemBuffer>());
    ASSERT_TRUE(m_pState->running.tryAcquire(1, 30000));

    separator.cancel(secondId);

    EXPECT_EQ(StemSeparationResult::Status::Canceled,
            waitForFinished(&finished, secondId, 1000).status);
    separator.cancel(firstId);
    EXPECT_EQ(StemSeparationResult::Status::Canceled,
            waitForFinished(&finished, firstId).status);
    EXPECT_EQ(1, m_pState->runs);
}

TEST_F(StemSeparatorTest, ReportsAFileThatDoesNotDecode) {
    const QString path = m_directory.filePath(QStringLiteral("broken.wav"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("RIFF and nothing else");
    file.close();
    StemSeparator separator(mockFactory());
    QSignalSpy finished(&separator, &StemSeparator::finished);

    const auto id = separator.separate(Track::newTemporary(path), std::make_shared<StemBuffer>());
    const StemSeparationResult result = waitForFinished(&finished, id);

    EXPECT_EQ(StemSeparationResult::Status::Failed, result.status);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_EQ(0, m_pState->runs);
}

TEST_F(StemSeparatorTest, ReportsAModelThatDoesNotLoad) {
    const QString path = writeTestFile(QStringLiteral("input.wav"), 44100, 44100);
    StemSeparator separator([](QString* pError) {
        *pError = QStringLiteral("no model here");
        return std::unique_ptr<StemModel>();
    });
    QSignalSpy finished(&separator, &StemSeparator::finished);

    const auto id = separator.separate(Track::newTemporary(path), std::make_shared<StemBuffer>());
    const StemSeparationResult result = waitForFinished(&finished, id);

    EXPECT_EQ(StemSeparationResult::Status::Failed, result.status);
    EXPECT_TRUE(result.error.contains(QStringLiteral("no model here")))
            << result.error.toStdString();
}

// The golden test: needs the exported model and its golden set, which are
// release assets and not part of the repository.
// - CRATE_TEST_MODEL: htdemucs-onnx-1.onnx.
// - CRATE_TEST_STEMS_GOLDEN: the directory htdemucs-onnx-1-golden.
// The GPU variant also needs CRATE_TEST_GPU=1.

std::vector<float> readFloats(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        ADD_FAILURE() << "Cannot read " << path.toStdString();
        return {};
    }
    const QByteArray bytes = file.readAll();
    std::vector<float> values(bytes.size() / sizeof(float));
    for (size_t i = 0; i < values.size(); ++i) {
        const quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + i * sizeof(float));
        std::memcpy(&values[i], &bits, sizeof(float));
    }
    return values;
}

double sdrDb(std::span<const float> reference, std::span<const float> estimate) {
    double signal = 0;
    double noise = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        const double difference = double(reference[i]) - estimate[i];
        signal += double(reference[i]) * reference[i];
        noise += difference * difference;
    }
    return 10 * std::log10(signal / std::max(noise, 1e-30));
}

void expectGoldenStems(InferenceProvider provider, double minimumSdrDb) {
    const QString modelPath = qEnvironmentVariable("CRATE_TEST_MODEL");
    const QDir golden(qEnvironmentVariable("CRATE_TEST_STEMS_GOLDEN"));
    constexpr SINT kFrames = 441000;
    const std::vector<float> input = readFloats(golden.filePath(QStringLiteral("track_input.bin")));
    const std::vector<float> expected =
            readFloats(golden.filePath(QStringLiteral("track_stems.bin")));
    ASSERT_EQ(size_t(2 * kFrames), input.size());
    ASSERT_EQ(size_t(8 * kFrames), expected.size());
    const InferenceRuntime runtime;
    QString error;
    const auto pModel = crate::createHtdemucsModel(runtime, modelPath, provider, &error);
    ASSERT_NE(nullptr, pModel) << error.toStdString();
    ASSERT_EQ(crate::inferenceProviderName(provider), pModel->description());
    StemBuffer stems;
    crate::StemSeparationSource source;
    source.originalSampleRate = 44100;
    source.originalChannelCount = 2;
    source.originalFrames = kFrames;
    source.frames = kFrames;
    ASSERT_TRUE(stems.begin(source, &error));

    std::vector<float> normalised = input;
    ASSERT_TRUE(crate::separateStems(normalised, kFrames, pModel.get(), &stems, nullptr, &error))
            << error.toStdString();

    ASSERT_EQ(kFrames, stems.frames());
    for (int source = 0; source < stemdsp::kSources; ++source) {
        const std::span<const float> reference(expected.data() + 2 * source * kFrames, 2 * kFrames);
        std::vector<float> estimate(stems.channel(source, 0).begin(),
                stems.channel(source, 0).end());
        estimate.insert(estimate.end(),
                stems.channel(source, 1).begin(),
                stems.channel(source, 1).end());
        const double sdr = sdrDb(reference, estimate);
        std::cout << "[golden] " << crate::inferenceProviderName(provider).toStdString()
                  << " source " << source << ": SDR " << sdr << " dB" << std::endl;
        EXPECT_GE(sdr, minimumSdrDb) << "source " << source;
    }
}

bool goldenTestsEnabled() {
    return !qEnvironmentVariable("CRATE_TEST_MODEL").isEmpty() &&
            !qEnvironmentVariable("CRATE_TEST_STEMS_GOLDEN").isEmpty();
}

TEST(StemSeparatorGoldenTest, MatchesTheGoldenStemsOnCpu) {
    if (!goldenTestsEnabled()) {
        GTEST_SKIP() << "Set CRATE_TEST_MODEL and CRATE_TEST_STEMS_GOLDEN to run it";
    }
    expectGoldenStems(InferenceProvider::Cpu, 60.0);
}

TEST(StemSeparatorGoldenTest, GpuMatchesTheGoldenStemsOnCuda) {
    if (!goldenTestsEnabled() || qEnvironmentVariableIntValue("CRATE_TEST_GPU") != 1) {
        GTEST_SKIP() << "Set CRATE_TEST_GPU=1, CRATE_TEST_MODEL and CRATE_TEST_STEMS_GOLDEN";
    }
    expectGoldenStems(InferenceProvider::Cuda, 60.0);
}

/// Cancels the real graph in the middle of a run on the CPU, where one
/// segment takes seconds. Needs CRATE_TEST_MODEL.
TEST_F(StemSeparatorTest, CancelsARunningGraphPromptly) {
    const QString modelPath = qEnvironmentVariable("CRATE_TEST_MODEL");
    if (modelPath.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_TEST_MODEL to run it";
    }
    const QString path = writeTestFile(QStringLiteral("input.wav"), 44100, 20 * 44100);
    const auto pRuntime = std::make_shared<InferenceRuntime>();
    StemSeparator separator([pRuntime, modelPath](QString* pError) {
        return crate::createHtdemucsModel(*pRuntime, modelPath, InferenceProvider::Cpu, pError);
    });
    QSignalSpy progress(&separator, &StemSeparator::progress);
    QSignalSpy finished(&separator, &StemSeparator::finished);
    const auto id = separator.separate(Track::newTemporary(path), std::make_shared<StemBuffer>());
    // Decoding ends at 10%; the first segment's run then takes seconds.
    QElapsedTimer timer;
    timer.start();
    while ((progress.isEmpty() || progress.last().at(1).toDouble() < 0.1) &&
            timer.elapsed() < 60000) {
        progress.wait(50);
    }
    QThread::msleep(1000);
    ASSERT_TRUE(finished.isEmpty());

    timer.restart();
    separator.cancel(id);
    const StemSeparationResult result = waitForFinished(&finished, id);

    EXPECT_EQ(StemSeparationResult::Status::Canceled, result.status) << result.error.toStdString();
    std::cout << "[cancel] a running CPU graph stopped in " << timer.elapsed() << " ms"
              << std::endl;
    EXPECT_LT(timer.elapsed(), 1000);
}

/// Discards the stems: the measurement keeps no audio in memory.
class CountingStemSink : public StemSink {
  public:
    bool write(SINT firstFrame,
            SINT frameCount,
            std::span<const float> stems,
            QString* pError) override {
        Q_UNUSED(firstFrame);
        Q_UNUSED(stems);
        Q_UNUSED(pError);
        frames += frameCount;
        return true;
    }
    SINT frames = 0;
};

/// The end-to-end time of one real track (decode, resampling, DSP, graph and
/// overlap-add) on the CPU and, with CRATE_TEST_GPU=1, on CUDA. Needs
/// CRATE_TEST_MODEL and CRATE_TEST_STEMS_TRACK, the path of a local audio
/// file. Prints its results.
TEST_F(StemSeparatorTest, MeasuresARealTrack) {
    const QString modelPath = qEnvironmentVariable("CRATE_TEST_MODEL");
    const QString trackPath = qEnvironmentVariable("CRATE_TEST_STEMS_TRACK");
    if (modelPath.isEmpty() || trackPath.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_TEST_MODEL and CRATE_TEST_STEMS_TRACK to measure a track";
    }
    QList<InferenceProvider> providers = {InferenceProvider::Cpu};
    if (qEnvironmentVariableIntValue("CRATE_TEST_GPU") == 1) {
        providers.prepend(InferenceProvider::Cuda);
    }
    if (qEnvironmentVariableIntValue("CRATE_TEST_STEMS_SKIP_CPU") == 1) {
        providers.removeAll(InferenceProvider::Cpu);
    }
    const auto pRuntime = std::make_shared<InferenceRuntime>();
    for (const InferenceProvider provider : std::as_const(providers)) {
        StemSeparator separator([pRuntime, modelPath, provider](QString* pError) {
            return crate::createHtdemucsModel(*pRuntime, modelPath, provider, pError);
        });
        QSignalSpy finished(&separator, &StemSeparator::finished);
        const auto pSink = std::make_shared<CountingStemSink>();
        QElapsedTimer timer;
        timer.start();

        const auto id = separator.separate(Track::newTemporary(trackPath), pSink);
        const StemSeparationResult result = waitForFinished(&finished, id, 30 * 60 * 1000);

        ASSERT_EQ(StemSeparationResult::Status::Done, result.status) << result.error.toStdString();
        EXPECT_EQ(result.source.frames, pSink->frames);
        std::cout << "[measure] " << result.model.toStdString() << ": "
                  << double(result.source.frames) / 44100 << " s of audio ("
                  << result.source.originalSampleRate << " Hz), total "
                  << timer.elapsed() / 1000.0 << " s: model " << result.modelMs / 1000.0
                  << " s, decode and resampling " << result.decodeMs / 1000.0 << " s, DSP "
                  << result.dspMs / 1000.0 << " s, graph " << result.graphMs / 1000.0 << " s"
                  << std::endl;
    }
}

} // namespace
