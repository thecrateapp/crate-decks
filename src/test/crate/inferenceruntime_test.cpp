#include "crate/inference/inferenceruntime.h"

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "test/mixxxtest.h"

namespace {

using crate::InferenceInput;
using crate::InferenceOutput;
using crate::InferenceProvider;
using crate::InferenceRuntime;
using crate::InferenceSession;
using crate::InferenceSystem;

constexpr qint64 kMiB = 1024 * 1024;

/// A machine whose ONNX Runtime build, libraries and video memory the test
/// decides. Sessions still run on the real ONNX Runtime.
class FakeInferenceSystem : public InferenceSystem {
  public:
    QStringList builtProviders() const override {
        return built;
    }

    bool canLoadLibrary(const QString& fileName, QString* pError) const override {
        if (missingLibraries.contains(fileName)) {
            if (pError) {
                *pError = fileName + QStringLiteral(": cannot open shared object file");
            }
            return false;
        }
        return true;
    }

    bool hasProviderLibrary(const QString& fileName) const override {
        return !missingLibraries.contains(fileName);
    }

    std::optional<qint64> freeVideoMemory(InferenceProvider provider, int deviceId) const override {
        Q_UNUSED(deviceId);
        return freeMemory.contains(provider) ? std::optional<qint64>(freeMemory.value(provider))
                                             : std::nullopt;
    }

    QStringList built = {QStringLiteral("CPUExecutionProvider")};
    QSet<QString> missingLibraries;
    QHash<InferenceProvider, qint64> freeMemory;
};

class InferenceRuntimeTest : public MixxxTest {
  protected:
    QString fixturePath() const {
        return getTestDir().filePath(QStringLiteral("crate/data/tiny_inference.onnx"));
    }

    /// Runs the fixture, sum = x + y and scaled = 2x, and checks the outputs.
    static void expectFixtureRuns(InferenceSession* pSession) {
        ASSERT_NE(nullptr, pSession);
        const std::vector<float> x = {0, 1, 2, 3, 4, 5};
        const std::vector<float> y = {1, 1, 1, -1, -1, -1};
        const std::vector<int64_t> shape = {1, 2, 3};
        std::vector<InferenceOutput> outputs;
        QString error;

        ASSERT_TRUE(pSession->run({{QStringLiteral("x"), shape, x},
                                          {QStringLiteral("y"), shape, y}},
                &outputs,
                &error))
                << error.toStdString();

        ASSERT_EQ(2u, outputs.size());
        EXPECT_EQ(QStringLiteral("sum"), outputs[0].name);
        EXPECT_EQ(shape, outputs[0].shape);
        EXPECT_EQ(std::vector<float>({1, 2, 3, 2, 3, 4}), outputs[0].data);
        EXPECT_EQ(QStringLiteral("scaled"), outputs[1].name);
        EXPECT_EQ(std::vector<float>({0, 2, 4, 6, 8, 10}), outputs[1].data);
    }

    /// A machine where CUDA works: an ONNX Runtime build with the CUDA
    /// provider and every library it needs.
    static std::shared_ptr<FakeInferenceSystem> machineWithCuda() {
        auto pSystem = std::make_shared<FakeInferenceSystem>();
        pSystem->built = {QStringLiteral("CUDAExecutionProvider"),
                QStringLiteral("CPUExecutionProvider")};
        pSystem->freeMemory.insert(InferenceProvider::Cuda, 8 * 1024 * kMiB);
        return pSystem;
    }
};

TEST_F(InferenceRuntimeTest, RunsTinyOnnxFixtureOnCpu) {
    const InferenceRuntime runtime;
    QString error;

    const auto pSession = runtime.createSession(fixturePath(), InferenceProvider::Cpu, 0, &error);

    ASSERT_NE(nullptr, pSession) << error.toStdString();
    EXPECT_EQ(InferenceProvider::Cpu, pSession->provider());
    EXPECT_TRUE(pSession->fallbackReason().isEmpty());
    EXPECT_EQ(QStringList({QStringLiteral("x"), QStringLiteral("y")}), pSession->inputNames());
    EXPECT_EQ(QStringList({QStringLiteral("sum"), QStringLiteral("scaled")}),
            pSession->outputNames());
    expectFixtureRuns(pSession.get());
}

TEST_F(InferenceRuntimeTest, CpuIsAlwaysAvailable) {
    const InferenceRuntime runtime(InferenceRuntime::Options(),
            std::make_shared<FakeInferenceSystem>());

    EXPECT_EQ(QList<InferenceProvider>({InferenceProvider::Cpu}), runtime.availableProviders());
    EXPECT_TRUE(runtime.providerStatus(InferenceProvider::Cpu).available);
}

TEST_F(InferenceRuntimeTest, FallsBackToCpuWhenPreferredProviderIsMissing) {
    // The CPU package of ONNX Runtime: no CUDA provider in the build.
    const InferenceRuntime runtime(InferenceRuntime::Options(),
            std::make_shared<FakeInferenceSystem>());
    EXPECT_FALSE(runtime.availableProviders().contains(InferenceProvider::Cuda));

    const auto pSession = runtime.createSession(fixturePath(), InferenceProvider::Cuda, 0);

    ASSERT_NE(nullptr, pSession);
    EXPECT_EQ(InferenceProvider::Cpu, pSession->provider());
    EXPECT_TRUE(pSession->fallbackReason().contains(QStringLiteral("CUDA")))
            << pSession->fallbackReason().toStdString();
    expectFixtureRuns(pSession.get());
}

TEST_F(InferenceRuntimeTest, CudaNeedsTheSystemCudaAndCudnnLibraries) {
    const auto pSystem = machineWithCuda();
    pSystem->missingLibraries.insert(QStringLiteral(
#ifdef Q_OS_WIN
            "cudnn64_9.dll"
#else
            "libcudnn.so.9"
#endif
            ));
    const InferenceRuntime runtime(InferenceRuntime::Options(), pSystem);

    const auto status = runtime.providerStatus(InferenceProvider::Cuda);

    EXPECT_FALSE(status.available);
    EXPECT_TRUE(status.reason.contains(QStringLiteral("cudnn"))) << status.reason.toStdString();
    EXPECT_FALSE(runtime.availableProviders().contains(InferenceProvider::Cuda));
    const auto pSession = runtime.createSession(fixturePath(), InferenceProvider::Cuda, 0);
    ASSERT_NE(nullptr, pSession);
    EXPECT_EQ(InferenceProvider::Cpu, pSession->provider());
    EXPECT_TRUE(pSession->fallbackReason().contains(QStringLiteral("cudnn")))
            << pSession->fallbackReason().toStdString();
}

TEST_F(InferenceRuntimeTest, CudaNeedsTheOptionalProviderLibrary) {
    const auto pSystem = machineWithCuda();
    pSystem->missingLibraries.insert(QStringLiteral(
#ifdef Q_OS_WIN
            "onnxruntime_providers_cuda.dll"
#else
            "libonnxruntime_providers_cuda.so"
#endif
            ));
    const InferenceRuntime runtime(InferenceRuntime::Options(), pSystem);

    const auto status = runtime.providerStatus(InferenceProvider::Cuda);

    EXPECT_FALSE(status.available);
    EXPECT_TRUE(status.reason.contains(QStringLiteral("onnxruntime_providers_cuda")))
            << status.reason.toStdString();
}

TEST_F(InferenceRuntimeTest, FallsBackToCpuWhenFreeVramIsBelowTheRequirement) {
    const auto pSystem = machineWithCuda();
    pSystem->freeMemory.insert(InferenceProvider::Cuda, 1024 * kMiB);
    const InferenceRuntime runtime(InferenceRuntime::Options(), pSystem);
    ASSERT_TRUE(runtime.availableProviders().contains(InferenceProvider::Cuda));

    const auto pSession = runtime.createSession(
            fixturePath(), InferenceProvider::Cuda, 1536 * kMiB);

    ASSERT_NE(nullptr, pSession);
    EXPECT_EQ(InferenceProvider::Cpu, pSession->provider());
    EXPECT_TRUE(pSession->fallbackReason().contains(QStringLiteral("1024 MiB")))
            << pSession->fallbackReason().toStdString();
    EXPECT_TRUE(pSession->fallbackReason().contains(QStringLiteral("1536 MiB")))
            << pSession->fallbackReason().toStdString();
    expectFixtureRuns(pSession.get());
}

TEST_F(InferenceRuntimeTest, FallsBackToCpuWhenTheProviderFailsToStart) {
    if (InferenceRuntime().providerStatus(InferenceProvider::Cuda).available) {
        GTEST_SKIP() << "CUDA works here: the GPU tests cover it";
    }
    // The system claims CUDA works, but the ONNX Runtime build cannot start it.
    const InferenceRuntime runtime(InferenceRuntime::Options(), machineWithCuda());

    const auto pSession = runtime.createSession(fixturePath(), InferenceProvider::Cuda, 0);

    ASSERT_NE(nullptr, pSession);
    EXPECT_EQ(InferenceProvider::Cpu, pSession->provider());
    EXPECT_TRUE(pSession->fallbackReason().contains(QStringLiteral("CUDA")))
            << pSession->fallbackReason().toStdString();
    expectFixtureRuns(pSession.get());
}

TEST_F(InferenceRuntimeTest, ReportsAModelThatDoesNotLoad) {
    const InferenceRuntime runtime;
    QString error;

    EXPECT_EQ(nullptr,
            runtime.createSession(getTestDir().filePath(QStringLiteral("crate/data/missing.onnx")),
                    InferenceProvider::Cpu,
                    0,
                    &error));
    EXPECT_FALSE(error.isEmpty());

    // Not an ONNX graph.
    error.clear();
    EXPECT_EQ(nullptr,
            runtime.createSession(
                    getTestDir().filePath(QStringLiteral("crate/data/make_tiny_inference.py")),
                    InferenceProvider::Cpu,
                    0,
                    &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(InferenceRuntimeTest, RejectsInputsThatDoNotMatchTheirShape) {
    const InferenceRuntime runtime;
    const auto pSession = runtime.createSession(fixturePath(), InferenceProvider::Cpu, 0);
    ASSERT_NE(nullptr, pSession);
    const std::vector<float> data = {0, 1, 2};
    std::vector<InferenceOutput> outputs;
    QString error;

    EXPECT_FALSE(pSession->run({{QStringLiteral("x"), {1, 2, 3}, data},
                                       {QStringLiteral("y"), {1, 2, 3}, data}},
            &outputs,
            &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(outputs.empty());
}

// GPU tests: only with CRATE_TEST_GPU=1, on a machine with the GPU runtime.

bool gpuTestsEnabled() {
    return qEnvironmentVariableIntValue("CRATE_TEST_GPU") == 1;
}

TEST_F(InferenceRuntimeTest, GpuRunsTinyOnnxFixtureOnCuda) {
    if (!gpuTestsEnabled()) {
        GTEST_SKIP() << "Set CRATE_TEST_GPU=1 to run the GPU tests";
    }
    const InferenceRuntime runtime;
    const auto status = runtime.providerStatus(InferenceProvider::Cuda);
    ASSERT_TRUE(status.available) << status.reason.toStdString();
    QString error;

    const auto pSession = runtime.createSession(
            fixturePath(), InferenceProvider::Cuda, 64 * kMiB, &error);

    ASSERT_NE(nullptr, pSession) << error.toStdString();
    EXPECT_EQ(InferenceProvider::Cuda, pSession->provider())
            << pSession->fallbackReason().toStdString();
    expectFixtureRuns(pSession.get());
}

TEST_F(InferenceRuntimeTest, GpuRunsTinyOnnxFixtureOnWebGpu) {
    // CRATE_TEST_WEBGPU_PLUGIN_DIR: the directory of the WebGPU plugin
    // library, libonnxruntime_providers_webgpu.so.
    const QString pluginDirectory = qEnvironmentVariable("CRATE_TEST_WEBGPU_PLUGIN_DIR");
    if (!gpuTestsEnabled() || pluginDirectory.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_TEST_GPU=1 and CRATE_TEST_WEBGPU_PLUGIN_DIR to run it";
    }
    InferenceRuntime::Options options;
    options.providerLibraryDirectory = pluginDirectory;
    const InferenceRuntime runtime(options);
    const auto status = runtime.providerStatus(InferenceProvider::WebGpu);
    ASSERT_TRUE(status.available) << status.reason.toStdString();
    QString error;

    const auto pSession = runtime.createSession(
            fixturePath(), InferenceProvider::WebGpu, 0, &error);

    ASSERT_NE(nullptr, pSession) << error.toStdString();
    EXPECT_EQ(InferenceProvider::WebGpu, pSession->provider())
            << pSession->fallbackReason().toStdString();
    expectFixtureRuns(pSession.get());
}

/// The ST0 §5 speed and parity measurement of the separation graph, through
/// this runtime: one 7.8 s segment on every available provider, against the
/// CPU. Needs CRATE_TEST_GPU=1 and CRATE_TEST_MODEL, the path of an exported
/// htdemucs graph (inputs mix and spec). Prints its results.
TEST_F(InferenceRuntimeTest, GpuMeasuresTheSeparationGraph) {
    const QString modelPath = qEnvironmentVariable("CRATE_TEST_MODEL");
    if (!gpuTestsEnabled() || modelPath.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_TEST_GPU=1 and CRATE_TEST_MODEL to measure a model";
    }
    constexpr int kSamples = 343980;
    constexpr int kBins = 2048;
    constexpr int kFrames = 336;
    // A 5-minute track at the ST0 stride (0.75 of a segment).
    const int segmentsPerTrack = static_cast<int>(std::ceil(300.0 * 44100 / (0.75 * kSamples)));
    std::mt19937 generator(0);
    std::normal_distribution<float> normal(0.0f, 0.3f);
    std::vector<float> mix(2 * kSamples);
    std::generate(mix.begin(), mix.end(), [&] { return normal(generator); });
    // The graph only needs a spectrogram of the right shape and scale.
    std::vector<float> spec(4 * kBins * kFrames);
    std::generate(spec.begin(), spec.end(), [&] { return normal(generator); });
    const std::vector<InferenceInput> inputs = {
            {QStringLiteral("mix"), {1, 2, kSamples}, mix},
            {QStringLiteral("spec"), {1, 4, kBins, kFrames}, spec},
    };

    InferenceRuntime::Options options;
    options.intraOpThreads = 8;
    options.providerLibraryDirectory = qEnvironmentVariable("CRATE_TEST_WEBGPU_PLUGIN_DIR");
    const InferenceRuntime runtime(options);
    std::vector<InferenceOutput> reference;
    for (const InferenceProvider provider : runtime.availableProviders()) {
        QElapsedTimer timer;
        timer.start();
        QString error;
        const auto pSession = runtime.createSession(modelPath, provider, 0, &error);
        ASSERT_NE(nullptr, pSession) << error.toStdString();
        ASSERT_EQ(provider, pSession->provider()) << pSession->fallbackReason().toStdString();
        const qint64 createMs = timer.restart();
        std::vector<InferenceOutput> outputs;
        ASSERT_TRUE(pSession->run(inputs, &outputs, &error)) << error.toStdString();
        const qint64 firstMs = timer.restart();
        std::vector<qint64> runsMs;
        for (int i = 0; i < 5; ++i) {
            timer.restart();
            ASSERT_TRUE(pSession->run(inputs, &outputs, &error)) << error.toStdString();
            runsMs.push_back(timer.elapsed());
        }
        std::sort(runsMs.begin(), runsMs.end());
        const qint64 medianMs = runsMs[runsMs.size() / 2];
        std::cout << "[measure] " << crate::inferenceProviderName(provider).toStdString()
                  << ": session " << createMs << " ms, first run " << firstMs
                  << " ms, median run " << medianMs << " ms, 5-minute track ~"
                  << medianMs * segmentsPerTrack / 1000.0 << " s" << std::endl;
        if (provider == InferenceProvider::Cpu) {
            reference = std::move(outputs);
            continue;
        }
        ASSERT_EQ(reference.size(), outputs.size());
        for (size_t i = 0; i < outputs.size(); ++i) {
            ASSERT_EQ(reference[i].data.size(), outputs[i].data.size());
            double signal = 0;
            double noise = 0;
            float maxDifference = 0;
            for (size_t j = 0; j < outputs[i].data.size(); ++j) {
                const double difference = outputs[i].data[j] - reference[i].data[j];
                signal += static_cast<double>(reference[i].data[j]) * reference[i].data[j];
                noise += difference * difference;
                maxDifference = std::max(maxDifference, static_cast<float>(std::abs(difference)));
            }
            const double snrDb = 10 * std::log10(signal / std::max(noise, 1e-30));
            std::cout << "[measure]   " << outputs[i].name.toStdString()
                      << " against the CPU: max |difference| " << maxDifference << ", SNR "
                      << snrDb << " dB" << std::endl;
            EXPECT_GT(snrDb, 60.0);
        }
    }
}

} // namespace
