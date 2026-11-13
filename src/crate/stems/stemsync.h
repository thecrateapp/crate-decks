#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "crate/client/crateresult.h"
#include "crate/stems/stemanalyzer.h"
#include "crate/stems/stemsink.h"

namespace crate {

class Client;

/// Decodes a stem file of the Crate server (STEM-D6: an MP4 with three AAC
/// streams, drums, bass and vocals) into `pSink` with libav: begin() with
/// the file's sample rate, then the stems in order, `other` silent. A
/// stream that ends early is silent for the rest. Blocks: call it on a
/// worker thread. `pCancel` (optional) stops it.
bool decodeServerStems(const QString& path,
        StemSink* pSink,
        const std::atomic<bool>* pCancel,
        QString* pError);

/// The stems of a cached track and the server (plan ST5.PR2, STEM-D6, D8):
/// what the server already has comes first, and Decks' own analysis goes
/// back to it when the server allows it.
///
/// fetch() reads the server's stem analysis of the track and its stem file.
/// With the file, Decks writes the track's sidecar from it, rebuilding
/// `other` as the residual against its own decode of the original
/// (StemSidecarWriter): no local separation is needed. Results for a source
/// revision that is no longer the track's current one are discarded, also
/// when the revision changes while they are made.
///
/// Main thread; the decoding and the sidecar run on a worker thread of its
/// own. Network through crate::Client.
class StemSync : public QObject {
    Q_OBJECT
  public:
    struct Options {
        /// The Client of a server; nullptr if there is none (any more).
        std::function<Client*(const QString& serverId)> clientFor;
        /// Whether the server takes Decks' analyses
        /// (capabilities.decks.features `stem_upload`).
        std::function<bool(const QString& serverId)> mayUpload;
        /// The track's current source revision on the server
        /// (crateserver_tracks), to discard what was made for another one.
        std::function<QString(const QString& serverId, const QString& entityUid)> currentRevision;
    };

    struct Outcome {
        enum class Result {
            /// The sidecar was written from the server's stem file.
            ServerStems,
            /// The server has no stem file of the track's current file:
            /// Decks separates it itself, if it can.
            NoServerStems,
            /// The track's source revision changed meanwhile: nothing kept.
            Discarded,
            Failed,
        };
        Result result = Result::Failed;
        /// The server's analysis of the track's current file, if any.
        std::optional<StemAnalysis> serverAnalysis;
        QString sidecarPath;
        QString error;
    };

    enum class UploadResult {
        /// Stored or replaced.
        Stored,
        /// The server keeps a stronger analysis, or an equal one of its own.
        KeptStronger,
        /// The server does not take uploads (no `stem_upload`), or the
        /// analysis is for another revision.
        NotUploaded,
        Failed,
    };

    explicit StemSync(Options options, QObject* parent = nullptr);
    ~StemSync() override;

    /// `cachedPath` is the track's file in the audio cache;
    /// `sourceRevision` the revision it was downloaded for.
    void fetch(const QString& serverId,
            const QString& entityUid,
            const QString& cachedPath,
            const QString& sourceRevision,
            std::function<void(const Outcome&)> done);

    /// Uploads an analysis Decks made of the track, if the server allows it
    /// and the analysis is still for the track's current file.
    void upload(const QString& serverId,
            const QString& entityUid,
            const StemAnalysis& analysis,
            std::function<void(UploadResult)> done);

    /// Whether `sourceRevision` is the track's current one.
    bool isCurrent(const QString& serverId,
            const QString& entityUid,
            const QString& sourceRevision) const;

  private:
    struct Fetch;
    using FetchPtr = std::shared_ptr<Fetch>;

    void fetchAnalysis(const FetchPtr& pFetch);
    void fetchStemFile(const FetchPtr& pFetch);
    void buildSidecar(const FetchPtr& pFetch);
    void finish(const FetchPtr& pFetch, Outcome outcome);

    const Options m_options;
    QThreadPool m_pool;
    std::atomic<bool> m_stopping = false;
};

} // namespace crate
