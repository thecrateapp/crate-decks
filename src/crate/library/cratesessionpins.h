#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <chrono>

#include "crate/cache/audiocache.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class PlayerInfo;
class PlaylistDAO;

namespace crate::library {

/// Keeps the Crate tracks in use in the audio cache for the session (design
/// §9: the LRU never evicts what is loaded or queued).
///
/// Every Crate track loaded in a player (decks, samplers, preview deck) is
/// pinned while it is there, and every one in the Auto DJ queue (the
/// playlist of type PLHT_AUTO_DJ) while it is queued, also before its file
/// is cached. The pins are session pins (AudioCache::kSessionPinPrefix):
/// lowering the cache's limit, "Clear cache" and the LRU before a download
/// leave those files alone, and nothing of them is left after a restart. A
/// track is unpinned when it is ejected or replaced, or leaves the queue.
///
/// The queue is read off the main thread whenever it changes.
class CrateSessionPins : public QObject {
    Q_OBJECT
  public:
    struct Options {
        /// Not owned.
        cache::AudioCache* pCache = nullptr;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// Not owned; its signals say when the queue changes.
        PlaylistDAO* pPlaylistDao = nullptr;
        /// The players' tracks. Not owned.
        PlayerInfo* pPlayerInfo = nullptr;
        /// The changes of the queue that come together are read once.
        std::chrono::milliseconds settleTime{250};
    };

    /// The reason of a track loaded in `group`, and of a queued one.
    static QString playerReason(const QString& group);
    static const QString kQueueReason;

    explicit CrateSessionPins(const Options& options, QObject* parent = nullptr);
    ~CrateSessionPins() override;

    /// Reads the queue again soon.
    void scheduleRefresh();

    /// The Crate tracks pinned for being in a player or in the queue, as
    /// "<server>/<uid>", sorted.
    QStringList pinnedTracks() const;

  signals:
    /// The queue was read and its pins set.
    void refreshed();

  private:
    void onPlayerTrackChanged(const QString& group, const TrackPointer& pTrack);
    void refresh();
    void applyQueue(const QStringList& locations);

    const QPointer<cache::AudioCache> m_pCache;
    const mixxx::DbConnectionPoolPtr m_pDbConnectionPool;
    QThreadPool m_pool;
    QTimer m_settleTimer;
    /// The Crate track of each player that has one.
    QHash<QString, cache::TrackKey> m_players;
    /// The Crate tracks of the queue, by "<server>/<uid>".
    QHash<QString, cache::TrackKey> m_queued;
};

} // namespace crate::library
