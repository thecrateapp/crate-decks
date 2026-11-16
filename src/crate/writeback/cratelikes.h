#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include "crate/cache/audiocache.h"
#include "util/db/dbconnectionpool.h"

namespace crate::writeback {

class Outbox;

/// The Crate tracks the user likes, and the likes Decks sends (plan
/// F7.C-PR2): `POST /api/me/likes` to like a track and `DELETE
/// /api/me/likes` to stop liking it, through the Outbox.
///
/// What the server says comes from its Likes container as Decks synced it
/// (`crateserver_container_tracks` of the `likes` containers), read with
/// reload() on a worker thread. A like or unlike made here shows at once,
/// and keeps showing until a reload finds the server agreeing.
class CrateLikes : public QObject {
    Q_OBJECT
  public:
    struct Options {
        /// Not owned; required.
        Outbox* pOutbox = nullptr;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
    };

    explicit CrateLikes(Options options, QObject* pParent = nullptr);
    ~CrateLikes() override;

    bool isLiked(const QString& serverId, const QString& entityUid) const;
    /// Whether every one of `tracks` is liked; false for none.
    bool allLiked(const QList<cache::TrackKey>& tracks) const;

    /// Likes `tracks`, or stops liking them if they are all liked already.
    /// Only the tracks whose state changes are sent.
    void toggle(const QList<cache::TrackKey>& tracks);
    /// Likes or stops liking `tracks`.
    void setLiked(const QList<cache::TrackKey>& tracks, bool liked);

    /// Reads the server's Likes container again, as Decks synced it.
    void reload(const QString& serverId);
    /// Waits for the reads in flight. For tests.
    void waitForReads();

  signals:
    /// The liked state of these tracks of `serverId` may have changed.
    void likesChanged(const QString& serverId);

  private:
    void onReloaded(const QString& serverId, const QSet<QString>& stored);

    Options m_options;
    QThreadPool m_pool;
    /// The Likes container of each server, as stored.
    QHash<QString, QSet<QString>> m_stored;
    /// The likes made here that the stored containers do not show yet, by
    /// server and lowercase uid.
    QHash<QString, QHash<QString, bool>> m_local;
};

} // namespace crate::writeback
