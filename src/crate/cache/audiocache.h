#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "crate/cache/audiocacheindex.h"
#include "crate/client/crateresult.h"

namespace crate {
class Client;
class Reply;
} // namespace crate

namespace crate::cache {

class AudioCache;

/// Who needs a track, from the most urgent: a deck being loaded, then the
/// next tracks of AutoDJ, then the prefetch, then an offline set.
enum class Priority {
    OfflineSet = 0,
    Prefetch = 1,
    AutoDj = 2,
    Deck = 3,
};

/// What the library knows of a track (`crateserver_tracks`). Tracks found
/// only through the DJ search have none of it: the cache then goes by the
/// stream's Content-Type and Content-Length.
struct TrackHints {
    /// The server's `format`, for the file extension.
    QString format;
    /// The server's `byte_length`: the expected size when the stream does
    /// not send Content-Length.
    std::optional<qint64> byteLength;
    /// The server's `source_revision`: a cached file of another revision is
    /// downloaded again.
    QString sourceRevision;
};

/// The free space of a volume. Tests replace it.
class StorageInfo {
  public:
    virtual ~StorageInfo() = default;
    /// Bytes this user can still write on the volume of `directory`;
    /// negative if unknown.
    virtual qint64 bytesAvailable(const QString& directory) const = 0;
};

/// What the cache holds and does, for its view (F4.C-PR4).
struct Usage {
    /// Complete files, and their size.
    int readyFiles = 0;
    qint64 readyBytes = 0;
    /// The part of them that is pinned to stay, such as offline sets: what
    /// the cache view shows as kept offline.
    qint64 pinnedBytes = 0;
    /// The part of them that only session pins keep (kSessionPinPrefix):
    /// tracks in a player or in the Auto DJ queue.
    qint64 inUseBytes = 0;
    int activeDownloads = 0;
    int queuedDownloads = 0;
};

/// Where a track's audio is, for the track tables (plan F4.C-PR3).
struct TrackState {
    enum class Kind {
        /// Not in the cache, and nobody asked for it.
        None,
        /// Waiting for its turn to download.
        Queued,
        Downloading,
        Ready,
        /// The last download failed.
        Failed,
    };
    Kind kind = Kind::None;
    /// Bytes of the file there are so far, while it downloads.
    qint64 bytesReceived = 0;
    /// The size of the whole file; -1 if unknown.
    qint64 bytesTotal = -1;
};

/// A download of the cache's queue, for its view (F4.C-PR4).
struct DownloadInfo {
    QString serverId;
    QString entityUid;
    Priority priority = Priority::Prefetch;
    /// On its way; otherwise waiting for its turn.
    bool active = false;
    /// Bytes of the file there are so far.
    qint64 bytesReceived = 0;
    /// The size of the whole file; -1 if unknown.
    qint64 bytesTotal = -1;
};

/// The tracks of a server that one pin reason keeps, such as an offline set
/// ("offline-set:<kind>:<id>"), for the cache's view.
struct PinnedSet {
    QString serverId;
    QString reason;
    int tracks = 0;
    /// The ones whose file is complete, and their size.
    int readyTracks = 0;
    qint64 readyBytes = 0;
};

/// A track of the cache.
struct TrackKey {
    QString serverId;
    QString entityUid;
};

/// One caller's interest in a track. AudioCache::ensure() returns it; it
/// emits ready() or failed() once, always after ensure() has returned, and
/// then deletes itself (deleteLater). Deleting it earlier, or cancel(),
/// drops the interest without a signal: a download nobody waits for any more
/// stops, and its part file stays to resume.
class CacheTicket : public QObject {
    Q_OBJECT
  public:
    ~CacheTicket() override;

    const QString& serverId() const {
        return m_serverId;
    }
    const QString& entityUid() const {
        return m_entityUid;
    }
    Priority priority() const {
        return m_priority;
    }

    void cancel();

  signals:
    /// Bytes of the file there are so far, of `bytesTotal` (-1 if unknown).
    void progress(qint64 bytesReceived, qint64 bytesTotal);
    /// The complete file, an absolute path.
    void ready(const QString& path);
    void failed(const crate::Error& error);

  private:
    friend class AudioCache;
    CacheTicket(AudioCache* pCache,
            const QString& serverId,
            const QString& entityUid,
            Priority priority,
            const TrackHints& hints);
    void finishReady(const QString& path);
    void finishFailed(const crate::Error& error);

    QPointer<AudioCache> m_pCache;
    const QString m_serverId;
    const QString m_entityUid;
    const Priority m_priority;
    const TrackHints m_hints;
    bool m_finished = false;
};

/// The local copy of Crate tracks that the decks play (design D4).
///
/// Each track is downloaded whole from `GET /api/tracks/by-entity/{uid}/stream`
/// (the original file), through the server's crate::Client and its
/// Authorization header, into `<cache>/<server_id>/<uid[0:2]>/<uid>.<ext>`
/// (cachepaths.h):
/// - Downloads wait in a queue by Priority, then by order of arrival; each
///   server has at most Options::maxDownloadsPerServer at the same time.
///   Asking again for a track that is on its way shares its download.
/// - The body goes to `<path>.part`, then a length check against
///   Content-Length (or `byte_length`), then an atomic rename. A download
///   that stops leaves its part file, and the next one resumes it with
///   `Range: bytes=<n>-` and `If-Range: <ETag>`: a changed file comes whole.
///   A transfer that breaks off after receiving data resumes at once, up to
///   Options::maxAttempts times.
/// - Before a file comes in, the least recently used unpinned files go until
///   it fits in the limit (20 GiB by default) and Options::minFreeBytes stay
///   free on the volume. Pinned files (offline sets, and for the session
///   the tracks in a player or in the Auto DJ queue) are never evicted: they
///   can take the cache over its limit, but never below the free space,
///   which fails the download instead. What the other downloads under way
///   still have to write counts as taken for both, so that two at once
///   never break the free space between them.
/// - The index (`<cache>/index.sqlite`, AudioCacheIndex) and every file
///   operation run on a worker thread of its own; the main thread only
///   hands over the chunks that arrive. At start the index is matched with
///   the files (AudioCacheIndex::reconcile()); requests made before that
///   wait for it.
class AudioCache : public QObject {
    Q_OBJECT
  public:
    static constexpr qint64 kGiB = qint64{1} << 30;

    struct Options {
        /// The cache directory. defaultDirectory() is the proposed default;
        /// the preferences choose it from F4.C-PR4.
        QString directory;
        /// The Client of a server; nullptr if there is none (any more).
        /// CrateServers::client() in the application.
        std::function<Client*(const QString& serverId)> clientFor;
        qint64 limitBytes = 20 * kGiB;
        /// Never go below this much free space on the cache's volume (§9).
        qint64 minFreeBytes = 2 * kGiB;
        int maxDownloadsPerServer = 2;
        /// Attempts of a download whose transfer breaks off, the first
        /// included.
        int maxAttempts = 3;
        /// Defaults to QStorageInfo.
        std::shared_ptr<StorageInfo> pStorageInfo;
        /// Milliseconds since the epoch, for the LRU. Defaults to the clock.
        std::function<qint64()> nowMs;
        /// The files that belong to a cached track beside its file (its stem
        /// sidecar, STEM-D9), by the track's absolute path; none by default.
        /// They count in the track's size for the limit and the free space,
        /// and go when it goes. Called on the cache's worker thread too.
        std::function<QStringList(const QString& trackPath)> companionsOf;
    };

    /// `<the app's cache location>/crate-audio`, e.g.
    /// ~/.cache/Crate/Crate Decks/crate-audio on Linux.
    static QString defaultDirectory();

    explicit AudioCache(Options options, QObject* parent = nullptr);
    ~AudioCache() override;

    const QString& directory() const {
        return m_options.directory;
    }
    /// Whether the index has been read and matched with the files.
    bool isLoaded() const {
        return m_loaded;
    }

    /// Makes sure the track is in the cache: at once if it is, else after
    /// its download. The ticket belongs to the cache.
    CacheTicket* ensure(const QString& serverId,
            const QString& entityUid,
            Priority priority,
            const TrackHints& hints = TrackHints());

    /// The complete file of a track, if the cache has it. Never starts a
    /// download (for TrackModel::getTrack(), F4.C-PR3).
    std::optional<QString> cachedPath(const QString& serverId, const QString& entityUid) const;

    /// The file of a track whether it is there or not: its cached file, else
    /// the one its download writes, for a download that names the track's
    /// `format` (TrackHints::format). A library track made with it now is the
    /// track of that file once it comes (F4.C-PR3 decision 1). nullopt if the
    /// format names no extension the decoders know, or for an id that cannot
    /// be part of a path.
    std::optional<QString> filePathFor(
            const QString& serverId, const QString& entityUid, const QString& format) const;

    /// Where the track's audio is: cached, on its way or neither.
    TrackState trackState(const QString& serverId, const QString& entityUid) const;

    /// The track whose cached file `path` is (absolute, or relative to the
    /// cache directory), whether the file is there or not; nullopt for any
    /// other path.
    std::optional<TrackKey> trackOfFile(const QString& path) const;

    /// Deletes the track's file and its pins, for the "Remove from cache"
    /// entry of the track menu; its session pins stay while the track is in
    /// use. A track that is downloading stays. Returns whether there was a
    /// file to delete.
    bool remove(const QString& serverId, const QString& entityUid);

    /// Pins whose reason starts with this last for this run only: they are
    /// kept in memory, never in the index (CrateSessionPins pins the tracks
    /// in a player or in the Auto DJ queue, design §9).
    static const QString kSessionPinPrefix;

    /// Keeps the track from being evicted while any reason holds, such as
    /// "offline-set:<id>": by the LRU before a download, a lower limit or
    /// clear(). Pins are kept in the index across restarts, also for a track
    /// that is not cached yet, except session pins (kSessionPinPrefix).
    void pin(const QString& serverId, const QString& entityUid, const QString& reason);
    void unpin(const QString& serverId, const QString& entityUid, const QString& reason);

    /// Measures the companion files of a cached track again
    /// (Options::companionsOf), once they were written or changed, and
    /// evicts what the limit leaves out.
    void refreshCompanions(const QString& serverId, const QString& entityUid);

    /// Evicts at once whatever the new limit leaves out.
    void setLimitBytes(qint64 limitBytes);
    qint64 limitBytes() const {
        return m_options.limitBytes;
    }
    Usage usage() const;
    /// The downloads on their way, then the ones waiting, the most urgent
    /// first.
    QList<DownloadInfo> downloads() const;
    /// The tracks pinned for reasons that start with `reasonPrefix`, by
    /// server and reason, in the order of the reasons.
    QList<PinnedSet> pinnedSets(const QString& reasonPrefix) const;
    /// Deletes every complete file that is not pinned, for "Clear cache".
    /// Downloads and pinned files stay. Returns how many files went.
    int clear();

  signals:
    void loaded();
    void usageChanged();
    void evicted(const QString& serverId, const QString& entityUid);
    /// trackState() of the track changed: its download was queued, moved on
    /// or ended, or its file went. Sent for every chunk that arrives.
    void trackChanged(const QString& serverId, const QString& entityUid);

  private:
    friend class CacheTicket;
    struct Worker;
    struct Download;
    using DownloadPtr = std::shared_ptr<Download>;
    using Key = QString;

    static Key keyOf(const QString& serverId, const QString& entityUid);
    qint64 now() const;
    QString absolutePath(const QString& relativePath) const;

    void resolve(CacheTicket* pTicket);
    void detach(CacheTicket* pTicket);
    void pump();
    int activeDownloads(const QString& serverId) const;
    void start(const DownloadPtr& pDownload);
    void request(const DownloadPtr& pDownload, qint64 offset);
    void onHeaders(const DownloadPtr& pDownload, Reply* pReply);
    void onBody(const DownloadPtr& pDownload, const QByteArray& chunk);
    void onFinished(const DownloadPtr& pDownload, const JsonResult& result);
    void onWriteFailed(quint64 downloadId);
    void finalize(const DownloadPtr& pDownload);
    /// Ends a download: its tickets get `error`, or the file if none.
    void finish(const DownloadPtr& pDownload, const std::optional<Error>& error);
    /// Stops a download nobody waits for; its part file stays.
    void stop(const DownloadPtr& pDownload);
    /// Cancels the request of a download, without its answer.
    void dropReply(const DownloadPtr& pDownload);
    void closePart(const DownloadPtr& pDownload);
    void forget(const DownloadPtr& pDownload);
    /// The entry of a track, created as Pending if there is none.
    CacheEntry& entryOf(const QString& serverId, const QString& entityUid);
    /// Evicts until `incomingBytes` more fit in the limit and leave the
    /// minimum free space, with the rest of the other downloads under way
    /// (reservedBytes()) counted as taken. Returns false if the free space
    /// is still short.
    bool makeRoom(qint64 incomingBytes, const Download* pFor = nullptr);
    /// What the downloads writing their part files, but `pExcept`, still
    /// have to write: the size they announced less their part file. Each
    /// gives its share back as it ends, fails or stops.
    qint64 reservedBytes(const Download* pExcept) const;
    /// Whether a pin keeps the file: one of its own, or a session pin.
    bool isKept(const Key& key, const CacheEntry& entry) const;
    void evict(const Key& key);
    void store(const CacheEntry& entry);

    /// Runs `task` on the worker thread, then `done` with its result here.
    template<typename Result, typename Task, typename Done>
    void inWorker(Task&& task, Done&& done);
    /// Runs `task` on the worker thread, after everything handed to it.
    template<typename Task>
    void onWorker(Task&& task);

    Options m_options;
    QThreadPool m_pool;
    std::shared_ptr<Worker> m_pWorker;
    bool m_loaded = false;
    QHash<Key, CacheEntry> m_entries;
    std::vector<DownloadPtr> m_downloads;
    QList<QPointer<CacheTicket>> m_waitingForIndex;
    /// Pins and unpins made before the index was read.
    QList<std::function<void()>> m_afterLoad;
    /// The session pins of each track (kSessionPinPrefix): in memory only,
    /// also for a track the cache does not have.
    QHash<Key, QStringList> m_sessionPins;
    quint64 m_nextDownloadId = 1;
};

} // namespace crate::cache
