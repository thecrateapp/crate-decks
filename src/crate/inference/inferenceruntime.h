#pragma once

#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace crate {

/// An ONNX Runtime execution provider that Decks can run a graph on
/// (STEM-D7). The CPU is always there; the others depend on the ONNX Runtime
/// build, the operating system and the GPU.
enum class InferenceProvider {
    Cpu,
    /// NVIDIA on Linux and Windows. Needs the user's CUDA and cuDNN
    /// libraries, which are never bundled.
    Cuda,
    /// Windows.
    DirectMl,
    /// macOS.
    CoreMl,
    /// Any GPU with a Vulkan, Direct3D 12 or Metal driver. On Linux it comes
    /// as a plugin library (libonnxruntime_providers_webgpu.so).
    WebGpu,
};

/// "CPU", "CUDA", "DirectML", "CoreML" or "WebGPU".
QString inferenceProviderName(InferenceProvider provider);

/// Whether a provider can run here, and why not.
struct InferenceProviderStatus {
    InferenceProvider provider = InferenceProvider::Cpu;
    bool available = false;
    /// Why it is not available, for the preferences and the log; empty if
    /// it is.
    QString reason;
};

/// A float tensor for InferenceSession::run(). It does not own its data,
/// which must hold the product of `shape` values.
struct InferenceInput {
    QString name;
    std::vector<int64_t> shape;
    std::span<const float> data;
};

/// A float tensor that InferenceSession::run() returns.
struct InferenceOutput {
    QString name;
    std::vector<int64_t> shape;
    std::vector<float> data;
};

/// What InferenceRuntime asks the machine. The default implementation asks
/// the real one; tests override it.
class InferenceSystem {
  public:
    virtual ~InferenceSystem() = default;

    /// The execution providers compiled into the ONNX Runtime library, with
    /// ONNX Runtime's names, e.g. "CUDAExecutionProvider".
    virtual QStringList builtProviders() const;
    /// Whether the dynamic library `fileName` loads (dlopen or LoadLibrary)
    /// from the system's search path. If not, `pError` gets the reason.
    virtual bool canLoadLibrary(const QString& fileName, QString* pError) const;
    /// Whether the provider library `fileName` (e.g.
    /// libonnxruntime_providers_cuda.so) is installed next to the ONNX
    /// Runtime library, where ONNX Runtime loads it from.
    virtual bool hasProviderLibrary(const QString& fileName) const;
    /// Free memory of the GPU `deviceId` of `provider`, in bytes, or nullopt
    /// if it cannot be measured. CUDA asks NVML, which comes with the NVIDIA
    /// driver.
    virtual std::optional<qint64> freeVideoMemory(InferenceProvider provider, int deviceId) const;
};

/// How InferenceRuntime runs graphs.
struct InferenceOptions {
    /// Threads of one run on the CPU, from the preferences; 0 lets ONNX
    /// Runtime choose (one per physical core).
    int intraOpThreads = 0;
    /// The GPU that GPU providers use.
    int deviceId = 0;
    /// Where optional provider libraries downloaded at run time live: the
    /// WebGPU plugin. Empty for none.
    QString providerLibraryDirectory;
};

/// How a session of one provider runs its graph.
struct InferenceSessionConfig {
    /// Runs independent nodes at the same time (ONNX Runtime's parallel
    /// executor) instead of one after another.
    bool parallelExecution = false;
    int interOpThreads = 1;
    /// 0 lets ONNX Runtime choose.
    int intraOpThreads = 0;
    /// Plans the memory of a run from the first one. Costs VRAM on GPUs.
    bool memoryPattern = true;
};

class InferenceSession;

/// Runs ONNX graphs with ONNX Runtime (STEM-D7), on the GPU when the machine
/// can and on the CPU otherwise.
///
/// GPU runtimes come from the user's system and are never bundled: CUDA is
/// offered only when the CUDA and cuDNN libraries that this ONNX Runtime
/// build needs load, and when its CUDA provider library (an optional
/// download) is installed.
///
/// Every method may block for a long time: it loads libraries, and creating
/// a session takes seconds. Call them from a worker thread, never from the
/// main or the engine thread. The methods are thread-safe.
class InferenceRuntime {
  public:
    using Provider = InferenceProvider;
    using Options = InferenceOptions;

    /// Without `pSystem`, the real machine.
    explicit InferenceRuntime(const Options& options = Options(),
            std::shared_ptr<InferenceSystem> pSystem = nullptr);
    ~InferenceRuntime();
    InferenceRuntime(const InferenceRuntime&) = delete;
    InferenceRuntime& operator=(const InferenceRuntime&) = delete;

    /// The version of the loaded ONNX Runtime library, e.g. "1.31.1".
    static QString onnxRuntimeVersion();

    /// The providers that can run here, in the order of InferenceProvider:
    /// the CPU first. Detected on first use and remembered.
    QList<Provider> availableProviders() const;
    InferenceProviderStatus providerStatus(Provider provider) const;
    /// How createSession() runs graphs on `provider`.
    InferenceSessionConfig sessionConfig(Provider provider) const;

    /// Loads the graph at `modelPath` for `preferred`. Falls back to the CPU,
    /// with the reason in InferenceSession::fallbackReason(), when that
    /// provider is not available, when its GPU has less free memory than
    /// `requiredVramBytes` (0 skips the check), or when it fails to start.
    ///
    /// Returns nullptr, with the reason in `pError`, only if the graph does
    /// not load on the CPU either.
    std::unique_ptr<InferenceSession> createSession(const QString& modelPath,
            Provider preferred,
            qint64 requiredVramBytes,
            QString* pError = nullptr) const;

  private:
    struct Impl;
    InferenceProviderStatus detectStatus(Provider provider) const;

    const Options m_options;
    const std::shared_ptr<InferenceSystem> m_pSystem;
    const std::unique_ptr<Impl> m_pImpl;
    mutable QMutex m_mutex;
    mutable QList<InferenceProviderStatus> m_statuses;
};

/// A graph loaded on one provider. Runs block; run one at a time.
class InferenceSession {
  public:
    ~InferenceSession();
    InferenceSession(const InferenceSession&) = delete;
    InferenceSession& operator=(const InferenceSession&) = delete;

    /// The provider the graph runs on. Nodes that it cannot run go to the
    /// CPU (ONNX Runtime logs a warning).
    InferenceProvider provider() const {
        return m_provider;
    }
    /// Why the session runs on the CPU and not on the preferred provider;
    /// empty if it runs on that one.
    const QString& fallbackReason() const {
        return m_fallbackReason;
    }
    QStringList inputNames() const;
    QStringList outputNames() const;

    /// Runs the graph once. On failure returns false, with the reason in
    /// `pError`, and leaves `pOutputs` empty.
    bool run(const std::vector<InferenceInput>& inputs,
            std::vector<InferenceOutput>* pOutputs,
            QString* pError = nullptr);

    /// Makes the run in progress fail between two nodes of the graph, and
    /// every later run fail, until clearTermination(). Thread-safe: this is
    /// how another thread cancels a run.
    void terminate();
    void clearTermination();

  private:
    friend class InferenceRuntime;
    struct Impl;
    InferenceSession(std::unique_ptr<Impl> pImpl, InferenceProvider provider);

    const std::unique_ptr<Impl> m_pImpl;
    const InferenceProvider m_provider;
    QString m_fallbackReason;
};

} // namespace crate
