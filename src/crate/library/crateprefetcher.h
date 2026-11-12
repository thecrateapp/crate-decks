#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <chrono>
#include <functional>

#include "crate/cache/audiocache.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class PlaylistDAO;

namespace crate::library {

/// Keeps the next tracks of Auto DJ in the audio cache (design §7.4, plan
/// F4.C-PR3).
///
/// Whenever the Auto DJ queue (the playlist of type PLHT_AUTO_DJ) changes,
/// its first tracks are read off the main thread and the cache is asked for
/// those whose file is a Crate one that is not there, with Auto DJ's
/// priority. A track that leaves those places stops its download.
///
/// It also answers Auto DJ's pending track hook
/// (AutoDJProcessor::setPendingTrackHook) while it lives: a queued Crate
/// track whose file is missing (evicted, or never cached: "Add to Auto DJ"
/// takes uncached tracks) is on its way while the cache downloads it.
/// It does so also while the prefetch is off (setEnabled(false), the "Prefetch
/// Auto DJ" preference): Auto DJ then waits for each Crate track as it comes.
class CratePrefetcher : public QObject {
    Q_OBJECT
  public:
    struct Options {
        /// Not owned.
        cache::AudioCache* pCache = nullptr;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// Not owned; its signals say when the queue changes.
        PlaylistDAO* pPlaylistDao = nullptr;
        /// The places of the queue that are kept in the cache.
        int count = 3;
        /// The changes of the queue that come together are read once.
        std::chrono::milliseconds settleTime{250};
        /// Whether to answer Auto DJ's pending track hook.
        bool answerPendingTrackHook = true;
        /// Whether the queue's first places are downloaded ahead.
        bool enabled = true;
        /// Milliseconds since the epoch. Defaults to the clock.
        std::function<qint64()> nowMs;
    };

    explicit CratePrefetcher(Options options, QObject* parent = nullptr);
    ~CratePrefetcher() override;

    /// Reads the queue again soon.
    void scheduleRefresh();

    /// Off, the queue's downloads stop and the queue is not read; on, it is
    /// read again.
    void setEnabled(bool enabled);
    bool isEnabled() const {
        return m_enabled;
    }

    /// Whether the missing file at `location` is a Crate track on its way:
    /// the cache downloads it now (starting the download if needed). A track
    /// whose download failed is not, for a minute.
    bool isOnItsWay(const QString& location);

    /// The Crate tracks of the queue's first places that are on their way,
    /// as "<server>/<uid>".
    QStringList prefetchedTracks() const;

  signals:
    /// The queue was read and its downloads asked for.
    void refreshed();

  private:
    void refresh();
    void apply(const QStringList& locations);
    /// Asks the cache for the track of `location`, if it is a Crate one.
    /// Returns its key, or empty.
    QString ensure(const QString& location, QHash<QString, QPointer<cache::CacheTicket>>* pTickets);
    qint64 now() const;

    const QPointer<cache::AudioCache> m_pCache;
    const mixxx::DbConnectionPoolPtr m_pDbConnectionPool;
    const Options m_options;
    QThreadPool m_pool;
    QTimer m_settleTimer;
    /// Downloads of the queue's first places, by key.
    QHash<QString, QPointer<cache::CacheTicket>> m_tickets;
    /// Downloads Auto DJ waits for, by key.
    QHash<QString, QPointer<cache::CacheTicket>> m_pendingTickets;
    /// When the download of a track failed, by key.
    QHash<QString, qint64> m_failedAtMs;
    bool m_hookInstalled = false;
    bool m_enabled = true;
};

} // namespace crate::library
