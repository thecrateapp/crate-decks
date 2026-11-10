#include "crate/inference/inferenceruntime.h"

#include <onnxruntime_cxx_api.h>
#include <onnxruntime_session_options_config_keys.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <clocale>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "util/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#if __has_include(<dml_provider_factory.h>)
#include <dml_provider_factory.h>
#define CRATE_INFERENCE_DIRECTML
#endif
#else
#include <dlfcn.h>
#include <locale.h>
#ifdef Q_OS_MACOS
#include <xlocale.h>
#endif
#endif

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::InferenceRuntime");

constexpr qint64 kMiB = 1024 * 1024;

const QList<InferenceProvider> kProviders = {
        InferenceProvider::Cpu,
        InferenceProvider::Cuda,
        InferenceProvider::DirectMl,
        InferenceProvider::CoreMl,
        InferenceProvider::WebGpu,
};

// ONNX Runtime's names of its execution providers.
const QString kCudaProviderName = QStringLiteral("CUDAExecutionProvider");
const QString kDirectMlProviderName = QStringLiteral("DmlExecutionProvider");
const QString kCoreMlProviderName = QStringLiteral("CoreMLExecutionProvider");
const std::string kWebGpuProviderName = "WebGpuExecutionProvider";
// The name the WebGPU plugin library is registered with.
const char* const kWebGpuPluginRegistration = "crate_webgpu";
// The metadata key ONNX Runtime sets to "1" on a discrete GPU.
const char* const kDiscreteGpuKey = "Discrete";

#ifdef Q_OS_WIN
const QString kCudaProviderLibrary = QStringLiteral("onnxruntime_providers_cuda.dll");
const QString kWebGpuPluginLibrary = QStringLiteral("onnxruntime_providers_webgpu.dll");
// What the CUDA provider of ONNX Runtime 1.31 (CUDA 13 build) loads: the
// driver, the CUDA 13 runtime, cuBLAS, cuRAND, cuFFT and cuDNN 9. None of
// them is part of Decks.
const QStringList kCudaSystemLibraries = {
        QStringLiteral("nvcuda.dll"),
        QStringLiteral("cudart64_13.dll"),
        QStringLiteral("cublas64_13.dll"),
        QStringLiteral("cublasLt64_13.dll"),
        QStringLiteral("curand64_10.dll"),
        QStringLiteral("cufft64_12.dll"),
        QStringLiteral("cudnn64_9.dll"),
};
const QString kNvmlLibrary = QStringLiteral("nvml.dll");
#else
const QString kCudaProviderLibrary = QStringLiteral("libonnxruntime_providers_cuda.so");
const QString kWebGpuPluginLibrary = QStringLiteral("libonnxruntime_providers_webgpu.so");
const QStringList kCudaSystemLibraries = {
        QStringLiteral("libcuda.so.1"),
        QStringLiteral("libcudart.so.13"),
        QStringLiteral("libcublas.so.13"),
        QStringLiteral("libcublasLt.so.13"),
        QStringLiteral("libcurand.so.10"),
        QStringLiteral("libcufft.so.12"),
        QStringLiteral("libcudnn.so.9"),
};
const QString kNvmlLibrary = QStringLiteral("libnvidia-ml.so.1");
#endif

/// A dynamic library, unloaded when this goes out of scope.
class DynamicLibrary {
  public:
    explicit DynamicLibrary(const QString& fileName) {
#ifdef Q_OS_WIN
        m_handle = LoadLibraryW(reinterpret_cast<const wchar_t*>(fileName.utf16()));
        if (!m_handle) {
            m_error = QStringLiteral("%1: error %2").arg(fileName).arg(GetLastError());
        }
#else
        m_handle = dlopen(QFile::encodeName(fileName).constData(), RTLD_LAZY | RTLD_LOCAL);
        if (!m_handle) {
            const char* const error = dlerror();
            m_error = error ? QString::fromLocal8Bit(error) : fileName;
        }
#endif
    }

    ~DynamicLibrary() {
        if (!m_handle) {
            return;
        }
#ifdef Q_OS_WIN
        FreeLibrary(m_handle);
#else
        dlclose(m_handle);
#endif
    }

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    bool isLoaded() const {
        return m_handle != nullptr;
    }

    const QString& error() const {
        return m_error;
    }

    template<typename Function>
    Function resolve(const char* symbol) const {
        if (!m_handle) {
            return nullptr;
        }
#ifdef Q_OS_WIN
        return reinterpret_cast<Function>(GetProcAddress(m_handle, symbol));
#else
        return reinterpret_cast<Function>(dlsym(m_handle, symbol));
#endif
    }

  private:
#ifdef Q_OS_WIN
    HMODULE m_handle = nullptr;
#else
    void* m_handle = nullptr;
#endif
    QString m_error;
};

/// The directory of the loaded ONNX Runtime library, where it looks for its
/// provider libraries.
QString onnxRuntimeDirectory() {
#ifdef Q_OS_WIN
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&OrtGetApiBase),
                &module)) {
        return QString();
    }
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return QString();
    }
    return QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).absolutePath();
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&OrtGetApiBase), &info) == 0 || !info.dli_fname) {
        return QString();
    }
    return QFileInfo(QFile::decodeName(info.dli_fname)).absolutePath();
#endif
}

/// Free memory of an NVIDIA GPU, from NVML. NVML comes with the driver and,
/// unlike the CUDA runtime, does not create a context on the GPU. NVML counts
/// GPUs in PCI order and CUDA fastest first: they agree on machines with one
/// NVIDIA GPU, unless CUDA_DEVICE_ORDER=PCI_BUS_ID makes them agree always.
std::optional<qint64> nvidiaFreeMemory(int deviceId) {
    // The parts of nvml.h used here.
    struct NvmlMemory {
        unsigned long long total;
        unsigned long long free;
        unsigned long long used;
    };
    struct NvmlDeviceOpaque;
    using NvmlDevice = NvmlDeviceOpaque*;
    using NvmlInit = int (*)();
    using NvmlShutdown = int (*)();
    using NvmlDeviceGetHandleByIndex = int (*)(unsigned int, NvmlDevice*);
    using NvmlDeviceGetMemoryInfo = int (*)(NvmlDevice, NvmlMemory*);
    constexpr int kNvmlSuccess = 0;

    const DynamicLibrary nvml(kNvmlLibrary);
    const auto init = nvml.resolve<NvmlInit>("nvmlInit_v2");
    const auto shutdown = nvml.resolve<NvmlShutdown>("nvmlShutdown");
    const auto getHandle = nvml.resolve<NvmlDeviceGetHandleByIndex>(
            "nvmlDeviceGetHandleByIndex_v2");
    const auto getMemory = nvml.resolve<NvmlDeviceGetMemoryInfo>("nvmlDeviceGetMemoryInfo");
    if (!init || !shutdown || !getHandle || !getMemory || deviceId < 0 ||
            init() != kNvmlSuccess) {
        return std::nullopt;
    }
    std::optional<qint64> freeBytes;
    NvmlDevice device = nullptr;
    NvmlMemory memory{};
    if (getHandle(static_cast<unsigned int>(deviceId), &device) == kNvmlSuccess &&
            getMemory(device, &memory) == kNvmlSuccess) {
        freeBytes = static_cast<qint64>(memory.free);
    }
    shutdown();
    return freeBytes;
}

/// Puts the calling thread in the C locale while it lives. ONNX Runtime and
/// its plugins write numbers with the C library, and Qt gives the process the
/// user's locale. The WebGPU plugin (0.5.0) writes its shaders that way: with
/// a decimal comma (es_ES) the htdemucs graph fails in its first
/// InstanceNormalization (measured in ST4.PR1). Graphs are created and run on
/// the calling thread, so a thread locale is enough and the rest of the
/// process keeps the user's.
///
/// TEST_GAP: only the WebGPU measurement under such a locale shows the bug;
/// the CPU provider does not format numbers.
class ScopedCLocale {
  public:
    ScopedCLocale() {
#ifdef Q_OS_WIN
        m_previousMode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
        const char* const previous = std::setlocale(LC_ALL, nullptr);
        if (previous) {
            m_previous = previous;
        }
        std::setlocale(LC_ALL, "C");
#else
        static const locale_t cLocale = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(0));
        if (cLocale) {
            m_previous = uselocale(cLocale);
        }
#endif
    }

    ~ScopedCLocale() {
#ifdef Q_OS_WIN
        if (!m_previous.empty()) {
            std::setlocale(LC_ALL, m_previous.c_str());
        }
        _configthreadlocale(m_previousMode);
#else
        if (m_previous) {
            uselocale(m_previous);
        }
#endif
    }

    ScopedCLocale(const ScopedCLocale&) = delete;
    ScopedCLocale& operator=(const ScopedCLocale&) = delete;

  private:
#ifdef Q_OS_WIN
    int m_previousMode = 0;
    std::string m_previous;
#else
    locale_t m_previous = static_cast<locale_t>(0);
#endif
};

std::basic_string<ORTCHAR_T> toOrtPath(const QString& path) {
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(path).toStdWString();
#else
    return QFile::encodeName(path).toStdString();
#endif
}

void ORT_API_CALL logOrtMessage(void* pParam,
        OrtLoggingLevel severity,
        const char* pCategory,
        const char* pLogId,
        const char* pCodeLocation,
        const char* pMessage) {
    Q_UNUSED(pParam);
    Q_UNUSED(pCategory);
    Q_UNUSED(pLogId);
    Q_UNUSED(pCodeLocation);
    const QString message = QString::fromUtf8(pMessage);
    switch (severity) {
    case ORT_LOGGING_LEVEL_VERBOSE:
    case ORT_LOGGING_LEVEL_INFO:
        kLogger.debug() << "ONNX Runtime:" << message;
        break;
    case ORT_LOGGING_LEVEL_WARNING:
        kLogger.warning() << "ONNX Runtime:" << message;
        break;
    case ORT_LOGGING_LEVEL_ERROR:
    case ORT_LOGGING_LEVEL_FATAL:
        kLogger.critical() << "ONNX Runtime:" << message;
        break;
    }
}

QString errorText(const std::exception& exception) {
    return QString::fromUtf8(exception.what());
}

} // namespace

QString inferenceProviderName(InferenceProvider provider) {
    switch (provider) {
    case InferenceProvider::Cpu:
        return QStringLiteral("CPU");
    case InferenceProvider::Cuda:
        return QStringLiteral("CUDA");
    case InferenceProvider::DirectMl:
        return QStringLiteral("DirectML");
    case InferenceProvider::CoreMl:
        return QStringLiteral("CoreML");
    case InferenceProvider::WebGpu:
        return QStringLiteral("WebGPU");
    }
    return QString();
}

QStringList InferenceSystem::builtProviders() const {
    QStringList providers;
    try {
        for (const std::string& provider : Ort::GetAvailableProviders()) {
            providers.append(QString::fromStdString(provider));
        }
    } catch (const Ort::Exception& exception) {
        kLogger.warning() << "Cannot list the ONNX Runtime providers:" << errorText(exception);
    }
    return providers;
}

bool InferenceSystem::canLoadLibrary(const QString& fileName, QString* pError) const {
    const DynamicLibrary library(fileName);
    if (!library.isLoaded() && pError) {
        *pError = library.error();
    }
    return library.isLoaded();
}

bool InferenceSystem::hasProviderLibrary(const QString& fileName) const {
    const QString directory = onnxRuntimeDirectory();
    return !directory.isEmpty() && QFileInfo(QDir(directory).filePath(fileName)).isFile();
}

std::optional<qint64> InferenceSystem::freeVideoMemory(
        InferenceProvider provider, int deviceId) const {
    if (provider == InferenceProvider::Cuda) {
        return nvidiaFreeMemory(deviceId);
    }
    // TODO(ST5): DXGI on Windows. CoreML shares the system memory; WebGPU
    // does not tell.
    return std::nullopt;
}

struct InferenceRuntime::Impl {
    /// ONNX Runtime keeps one environment per process; the sessions share it
    /// and keep it alive.
    std::shared_ptr<Ort::Env> pEnv;
    /// The WebGPU device of the plugin library, once registered.
    std::optional<Ort::ConstEpDevice> webGpuPluginDevice;
};

struct InferenceSession::Impl {
    std::shared_ptr<Ort::Env> pEnv;
    Ort::Session session{nullptr};
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    /// Every run uses these. Their terminate flag is atomic in ONNX Runtime,
    /// and the executors check it before each node.
    Ort::RunOptions runOptions;
};

InferenceRuntime::InferenceRuntime(const Options& options, std::shared_ptr<InferenceSystem> pSystem)
        : m_options(options),
          m_pSystem(pSystem ? std::move(pSystem) : std::make_shared<InferenceSystem>()),
          m_pImpl(std::make_unique<Impl>()) {
    m_pImpl->pEnv = std::make_shared<Ort::Env>(
            ORT_LOGGING_LEVEL_WARNING, "crate", &logOrtMessage, nullptr);
}

InferenceRuntime::~InferenceRuntime() = default;

QString InferenceRuntime::onnxRuntimeVersion() {
    return QString::fromStdString(Ort::GetVersionString());
}

QList<InferenceProvider> InferenceRuntime::availableProviders() const {
    QList<Provider> providers;
    for (const Provider provider : kProviders) {
        if (providerStatus(provider).available) {
            providers.append(provider);
        }
    }
    return providers;
}

InferenceProviderStatus InferenceRuntime::providerStatus(Provider provider) const {
    const QMutexLocker locker(&m_mutex);
    for (const InferenceProviderStatus& status : std::as_const(m_statuses)) {
        if (status.provider == provider) {
            return status;
        }
    }
    const InferenceProviderStatus status = detectStatus(provider);
    if (!status.available) {
        kLogger.info() << inferenceProviderName(provider) << "unavailable:" << status.reason;
    }
    m_statuses.append(status);
    return status;
}

InferenceSessionConfig InferenceRuntime::sessionConfig(Provider provider) const {
    InferenceSessionConfig config;
    config.intraOpThreads = m_options.intraOpThreads;
    if (provider == Provider::Cpu) {
        return config;
    }
    // Measured in ST0: the memory pattern costs 2.3 GB of VRAM with the
    // htdemucs graph. DirectML needs it off, and runs only in order.
    config.memoryPattern = false;
    if (provider == Provider::Cuda) {
        // In order, ONNX Runtime keeps the encoder's buffers of htdemucs for
        // the decoder tensors of the same shape, through the whole
        // transformer: 2.2 GB of VRAM at the peak, against 630 MiB with the
        // parallel executor, for about 15% more time (ST4.PR2 export check).
        // With one inter-op thread ONNX Runtime runs in order again.
        // TEST_GAP: only the GPU measurement shows the VRAM.
        config.parallelExecution = true;
        config.interOpThreads = 2;
    }
    return config;
}

InferenceProviderStatus InferenceRuntime::detectStatus(Provider provider) const {
    InferenceProviderStatus status;
    status.provider = provider;
    const QStringList built = m_pSystem->builtProviders();
    switch (provider) {
    case Provider::Cpu:
        status.available = true;
        return status;
    case Provider::Cuda: {
        if (!built.contains(kCudaProviderName)) {
            status.reason = QStringLiteral("this ONNX Runtime build has no CUDA provider");
            return status;
        }
        if (!m_pSystem->hasProviderLibrary(kCudaProviderLibrary)) {
            status.reason = QStringLiteral("%1 is not installed next to ONNX Runtime")
                                    .arg(kCudaProviderLibrary);
            return status;
        }
        for (const QString& library : kCudaSystemLibraries) {
            QString error;
            if (!m_pSystem->canLoadLibrary(library, &error)) {
                status.reason = QStringLiteral(
                        "the system has no %1 (CUDA 13 and cuDNN 9 are needed): %2")
                                        .arg(library, error);
                return status;
            }
        }
        status.available = true;
        return status;
    }
    case Provider::DirectMl:
#ifdef CRATE_INFERENCE_DIRECTML
        status.available = built.contains(kDirectMlProviderName);
        if (!status.available) {
            status.reason = QStringLiteral("this ONNX Runtime build has no DirectML provider");
        }
#else
        status.reason = QStringLiteral("DirectML needs Windows and the DirectML build");
#endif
        return status;
    case Provider::CoreMl:
        status.available = built.contains(kCoreMlProviderName);
        if (!status.available) {
            status.reason = QStringLiteral("this ONNX Runtime build has no CoreML provider");
        }
        return status;
    case Provider::WebGpu: {
        if (built.contains(QString::fromStdString(kWebGpuProviderName))) {
            status.available = true;
            return status;
        }
        if (m_options.providerLibraryDirectory.isEmpty()) {
            status.reason = QStringLiteral("this ONNX Runtime build has no WebGPU provider");
            return status;
        }
        const QString pluginPath =
                QDir(m_options.providerLibraryDirectory).filePath(kWebGpuPluginLibrary);
        if (!QFileInfo(pluginPath).isFile()) {
            status.reason = QStringLiteral("the WebGPU plugin %1 is not installed").arg(pluginPath);
            return status;
        }
        try {
            const ScopedCLocale cLocale;
            // The plugin takes one device. It runs on its default adapter,
            // which on a laptop with an integrated and an NVIDIA GPU is the
            // NVIDIA one whatever device it gets (measured in ST4.PR1), so it
            // gets the discrete GPU when there is one, to match.
            const auto findDevice = [this]() -> std::optional<Ort::ConstEpDevice> {
                std::optional<Ort::ConstEpDevice> found;
                for (const Ort::ConstEpDevice& device : m_pImpl->pEnv->GetEpDevices()) {
                    if (device.EpName() != kWebGpuProviderName ||
                            device.Device().Type() != OrtHardwareDeviceType_GPU) {
                        continue;
                    }
                    const char* const discrete =
                            device.Device().Metadata().GetValue(kDiscreteGpuKey);
                    if (discrete && std::string_view(discrete) == "1") {
                        return device;
                    }
                    if (!found) {
                        found = device;
                    }
                }
                return found;
            };
            // Another runtime of this process may have registered it in the
            // shared environment already.
            m_pImpl->webGpuPluginDevice = findDevice();
            if (!m_pImpl->webGpuPluginDevice) {
                m_pImpl->pEnv->RegisterExecutionProviderLibrary(
                        kWebGpuPluginRegistration, toOrtPath(pluginPath));
                m_pImpl->webGpuPluginDevice = findDevice();
            }
        } catch (const Ort::Exception& exception) {
            status.reason = QStringLiteral("the WebGPU plugin does not load: %1")
                                    .arg(errorText(exception));
            return status;
        }
        status.available = m_pImpl->webGpuPluginDevice.has_value();
        if (!status.available) {
            status.reason = QStringLiteral("WebGPU finds no GPU");
        }
        return status;
    }
    }
    status.reason = QStringLiteral("unknown provider");
    return status;
}

std::unique_ptr<InferenceSession> InferenceRuntime::createSession(const QString& modelPath,
        Provider preferred,
        qint64 requiredVramBytes,
        QString* pError) const {
    Provider provider = preferred;
    QString fallbackReason;
    if (preferred != Provider::Cpu) {
        const QString name = inferenceProviderName(preferred);
        const InferenceProviderStatus status = providerStatus(preferred);
        if (!status.available) {
            fallbackReason = QStringLiteral("%1 is not available: %2").arg(name, status.reason);
            provider = Provider::Cpu;
        } else if (requiredVramBytes > 0) {
            const std::optional<qint64> freeBytes =
                    m_pSystem->freeVideoMemory(preferred, m_options.deviceId);
            if (freeBytes && *freeBytes < requiredVramBytes) {
                fallbackReason = QStringLiteral(
                        "the %1 GPU has %2 MiB of free memory, %3 MiB needed")
                                         .arg(name)
                                         .arg(*freeBytes / kMiB)
                                         .arg(requiredVramBytes / kMiB);
                provider = Provider::Cpu;
            }
        }
    }

    std::optional<Ort::ConstEpDevice> webGpuPluginDevice;
    {
        const QMutexLocker locker(&m_mutex);
        webGpuPluginDevice = m_pImpl->webGpuPluginDevice;
    }
    const auto create = [&](Provider sessionProvider, QString* pCreateError)
            -> std::unique_ptr<InferenceSession> {
        const ScopedCLocale cLocale;
        try {
            const InferenceSessionConfig config = sessionConfig(sessionProvider);
            Ort::SessionOptions options;
            options.SetExecutionMode(config.parallelExecution ? ORT_PARALLEL : ORT_SEQUENTIAL);
            options.SetIntraOpNumThreads(config.intraOpThreads);
            options.SetInterOpNumThreads(config.interOpThreads);
            // Waiting threads sleep instead of spinning: the audio engine
            // needs the CPU more than a separation does.
            options.AddConfigEntry(kOrtSessionOptionsConfigAllowIntraOpSpinning, "0");
            options.AddConfigEntry(kOrtSessionOptionsConfigAllowInterOpSpinning, "0");
            if (!config.memoryPattern) {
                options.DisableMemPattern();
            }
            switch (sessionProvider) {
            case Provider::Cpu:
                break;
            case Provider::Cuda: {
                Ort::CUDAProviderOptions cudaOptions;
                cudaOptions.Update({
                        {"device_id", std::to_string(m_options.deviceId)},
                        {"arena_extend_strategy", "kSameAsRequested"},
                });
                options.AppendExecutionProvider_CUDA_V2(*cudaOptions);
                break;
            }
            case Provider::DirectMl:
#ifdef CRATE_INFERENCE_DIRECTML
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(
                        options, m_options.deviceId));
#endif
                break;
            case Provider::CoreMl:
                options.AppendExecutionProvider("CoreML", {});
                break;
            case Provider::WebGpu:
                if (webGpuPluginDevice) {
                    options.AppendExecutionProvider_V2(*m_pImpl->pEnv,
                            {*webGpuPluginDevice},
                            std::unordered_map<std::string, std::string>());
                } else {
                    options.AppendExecutionProvider("WebGPU", {});
                }
                break;
            }
            auto pImpl = std::make_unique<InferenceSession::Impl>();
            pImpl->pEnv = m_pImpl->pEnv;
            pImpl->session = Ort::Session(*m_pImpl->pEnv, toOrtPath(modelPath).c_str(), options);
            Ort::AllocatorWithDefaultOptions allocator;
            for (size_t i = 0; i < pImpl->session.GetInputCount(); ++i) {
                pImpl->inputNames.emplace_back(
                        pImpl->session.GetInputNameAllocated(i, allocator).get());
            }
            for (size_t i = 0; i < pImpl->session.GetOutputCount(); ++i) {
                pImpl->outputNames.emplace_back(
                        pImpl->session.GetOutputNameAllocated(i, allocator).get());
            }
            return std::unique_ptr<InferenceSession>(
                    new InferenceSession(std::move(pImpl), sessionProvider));
        } catch (const Ort::Exception& exception) {
            *pCreateError = errorText(exception);
            return nullptr;
        }
    };

    if (!QFileInfo(modelPath).isFile()) {
        if (pError) {
            *pError = QStringLiteral("No model file at %1").arg(modelPath);
        }
        return nullptr;
    }
    QString error;
    if (provider != Provider::Cpu) {
        auto pSession = create(provider, &error);
        if (pSession) {
            kLogger.info() << "Running" << modelPath << "on" << inferenceProviderName(provider);
            return pSession;
        }
        fallbackReason = QStringLiteral("%1 failed to start: %2")
                                 .arg(inferenceProviderName(provider), error);
    }
    auto pSession = create(Provider::Cpu, &error);
    if (!pSession) {
        kLogger.warning() << "Cannot load" << modelPath << ':' << error;
        if (pError) {
            *pError = error;
        }
        return nullptr;
    }
    if (fallbackReason.isEmpty()) {
        kLogger.info() << "Running" << modelPath << "on the CPU";
    } else {
        kLogger.info() << "Running" << modelPath << "on the CPU:" << fallbackReason;
        pSession->m_fallbackReason = fallbackReason;
    }
    return pSession;
}

InferenceSession::InferenceSession(std::unique_ptr<Impl> pImpl, InferenceProvider provider)
        : m_pImpl(std::move(pImpl)),
          m_provider(provider) {
}

InferenceSession::~InferenceSession() = default;

QStringList InferenceSession::inputNames() const {
    QStringList names;
    for (const std::string& name : m_pImpl->inputNames) {
        names.append(QString::fromStdString(name));
    }
    return names;
}

QStringList InferenceSession::outputNames() const {
    QStringList names;
    for (const std::string& name : m_pImpl->outputNames) {
        names.append(QString::fromStdString(name));
    }
    return names;
}

bool InferenceSession::run(const std::vector<InferenceInput>& inputs,
        std::vector<InferenceOutput>* pOutputs,
        QString* pError) {
    pOutputs->clear();
    const auto fail = [pError](const QString& error) {
        if (pError) {
            *pError = error;
        }
        return false;
    };
    const ScopedCLocale cLocale;
    std::vector<std::string> inputNames;
    std::vector<Ort::Value> inputValues;
    const Ort::MemoryInfo memoryInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    try {
        for (const InferenceInput& input : inputs) {
            size_t elements = 1;
            for (const int64_t dimension : input.shape) {
                if (dimension < 0) {
                    return fail(
                            QStringLiteral("Input %1 has a negative dimension")
                                    .arg(input.name));
                }
                elements *= static_cast<size_t>(dimension);
            }
            if (elements != input.data.size()) {
                return fail(QStringLiteral("Input %1 has %2 values for %3 in its shape")
                                .arg(input.name)
                                .arg(input.data.size())
                                .arg(elements));
            }
            inputNames.push_back(input.name.toStdString());
            // ONNX Runtime does not write to its inputs.
            inputValues.push_back(Ort::Value::CreateTensor<float>(memoryInfo,
                    const_cast<float*>(input.data.data()),
                    input.data.size(),
                    input.shape.data(),
                    input.shape.size()));
        }
        std::vector<const char*> inputNamePointers;
        for (const std::string& name : inputNames) {
            inputNamePointers.push_back(name.c_str());
        }
        std::vector<const char*> outputNamePointers;
        for (const std::string& name : m_pImpl->outputNames) {
            outputNamePointers.push_back(name.c_str());
        }
        std::vector<Ort::Value> outputValues = m_pImpl->session.Run(m_pImpl->runOptions,
                inputNamePointers.data(),
                inputValues.data(),
                inputValues.size(),
                outputNamePointers.data(),
                outputNamePointers.size());
        std::vector<InferenceOutput> outputs;
        outputs.reserve(outputValues.size());
        for (size_t i = 0; i < outputValues.size(); ++i) {
            const Ort::TensorTypeAndShapeInfo info = outputValues[i].GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                return fail(QStringLiteral("Output %1 is not a float tensor")
                                .arg(QString::fromStdString(m_pImpl->outputNames[i])));
            }
            const float* const pData = outputValues[i].GetTensorData<float>();
            InferenceOutput& output = outputs.emplace_back();
            output.name = QString::fromStdString(m_pImpl->outputNames[i]);
            output.shape = info.GetShape();
            output.data.assign(pData, pData + info.GetElementCount());
        }
        *pOutputs = std::move(outputs);
        return true;
    } catch (const Ort::Exception& exception) {
        return fail(errorText(exception));
    }
}

void InferenceSession::terminate() {
    m_pImpl->runOptions.SetTerminate();
}

void InferenceSession::clearTermination() {
    m_pImpl->runOptions.UnsetTerminate();
}

} // namespace crate
