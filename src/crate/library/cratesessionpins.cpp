#include "crate/library/cratesessionpins.h"

#include <QFutureWatcher>
#include <QMap>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QtConcurrentRun>
#include <optional>

#include "library/dao/playlistdao.h"
#include "mixer/playerinfo.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::CrateSessionPins");

constexpr int kReadAttempts = 4;
constexpr unsigned long kReadRetryMillis = 25;

QString keyOf(const cache::TrackKey& track) {
    return track.serverId + QLatin1Char('/') + track.entityUid;
}

/// The locations of every track of the Auto DJ queue.
QStringList readQueue(const QSqlDatabase& database) {
    for (int attempt = 1;; ++attempt) {
        QSqlQuery query(database);
        query.setForwardOnly(true);
        query.prepare(QStringLiteral(
                "SELECT tl.location FROM PlaylistTracks pt "
                "JOIN Playlists p ON p.id = pt.playlist_id "
                "JOIN library l ON l.id = pt.track_id "
                "JOIN track_locations tl ON tl.id = l.location "
                "WHERE p.hidden = :type"));
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

const QString CrateSessionPins::kQueueReason = cache::AudioCache::kSessionPinPrefix +
        QStringLiteral("auto-dj");

QString CrateSessionPins::playerReason(const QString& group) {
    return cache::AudioCache::kSessionPinPrefix + QStringLiteral("player:") + group;
}

CrateSessionPins::CrateSessionPins(const Options& options, QObject* parent)
        : QObject(parent),
          m_pCache(options.pCache),
          m_pDbConnectionPool(options.pDbConnectionPool) {
    m_pool.setMaxThreadCount(1);
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(options.settleTime);
    connect(&m_settleTimer, &QTimer::timeout, this, &CrateSessionPins::refresh);
    if (PlaylistDAO* pDao = options.pPlaylistDao) {
        connect(pDao, &PlaylistDAO::tracksAdded, this, &CrateSessionPins::scheduleRefresh);
        connect(pDao, &PlaylistDAO::tracksMoved, this, &CrateSessionPins::scheduleRefresh);
        connect(pDao, &PlaylistDAO::tracksRemoved, this, &CrateSessionPins::scheduleRefresh);
        connect(pDao,
                &PlaylistDAO::playlistContentChanged,
                this,
                &CrateSessionPins::scheduleRefresh);
    }
    if (PlayerInfo* pPlayerInfo = options.pPlayerInfo) {
        connect(pPlayerInfo,
                &PlayerInfo::trackChanged,
                this,
                [this](const QString& group, TrackPointer pNewTrack, TrackPointer pOldTrack) {
                    Q_UNUSED(pOldTrack);
                    onPlayerTrackChanged(group, pNewTrack);
                });
        // What the players have already
        const QMap<QString, TrackPointer> loaded = pPlayerInfo->getLoadedTracks();
        for (auto it = loaded.cbegin(); it != loaded.cend(); ++it) {
            onPlayerTrackChanged(it.key(), it.value());
        }
    }
    scheduleRefresh();
}

CrateSessionPins::~CrateSessionPins() {
    m_settleTimer.stop();
    m_pool.waitForDone();
    if (!m_pCache) {
        return;
    }
    // The cache goes on without them
    for (auto it = m_players.cbegin(); it != m_players.cend(); ++it) {
        m_pCache->unpin(it->serverId, it->entityUid, playerReason(it.key()));
    }
    for (auto it = m_queued.cbegin(); it != m_queued.cend(); ++it) {
        m_pCache->unpin(it->serverId, it->entityUid, kQueueReason);
    }
}

void CrateSessionPins::scheduleRefresh() {
    if (!m_settleTimer.isActive()) {
        m_settleTimer.start();
    }
}

QStringList CrateSessionPins::pinnedTracks() const {
    QSet<QString> keys;
    for (auto it = m_players.cbegin(); it != m_players.cend(); ++it) {
        keys.insert(keyOf(it.value()));
    }
    for (auto it = m_queued.cbegin(); it != m_queued.cend(); ++it) {
        keys.insert(it.key());
    }
    QStringList sorted(keys.cbegin(), keys.cend());
    sorted.sort();
    return sorted;
}

void CrateSessionPins::onPlayerTrackChanged(const QString& group, const TrackPointer& pTrack) {
    if (!m_pCache) {
        return;
    }
    // Only Crate tracks: their files are the cache's
    const std::optional<cache::TrackKey> track =
            pTrack ? m_pCache->trackOfFile(pTrack->getLocation()) : std::nullopt;
    const QString reason = playerReason(group);
    if (track) {
        // The new one first, so that unpinning the old one never makes
        // room at its expense
        m_pCache->pin(track->serverId, track->entityUid, reason);
    }
    const auto it = m_players.constFind(group);
    if (it != m_players.cend()) {
        const cache::TrackKey old = it.value();
        m_players.remove(group);
        if (!track || keyOf(old) != keyOf(*track)) {
            m_pCache->unpin(old.serverId, old.entityUid, reason);
        }
    }
    if (track) {
        m_players.insert(group, *track);
    }
}

void CrateSessionPins::refresh() {
    if (!m_pDbConnectionPool) {
        return;
    }
    auto* pWatcher = new QFutureWatcher<QStringList>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher] {
        pWatcher->deleteLater();
        applyQueue(pWatcher->result());
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool, [pPool = m_pDbConnectionPool]() {
        const mixxx::DbConnectionPooler pooler(pPool);
        return readQueue(mixxx::DbConnectionPooled(pPool));
    }));
}

void CrateSessionPins::applyQueue(const QStringList& locations) {
    if (!m_pCache) {
        return;
    }
    QHash<QString, cache::TrackKey> queued;
    for (const QString& location : locations) {
        if (const std::optional<cache::TrackKey> track = m_pCache->trackOfFile(location)) {
            queued.insert(keyOf(*track), *track);
        }
    }
    // The new ones first, so that unpinning never makes room at their
    // expense
    for (auto it = queued.cbegin(); it != queued.cend(); ++it) {
        if (!m_queued.contains(it.key())) {
            m_pCache->pin(it->serverId, it->entityUid, kQueueReason);
        }
    }
    for (auto it = m_queued.cbegin(); it != m_queued.cend(); ++it) {
        if (!queued.contains(it.key())) {
            m_pCache->unpin(it->serverId, it->entityUid, kQueueReason);
        }
    }
    m_queued = std::move(queued);
    emit refreshed();
}

} // namespace crate::library

#include "moc_cratesessionpins.cpp"
