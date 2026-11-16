#include "crate/recs/recommendationqueue.h"

#include <QFutureWatcher>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QtConcurrentRun>

#include "crate/library/crateloadcoordinator.h"
#include "library/dao/playlistdao.h"
#include "moc_recommendationqueue.cpp"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::recs {

namespace {

const mixxx::Logger kLogger("crate::recs::AutoDjRecommendationQueue");

constexpr int kReadAttempts = 4;
constexpr unsigned long kReadRetryMillis = 25;

/// The locations of the tracks of Auto DJ's queue, in its order.
QStringList readQueueLocations(const QSqlDatabase& database) {
    for (int attempt = 1;; ++attempt) {
        QSqlQuery query(database);
        query.setForwardOnly(true);
        query.prepare(QStringLiteral(
                "SELECT tl.location FROM PlaylistTracks pt "
                "JOIN Playlists p ON p.id = pt.playlist_id "
                "JOIN library l ON l.id = pt.track_id "
                "JOIN track_locations tl ON tl.id = l.location "
                "WHERE p.hidden = :type ORDER BY pt.position"));
        query.bindValue(QStringLiteral(":type"), static_cast<int>(PlaylistDAO::PLHT_AUTO_DJ));
        if (!query.exec()) {
            if (attempt < kReadAttempts) {
                QThread::msleep(kReadRetryMillis * attempt);
                continue;
            }
            kLogger.warning() << "Cannot read the Auto DJ queue:" << query.lastError().text();
            return {};
        }
        QStringList locations;
        while (query.next()) {
            locations.append(query.value(0).toString());
        }
        return locations;
    }
}

} // namespace

AutoDjRecommendationQueue::AutoDjRecommendationQueue(Options options, QObject* parent)
        : RecommendationQueue(parent),
          m_options(std::move(options)) {
    m_pool.setMaxThreadCount(1);
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(m_options.settleTime);
    connect(&m_settleTimer,
            &QTimer::timeout,
            this,
            &AutoDjRecommendationQueue::queueChanged);
    if (PlaylistDAO* pDao = m_options.pPlaylistDao) {
        const auto changed = [this] {
            if (!m_settleTimer.isActive()) {
                m_settleTimer.start();
            }
        };
        connect(pDao, &PlaylistDAO::tracksAdded, this, changed);
        connect(pDao, &PlaylistDAO::tracksMoved, this, changed);
        connect(pDao, &PlaylistDAO::tracksRemoved, this, changed);
        connect(pDao, &PlaylistDAO::playlistContentChanged, this, changed);
    }
}

AutoDjRecommendationQueue::~AutoDjRecommendationQueue() {
    m_settleTimer.stop();
    m_pool.waitForDone();
}

QList<TrackId> AutoDjRecommendationQueue::trackIdsOf(
        const QString& serverId, const QList<RecommendedTrack>& tracks) {
    if (!m_options.trackIds) {
        return {};
    }
    QList<library::CrateQueuedTrack> queued;
    queued.reserve(tracks.size());
    for (const RecommendedTrack& track : tracks) {
        library::CrateQueuedTrack item;
        item.track.serverId = serverId;
        item.track.entityUid = track.entityUid;
        item.track.hints.format = track.format;
        item.tags.title = track.title;
        item.tags.artist = track.artist;
        item.tags.album = track.album;
        item.tags.durationMs = track.durationMs;
        queued.append(item);
    }
    QList<TrackId> ids = m_options.trackIds(queued);
    ids.removeIf([](const TrackId& id) {
        return !id.isValid();
    });
    return ids;
}

void AutoDjRecommendationQueue::enqueue(
        const QString& serverId, const QList<RecommendedTrack>& tracks) {
    const QList<TrackId> ids = trackIdsOf(serverId, tracks);
    if (ids.isEmpty() || !m_options.pPlaylistDao) {
        return;
    }
    m_options.pPlaylistDao->addTracksToAutoDJQueue(ids, PlaylistDAO::AutoDJSendLoc::BOTTOM);
}

void AutoDjRecommendationQueue::readQueue(
        std::function<void(const QList<cache::TrackKey>& queued)> done) {
    auto* pWatcher = new QFutureWatcher<QStringList>(this);
    connect(pWatcher,
            &QFutureWatcherBase::finished,
            this,
            [this, pWatcher, done = std::move(done)] {
                pWatcher->deleteLater();
                QList<cache::TrackKey> queued;
                const QStringList locations = pWatcher->result();
                for (const QString& location : locations) {
                    if (const auto track = m_options.trackOfFile
                                    ? m_options.trackOfFile(location)
                                    : std::nullopt) {
                        queued.append(*track);
                    }
                }
                done(queued);
            });
    pWatcher->setFuture(QtConcurrent::run(&m_pool, [pPool = m_options.pDbConnectionPool]() {
        const mixxx::DbConnectionPooler pooler(pPool);
        return readQueueLocations(mixxx::DbConnectionPooled(pPool));
    }));
}

QString AutoDjRecommendationQueue::saveAsPlaylist(
        const QString& name, const QString& serverId, const QList<RecommendedTrack>& tracks) {
    PlaylistDAO* pDao = m_options.pPlaylistDao;
    if (!pDao) {
        return QString();
    }
    const QList<TrackId> ids = trackIdsOf(serverId, tracks);
    if (ids.isEmpty()) {
        return QString();
    }
    QString unique = name;
    const int playlistId = pDao->createUniquePlaylist(&unique);
    if (playlistId < 0 || !pDao->appendTracksToPlaylist(ids, playlistId)) {
        kLogger.warning() << "Cannot save the Music Path as a playlist";
        return QString();
    }
    return unique;
}

} // namespace crate::recs
