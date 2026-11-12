#include "crate/stems/appstemseparator.h"

#include "crate/inference/modelstore.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::AppStemSeparator");

} // namespace

AppStemSeparator::AppStemSeparator(Options options, QObject* parent)
        : QObject(parent),
          m_options(std::move(options)) {
    if (m_options.pModels) {
        connect(m_options.pModels,
                &ModelStore::downloadFinished,
                this,
                &AppStemSeparator::onModelDownloaded);
    }
}

AppStemSeparator::~AppStemSeparator() {
    m_finished = nullptr;
    m_pSeparator.reset();
}

QString AppStemSeparator::modelPath() const {
    return m_options.pModels ? m_options.pModels->installedPath(kHtdemucsModel) : QString();
}

bool AppStemSeparator::isAvailable() const {
    if (!m_options.pModels) {
        return false;
    }
    if (!modelPath().isEmpty()) {
        return true;
    }
    // Downloadable, unless the user said no in this run
    for (const ModelManifestEntry& entry : m_options.pModels->manifest()) {
        if (entry.name == kHtdemucsModel) {
            return !m_declined;
        }
    }
    return false;
}

void AppStemSeparator::separate(const TrackPointer& pTrack,
        std::shared_ptr<StemSink> pSink,
        std::function<void(bool done, const QString& error)> finished) {
    Request request{pTrack, std::move(pSink), std::move(finished)};
    if (!modelPath().isEmpty()) {
        run(std::move(request));
        return;
    }
    if (m_declined || !m_options.pModels) {
        request.finished(false, QStringLiteral("No separation model"));
        return;
    }
    m_waiting = std::move(request);
    if (m_asking || m_options.pModels->isDownloading(kHtdemucsModel)) {
        return;
    }
    qint64 bytes = 0;
    for (const ModelManifestEntry& entry : m_options.pModels->manifest()) {
        if (entry.name == kHtdemucsModel) {
            bytes = entry.size;
        }
    }
    m_asking = true;
    const auto answer = [this](bool accepted) {
        m_asking = false;
        if (!accepted) {
            m_declined = true;
            if (m_waiting) {
                const auto finished = std::move(m_waiting->finished);
                m_waiting.reset();
                finished(false, QStringLiteral("The separation model was not downloaded"));
            }
            return;
        }
        kLogger.info() << "Downloading the separation model";
        m_options.pModels->download(kHtdemucsModel);
    };
    if (m_options.confirmDownload) {
        m_options.confirmDownload(bytes, answer);
    } else {
        // Nobody to ask: never download silently
        answer(false);
    }
}

void AppStemSeparator::onModelDownloaded(
        const QString& name, const QString& path, const QString& error) {
    if (name != kHtdemucsModel || !m_waiting) {
        return;
    }
    Request request = std::move(*m_waiting);
    m_waiting.reset();
    if (path.isEmpty()) {
        kLogger.warning() << "Cannot download the separation model:" << error;
        request.finished(false, error);
        return;
    }
    run(std::move(request));
}

InferenceProvider AppStemSeparator::provider() const {
    if (m_options.useGpu && !m_options.useGpu()) {
        return InferenceProvider::Cpu;
    }
    if (!m_pRuntime) {
        return InferenceProvider::Cpu;
    }
    const QList<InferenceProvider> providers = m_pRuntime->availableProviders();
    for (const InferenceProvider provider : providers) {
        if (provider != InferenceProvider::Cpu) {
            return provider;
        }
    }
    return InferenceProvider::Cpu;
}

void AppStemSeparator::run(Request request) {
    if (!m_pSeparator) {
        if (!m_options.modelFactory && !m_pRuntime) {
            m_pRuntime = std::make_unique<InferenceRuntime>();
        }
        const QString path = modelPath();
        const InferenceProvider preferred = provider();
        auto factory = m_options.modelFactory;
        InferenceRuntime* pRuntime = m_pRuntime.get();
        m_pSeparator = std::make_unique<StemSeparator>(
                [factory, pRuntime, path, preferred](QString* pError) {
                    if (factory) {
                        return factory(path, preferred, pError);
                    }
                    return createHtdemucsModel(*pRuntime, path, preferred, pError);
                });
        connect(m_pSeparator.get(),
                &StemSeparator::finished,
                this,
                [this](StemSeparator::JobId id, const StemSeparationResult& result) {
                    if (id != m_job || !m_finished) {
                        return;
                    }
                    const auto finished = std::move(m_finished);
                    m_finished = nullptr;
                    m_job = 0;
                    finished(result.status == StemSeparationResult::Status::Done, result.error);
                });
    }
    m_finished = std::move(request.finished);
    m_job = m_pSeparator->separate(request.pTrack, std::move(request.pSink));
}

void AppStemSeparator::cancel() {
    m_waiting.reset();
    if (m_pSeparator && m_job != 0) {
        m_pSeparator->cancel(m_job);
    }
    m_job = 0;
    m_finished = nullptr;
}

} // namespace crate

#include "moc_appstemseparator.cpp"
