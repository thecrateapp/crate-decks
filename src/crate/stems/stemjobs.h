#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

#include "crate/cache/audiocache.h"
#include "crate/library/crateloadcoordinator.h"
#include "crate/stems/stemsink.h"
#include "crate/stems/stemsync.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

namespace crate {

class Client;

/// Separates tracks into stems on this computer (STEM-D7): StemSeparator
/// with the htdemucs model in the application. Its sink gets the stems on
/// the separator's thread; `finished` comes on the main thread.
class LocalStemSeparator {
  public:
    virtual ~LocalStemSeparator() = default;
    /// Whether it can separate now: the runtime is built in and the model is
    /// installed.
    virtual bool isAvailable() const = 0;
    virtual void separate(const TrackPointer& pTrack,
            std::shared_ptr<StemSink> pSink,
            std::function<void(bool done, const QString& error)> finished) = 0;
    /// Stops the separation in progress, without `finished`.
    virtual void cancel() = 0;
};

/// The stems of Smart Mix playlists and of tracks on demand in Crate Decks
/// (plan ST5.PR2; STEM-D2, D7, D8, D9).
///
/// **Smart Mix playlists** are playlists their owner marks on the server;
/// in Decks they are offline sets (STEM-D2). StemJobs reads them with
/// `GET /api/smart-mix/playlists` (all of them at start, then the changes
/// since the last revision, every Options::pollInterval). For a playlist
/// turned on, it reads its tracks, asks before downloading what is not
/// cached (Options::confirmDownload, with the size), downloads and pins
/// them as the offline set of the playlist ("offline-set:playlist:<id>"),
/// and queues their stems ahead of any first load. A playlist turned off
/// on the server unpins its tracks; their stems stay until the cache
/// evicts them.
///
/// **A track's stems**, one track at a time:
/// 1. its file in the cache (an on-demand track is downloaded);
/// 2. the server's stem file first (StemSync): the sidecar is rebuilt from
///    it against Decks' own decode;
/// 3. otherwise Decks separates it itself (LocalStemSeparator), writing the
///    sidecar and its `stem-analysis/1` at once, and uploads the analysis
///    when the server allows it;
/// 4. the sidecar is recorded in `crateserver_stems` and counts in the
///    cache's size (AudioCache::refreshCompanions()). A result for a source
///    revision that is no longer the track's is discarded.
///
/// **While any deck plays**, nothing starts and a local separation in
/// progress stops and goes back to the front of the queue, unless the
/// preference allows separating while playing (STEM-D7).
///
/// Main thread; database reads and writes on a worker thread.
class StemJobs : public QObject {
    Q_OBJECT
  public:
    using CrateTrackRef = library::CrateTrackRef;

    struct Options {
        /// Not owned.
        cache::AudioCache* pCache = nullptr;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// Not owned.
        StemSync* pSync = nullptr;
        std::function<Client*(const QString& serverId)> clientFor;
        /// Reads the tracks of a server playlist, syncing it first if Decks
        /// does not have it: nullopt if it cannot.
        std::function<void(const QString& serverId,
                qint64 playlistId,
                std::function<void(std::optional<QList<CrateTrackRef>> tracks)> done)>
                readPlaylist;
        /// Asks before downloading the tracks of a Smart Mix playlist that are
        /// not cached: `bytes` of the tracks whose size is known.
        std::function<void(const QString& serverId,
                qint64 playlistId,
                int tracks,
                qint64 bytes,
                std::function<void(bool accepted)> answer)>
                confirmDownload;
        /// Downloads and pins tracks: CrateLoadCoordinator::download() with
        /// the offline set's priority in the application.
        std::function<void(const QList<CrateTrackRef>& tracks, const QString& pinReason)>
                download;
        /// Not owned; nullptr without local separation.
        LocalStemSeparator* pSeparator = nullptr;
        /// Whether separation may run while a deck plays (preference).
        std::function<bool()> separateWhilePlaying;
        /// Whether any deck plays. Defaults to the decks' `play` controls.
        std::function<bool()> anyDeckPlays;
        std::chrono::milliseconds pollInterval = std::chrono::minutes(5);
        /// How often a paused queue looks at the decks again.
        std::chrono::milliseconds pauseCheckInterval = std::chrono::milliseconds(500);
    };

    enum class TrackState {
        None,
        Queued,
        /// Its file, the server's stems or a separation are on their way.
        Working,
        Ready,
        /// No server stems and no local separation.
        Unavailable,
        Failed,
    };

    struct PlaylistState {
        bool enabled = false;
        int tracks = 0;
        int ready = 0;
    };

    /// `offline-set:playlist:<id>`, the pin of a Smart Mix playlist.
    static QString pinReasonOf(qint64 playlistId);

    explicit StemJobs(Options options, QObject* parent = nullptr);
    ~StemJobs() override;

    /// Reads which tracks have stems (`crateserver_stems`), then the Smart
    /// Mix playlists of `serverId`, and keeps reading their changes.
    void startServer(const QString& serverId);
    void stopServer(const QString& serverId);
    /// Reads the changes of the server's Smart Mix playlists now.
    void syncPlaylists(const QString& serverId);

    /// Queues the track's stems ahead of the playlists' (on demand).
    void requestStems(const CrateTrackRef& track);

    TrackState trackState(const QString& serverId, const QString& entityUid) const;
    /// A Smart Mix playlist's state, for its chip in the library; nullopt
    /// for a playlist that is not one.
    std::optional<PlaylistState> playlistState(const QString& serverId, qint64 playlistId) const;
    bool isPaused() const {
        return m_paused;
    }

  signals:
    void trackStateChanged(const QString& serverId, const QString& entityUid);
    void playlistsChanged(const QString& serverId);

  private:
    struct Job {
        CrateTrackRef track;
    };
    struct Playlist {
        bool enabled = false;
        QList<CrateTrackRef> tracks;
    };
    struct Server {
        qint64 revision = -1;
        bool polling = false;
        bool loaded = false;
        QHash<qint64, Playlist> playlists;
        /// Tracks with a recorded sidecar: entity uid to source revision.
        QHash<QString, QString> stems;
    };

    static QString keyOf(const QString& serverId, const QString& entityUid);
    void loadStems(const QString& serverId);
    void onPlaylists(const QString& serverId, const QJsonDocument& document);
    void enablePlaylist(const QString& serverId, qint64 playlistId);
    void disablePlaylist(const QString& serverId, qint64 playlistId);
    void enqueue(const CrateTrackRef& track, bool front);
    void setState(const CrateTrackRef& track, TrackState state);
    bool mustPause() const;
    void pump();
    void runJob();
    void separateLocally(const QString& cachedPath, const QString& sourceRevision);
    void finishJob(TrackState state);
    void recordStems(const CrateTrackRef& track, const QString& sidecarPath, bool uploaded);
    void checkPause();

    const Options m_options;
    QThreadPool m_pool;
    QTimer m_pollTimer;
    QTimer m_pauseTimer;
    QHash<QString, Server> m_servers;
    std::deque<Job> m_queue;
    std::optional<Job> m_running;
    /// The running job waits for its file in the cache.
    QPointer<cache::CacheTicket> m_pTicket;
    /// Separating locally: stopped when a deck starts playing.
    bool m_separating = false;
    quint64 m_generation = 0;
    QHash<QString, TrackState> m_states;
    bool m_paused = false;
};

} // namespace crate
