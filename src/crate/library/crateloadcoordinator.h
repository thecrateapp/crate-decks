#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <functional>
#include <optional>

#include "crate/cache/audiocache.h"
#include "crate/client/crateresult.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "track/trackid.h"

class PlayerInfo;
class TrackCollectionManager;

namespace crate::library {

/// A Crate track, with what the library knows of its file.
struct CrateTrackRef {
    QString serverId;
    QString entityUid;
    cache::TrackHints hints;
};

/// What the Crate library shows of a track, for a library track made before
/// its file is there.
struct CrateTrackTags {
    QString title;
    QString artist;
    QString album;
    QString genre;
    std::optional<qint64> durationMs;
};

/// A Crate track for the Auto DJ queue.
struct CrateQueuedTrack {
    CrateTrackRef track;
    CrateTrackTags tags;
};

/// Loads Crate tracks into decks (design §7.3, plan F4.C-PR3).
///
/// A track whose file is in the audio cache is a track of Mixxx's library
/// at once (cachedTrack()), so it loads like any local one. Any other comes
/// through the asynchronous load hook of the track table and menu
/// (TrackModel::requestTrackLoad): requestLoad() downloads it with the
/// deck's priority while its row shows the download, then loads it into the
/// deck it was meant for. A newer load to the same deck replaces the one
/// still downloading.
///
/// The main thread never reads a file the library does not have yet (design
/// §9: it never blocks over 16 ms). Adding a file reads its tags and decodes,
/// scales and hashes its embedded cover, 40-90 ms for real files (G3,
/// 2026-11-11), so with Options::prepareTrack that happens on a worker and
/// the main thread only writes the library row: the first load of a
/// download, and the first load of a file cached before, go through
/// requestLoad(), and cachedTrack() answers nullptr for such a file while it
/// adds it in the background.
///
/// It never interrupts a playing deck that Mixxx would not interrupt: when
/// the download ends, a deck that started playing meanwhile keeps its track
/// unless the deck preferences allow loading into a playing deck
/// (`[Controls],LoadWhenDeckPlaying`), as for a local track.
///
/// Crate's analysis of a track (its mix profile) is applied the first time
/// the track is taken in each run, before it is loaded. A Crate track that a
/// player loads some other way (Auto DJ, a list of Mixxx's library) takes it
/// once it is loaded, if it has not had it yet in this run.
///
/// "Add to Auto DJ" takes uncached tracks too (F4.C-PR3 decision 1, Diego,
/// 2026-11-11): trackIdsForAutoDj() gives each one a library track for the
/// file the cache will download it to, with Crate's title, artist and
/// duration, so that the queue shows it; Auto DJ waits for that file when the
/// track's turn comes (CratePrefetcher answers its pending track hook). Once
/// cached, the track is that same library track.
///
/// Such a library track has never been read from its file, and the database
/// reads it, tags and cover, the first time it gives the track with the file
/// there: on the main thread, when Auto DJ takes it (60-74 ms, G3,
/// 2026-11-12). With Options::readTrack that read happens on a worker
/// instead: when the download of a file that the library has such a track of
/// starts, the track is taken from the database (the file is not there, so
/// nothing is read) and held in the track cache; once the file is complete,
/// a worker reads it into that track, and whoever takes it from the library
/// meanwhile gets that same track. Then it is let go, which saves it.
class CrateLoadCoordinator : public QObject {
    Q_OBJECT
  public:
    struct Options {
        /// Not owned.
        cache::AudioCache* pCache = nullptr;
        /// For the deck preferences.
        UserSettingsPointer pConfig;
        /// The library's track of a file in the cache:
        /// TrackCollectionManager::getOrAddTrack() in the application.
        std::function<TrackPointer(const QString& path)> trackOfFile;
        /// The library's tracks of files, there or not yet, in their order,
        /// adding the ones it does not have in one go; an invalid id for a
        /// file it does not take. trackIdsOfFilesIn() in the application.
        std::function<QList<TrackId>(const QStringList& paths)> trackIdsOfFiles;
        /// TrackCollectionManager::getTrackById() in the application.
        std::function<TrackPointer(TrackId trackId)> trackById;
        /// The library's track of a file, from the track cache or the
        /// database, never adding it: TrackCollectionManager::getTrackByRef()
        /// in the application. Optional, with prepareTrack.
        std::function<TrackPointer(const QString& path)> trackInLibrary;
        /// Reads a file the library does not have yet into the track cache,
        /// on a worker, before trackOfFile adds it on the main thread:
        /// prepareTrackForLibrary() in the application. Optional; without it
        /// trackOfFile reads the file itself.
        std::function<TrackPointer(const QString& path)> prepareTrack;
        /// Reads a library track that has never been read from its file,
        /// on a worker: readLibraryTrackFromFile() in the application.
        /// Optional, with trackInLibrary and prepareTrack.
        std::function<void(const TrackPointer& pTrack)> readTrack;
        /// Applies Crate's analysis of a track to it (ProfileStore and
        /// AnalysisImporter in the application); it may finish later.
        /// Optional.
        std::function<void(const TrackPointer& pTrack,
                const QString& serverId,
                const QString& entityUid)>
                applyAnalysis;
        /// The players' tracks, to apply the analysis to Crate tracks loaded
        /// some other way. Optional; not owned.
        PlayerInfo* pPlayerInfo = nullptr;
    };

    /// Options::trackIdsOfFiles for the library of `pCollection`: one
    /// transaction adds the missing files, whose tags cannot be read yet.
    static std::function<QList<TrackId>(const QStringList& paths)> trackIdsOfFilesIn(
            TrackCollectionManager* pCollection);

    explicit CrateLoadCoordinator(Options options, QObject* parent = nullptr);
    ~CrateLoadCoordinator() override;

    cache::AudioCache* cache() const {
        return m_pCache;
    }

    /// The library's track of a cached Crate track; nullptr if its file is
    /// not in the cache. Never starts a download: TrackModel::getTrack()
    /// also runs when rows are selected and menus are built.
    /// Also nullptr for a cached file the library does not have yet: it
    /// adds that one off the main thread, and has it the next time.
    TrackPointer cachedTrack(const QString& serverId, const QString& entityUid);
    bool isCached(const QString& serverId, const QString& entityUid) const;
    /// Whether cachedTrack() has the track now: it is cached and its file
    /// is in the library. Otherwise a load goes through requestLoad().
    bool loadsAtOnce(const QString& serverId, const QString& entityUid) const;

    /// Loads the track into `group` once its file is in the cache; an empty
    /// group is the first stopped deck, as a double click loads. Replaces a
    /// load to the same group that is still downloading.
    void requestLoad(const CrateTrackRef& track, const QString& group, bool play);
    /// Whether a load to `group` waits for its download.
    bool isLoadPending(const QString& group) const;

    /// Downloads the tracks without loading them, pinned for `pinReason`
    /// (such as "offline-set:<id>") unless it is empty.
    void download(const QList<CrateTrackRef>& tracks,
            cache::Priority priority,
            const QString& pinReason = QString());
    /// Deletes the cached file of a track. Returns whether there was one.
    bool removeFromCache(const QString& serverId, const QString& entityUid);

    /// The library tracks of `tracks` for the Auto DJ queue, in their order
    /// (TrackModel::getTrackIdsForAutoDJ): a cached track's, or a new one for
    /// the file its download will write, with `tags` for what its file
    /// cannot tell yet. Leaves out a track whose audio format is unknown
    /// (TrackHints::format empty, as after a DJ search): its file has no
    /// name before it downloads. The analysis waits for the load.
    QList<TrackId> trackIdsForAutoDj(const QList<CrateQueuedTrack>& tracks);

    /// Whether the deck preferences let a load into `group` go ahead now.
    /// The preview deck always takes it; a playing sampler never.
    static bool mayLoadInto(const QString& group, const UserSettingsPointer& pConfig);

  signals:
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);
    /// A load whose group was empty: into the first stopped deck.
    void loadTrackIntoNextAvailableDeck(TrackPointer pTrack);
    /// The download of a track meant for a deck failed.
    void loadFailed(const QString& serverId, const QString& entityUid, const crate::Error& error);

  private:
    struct PendingLoad {
        CrateTrackRef track;
        bool play = false;
        QPointer<cache::CacheTicket> pTicket;
        /// Once the file is there: the number of the wait for its library
        /// track (whenInLibrary()), so that a newer load can replace it.
        quint64 wait = 0;
    };

    using LibraryTrackCallback = std::function<void(const TrackPointer& pTrack)>;

    /// A file read into the track cache on a worker, before the library
    /// adds it.
    struct Preparation {
        QFutureWatcher<TrackPointer>* pWatcher = nullptr;
        QList<LibraryTrackCallback> callbacks;
    };

    static QString keyOf(const QString& serverId, const QString& entityUid);
    /// The library's track of `path`, with Crate's analysis applied once.
    TrackPointer trackOf(const QString& path, const QString& serverId, const QString& entityUid);
    /// Applies Crate's analysis to the track, if it has not had it in this
    /// run.
    void analyseOnce(const TrackPointer& pTrack, const QString& serverId, const QString& entityUid);
    void onPlayerTrackChanged(const TrackPointer& pTrack);
    void onReady(const QString& group, cache::CacheTicket* pTicket, const QString& path);
    /// Loads the track of a group whose file is in the library now.
    void finishLoad(const QString& group, const PendingLoad& load, const TrackPointer& pTrack);
    void cancel(const QString& group);
    /// The library's track of a file it has; nullptr while it does not, or
    /// while the file is read for it.
    TrackPointer libraryTrackOf(const QString& path) const;
    /// Calls `callback` with the library's track of the cached file `path`
    /// (nullptr if the library does not take it): at once if the library
    /// has it, else once a worker has read the file and the library has
    /// added it. Each file is read once, however many wait for it.
    void whenInLibrary(const QString& path, LibraryTrackCallback callback);
    void onPrepared(const QString& path);
    /// AudioCache::trackChanged(): holds the library track of a download
    /// that starts, if the library has one that was never read from its
    /// file, and reads it once the file is complete.
    void onCacheTrackChanged(const QString& serverId, const QString& entityUid);
    /// Lets go of the unread library track of `key`, if one is held.
    void releaseUnread(const QString& key);

    const QPointer<cache::AudioCache> m_pCache;
    const UserSettingsPointer m_pConfig;
    const std::function<TrackPointer(const QString& path)> m_trackOfFile;
    const std::function<QList<TrackId>(const QStringList& paths)> m_trackIdsOfFiles;
    const std::function<TrackPointer(TrackId trackId)> m_trackById;
    const std::function<TrackPointer(const QString& path)> m_trackInLibrary;
    const std::function<TrackPointer(const QString& path)> m_prepareTrack;
    const std::function<void(const TrackPointer& pTrack)> m_readTrack;
    const std::function<void(const TrackPointer&, const QString&, const QString&)> m_applyAnalysis;
    /// By group; "" for the first stopped deck.
    QHash<QString, PendingLoad> m_pending;
    quint64 m_lastWait = 0;
    /// By file.
    QHash<QString, Preparation> m_preparing;
    /// Cached files the library has had in this run.
    mutable QSet<QString> m_inLibrary;
    /// Reads files for the library: one per deck at a time.
    QThreadPool m_preparePool;
    /// Tracks whose analysis was applied in this run.
    QSet<QString> m_analysed;
    /// Library tracks of uncached files that have Crate's tags, checked in
    /// this run.
    QSet<TrackId> m_tagged;
    /// Library tracks never read from their file, held from the start of
    /// their download until a worker has read them, by file.
    QHash<QString, TrackPointer> m_unread;
    /// Their files, by track key.
    QHash<QString, QString> m_unreadPaths;
    /// Downloads under way whose start was handled, by track key.
    QSet<QString> m_downloadsSeen;
};

} // namespace crate::library
