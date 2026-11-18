#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <chrono>
#include <functional>

#include "crate/cache/audiocache.h"
#include "crate/recs/recsjson.h"
#include "track/trackid.h"
#include "util/db/dbconnectionpool.h"

class PlaylistDAO;

namespace crate::library {
struct CrateQueuedTrack;
} // namespace crate::library

namespace crate::recs {

/// Where Radio and Music Paths put their tracks: Auto DJ's queue, and new
/// playlists of this computer's library (plan F5.C-PR2 and F5.C-PR3).
/// AutoDjRecommendationQueue is the application's; tests use their own.
class RecommendationQueue : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;

    /// Appends `tracks` of the server `serverId` to the end of Auto DJ's
    /// queue, in their order, also before their files are downloaded.
    virtual void enqueue(const QString& serverId, const QList<RecommendedTrack>& tracks) = 0;

    /// The Crate tracks in Auto DJ's queue, in its order, read off the main
    /// thread: `done` comes later, on the main thread.
    virtual void readQueue(
            const std::function<void(const QList<cache::TrackKey>& queued)>& done) = 0;

    /// Saves `tracks` as a new playlist named after `name` (made unique);
    /// the name it got, or an empty string if it could not.
    virtual QString saveAsPlaylist(const QString& name,
            const QString& serverId,
            const QList<RecommendedTrack>& tracks) = 0;

  signals:
    /// Auto DJ's queue changed: tracks came, went or moved.
    void queueChanged();
};

/// The application's RecommendationQueue: Auto DJ's queue (the playlist of
/// type PLHT_AUTO_DJ) and the playlists of upstream's PlaylistDAO. A track
/// that is not downloaded yet goes in as Crate's "Add to Auto DJ" puts it
/// (CrateLoadCoordinator::trackIdsForAutoDj()): a library track for the
/// file its download will write, which Auto DJ waits for.
class AutoDjRecommendationQueue : public RecommendationQueue {
    Q_OBJECT
  public:
    struct Options {
        /// Not owned; its signals say when the queue changes.
        PlaylistDAO* pPlaylistDao = nullptr;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// The library tracks of Crate tracks, in their order:
        /// CrateLoadCoordinator::trackIdsForAutoDj() in the application.
        std::function<QList<TrackId>(const QList<library::CrateQueuedTrack>& tracks)> trackIds;
        /// The Crate track of a cached file (cache::AudioCache::trackOfFile).
        std::function<std::optional<cache::TrackKey>(const QString& location)> trackOfFile;
        /// The changes of the queue that come together are told once.
        std::chrono::milliseconds settleTime{250};
    };

    explicit AutoDjRecommendationQueue(Options options, QObject* parent = nullptr);
    ~AutoDjRecommendationQueue() override;

    void enqueue(const QString& serverId, const QList<RecommendedTrack>& tracks) override;
    void readQueue(
            const std::function<void(const QList<cache::TrackKey>& queued)>& done) override;
    QString saveAsPlaylist(const QString& name,
            const QString& serverId,
            const QList<RecommendedTrack>& tracks) override;

  private:
    QList<TrackId> trackIdsOf(const QString& serverId, const QList<RecommendedTrack>& tracks);

    const Options m_options;
    QThreadPool m_pool;
    QTimer m_settleTimer;
};

} // namespace crate::recs
