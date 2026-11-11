#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <functional>

#include "crate/cache/audiocache.h"
#include "crate/client/crateresult.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

namespace crate::library {

/// A Crate track, with what the library knows of its file.
struct CrateTrackRef {
    QString serverId;
    QString entityUid;
    cache::TrackHints hints;
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
/// the track is taken in each run, before it is loaded.
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
        /// Applies Crate's analysis of a track to it (ProfileStore and
        /// AnalysisImporter in the application); it may finish later.
        /// Optional.
        std::function<void(const TrackPointer& pTrack,
                const QString& serverId,
                const QString& entityUid)>
                applyAnalysis;
    };

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
    void onReady(const QString& group, cache::CacheTicket* pTicket, const QString& path);
    void cancel(const QString& group);

    const QPointer<cache::AudioCache> m_pCache;
    const UserSettingsPointer m_pConfig;
    const std::function<TrackPointer(const QString& path)> m_trackOfFile;
    const std::function<void(const TrackPointer&, const QString&, const QString&)> m_applyAnalysis;
    /// By group; "" for the first stopped deck.
    QHash<QString, PendingLoad> m_pending;
    /// Tracks whose analysis was applied in this run.
    QSet<QString> m_analysed;
};

} // namespace crate::library
