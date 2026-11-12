#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
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
    TrackPointer cachedTrack(const QString& serverId, const QString& entityUid);
    bool isCached(const QString& serverId, const QString& entityUid) const;

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
    };

    static QString keyOf(const QString& serverId, const QString& entityUid);
    /// The library's track of `path`, with Crate's analysis applied once.
    TrackPointer trackOf(const QString& path, const QString& serverId, const QString& entityUid);
    /// Applies Crate's analysis to the track, if it has not had it in this
    /// run.
    void analyseOnce(const TrackPointer& pTrack, const QString& serverId, const QString& entityUid);
    void onPlayerTrackChanged(const TrackPointer& pTrack);
    void onReady(const QString& group, cache::CacheTicket* pTicket, const QString& path);
    void cancel(const QString& group);

    const QPointer<cache::AudioCache> m_pCache;
    const UserSettingsPointer m_pConfig;
    const std::function<TrackPointer(const QString& path)> m_trackOfFile;
    const std::function<QList<TrackId>(const QStringList& paths)> m_trackIdsOfFiles;
    const std::function<TrackPointer(TrackId trackId)> m_trackById;
    const std::function<void(const TrackPointer&, const QString&, const QString&)> m_applyAnalysis;
    /// By group; "" for the first stopped deck.
    QHash<QString, PendingLoad> m_pending;
    /// Tracks whose analysis was applied in this run.
    QSet<QString> m_analysed;
    /// Library tracks of uncached files that have Crate's tags, checked in
    /// this run.
    QSet<TrackId> m_tagged;
};

} // namespace crate::library
