#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <memory>

#include "crate/inference/inferenceruntime.h"
#include "crate/stems/stemjobs.h"
#include "crate/stems/stemseparator.h"

namespace crate {

class ModelStore;

/// Local separation in the application (STEM-D7): StemSeparator with the
/// htdemucs model of the ModelStore, on the GPU when the preference allows
/// it and one can run it, else on the CPU.
///
/// The model is not in the installer. The first time a track needs it, the
/// user is asked, with its size (Options::confirmDownload), and it is
/// downloaded only on a yes. A no lasts for the run: tracks without server
/// stems then stay without stems. Never downloads silently.
class AppStemSeparator : public QObject, public LocalStemSeparator {
    Q_OBJECT
  public:
    struct Options {
        /// Not owned.
        ModelStore* pModels = nullptr;
        /// Asks whether to download the model of `bytes`.
        std::function<void(qint64 bytes, std::function<void(bool accepted)> answer)>
                confirmDownload;
        /// Whether to separate on the GPU when there is one (preference).
        std::function<bool()> useGpu;
        /// Creates the model from its file, on the separator's thread.
        /// Defaults to createHtdemucsModel() on the runtime; tests replace it.
        std::function<std::unique_ptr<StemModel>(
                const QString& modelPath, InferenceProvider provider, QString* pError)>
                modelFactory;
    };

    explicit AppStemSeparator(Options options, QObject* parent = nullptr);
    ~AppStemSeparator() override;

    bool isAvailable() const override;
    void separate(const TrackPointer& pTrack,
            std::shared_ptr<StemSink> pSink,
            std::function<void(bool done, const QString& error)> finished) override;
    void cancel() override;

  private:
    struct Request {
        TrackPointer pTrack;
        std::shared_ptr<StemSink> pSink;
        std::function<void(bool, const QString&)> finished;
    };

    QString modelPath() const;
    void onModelDownloaded(const QString& name, const QString& path, const QString& error);
    void run(Request request);
    InferenceProvider provider() const;

    const Options m_options;
    std::unique_ptr<InferenceRuntime> m_pRuntime;
    std::unique_ptr<StemSeparator> m_pSeparator;
    /// Waiting for the model.
    std::optional<Request> m_waiting;
    bool m_asking = false;
    bool m_declined = false;
    /// The separation in progress.
    StemSeparator::JobId m_job = 0;
    std::function<void(bool, const QString&)> m_finished;
};

} // namespace crate
