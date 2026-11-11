#include "crate/library/crateprefetcher.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QtConcurrentRun>

#include "library/autodj/autodjprocessor.h"
#include "library/dao/playlistdao.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::CratePrefetcher");

/// A track whose download failed is not waited for again before this.
constexpr qint64 kRetryAfterFailureMs = 60 * 1000;

constexpr int kReadAttempts = 4;
constexpr unsigned long kReadRetryMillis = 25;

QString keyOf(const cache::TrackKey& track) {
    return track.serverId + QLatin1Char('/') + track.entityUid;
}

/// The locations of the first `count` tracks of the Auto DJ queue.
QStringList readQueue(const QSqlDatabase& database, int count) {
    for (int attempt = 1;; ++attempt) {
        QSqlQuery query(database);
        query.setForwardOnly(true);
        query.prepare(QStringLiteral(
                "SELECT tl.location FROM PlaylistTracks pt "
                "JOIN Playlists p ON p.id = pt.playlist_id "
                "JOIN library l ON l.id = pt.track_id "
                "JOIN track_locations tl ON tl.id = l.location "
                "WHERE p.hidden = :type ORDER BY pt.position LIMIT :count"));
        query.bindValue(QStringLiteral(":type"), static_cast<int>(PlaylistDAO::PLHT_AUTO_DJ));
        query.bindValue(QStringLiteral(":count"), count);
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

CratePrefetcher::CratePrefetcher(Options options, QObject* parent)
        : QObject(parent),
          m_pCache(options.pCache),
          m_pDbConnectionPool(options.pDbConnectionPool),
          m_options(std::move(options)) {
    m_pool.setMaxThreadCount(1);
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(m_options.settleTime);
    connect(&m_settleTimer, &QTimer::timeout, this, &CratePrefetcher::refresh);
    if (PlaylistDAO* pDao = m_options.pPlaylistDao) {
        connect(pDao, &PlaylistDAO::tracksAdded, this, &CratePrefetcher::scheduleRefresh);
        connect(pDao, &PlaylistDAO::tracksMoved, this, &CratePrefetcher::scheduleRefresh);
        connect(pDao, &PlaylistDAO::tracksRemoved, this, &CratePrefetcher::scheduleRefresh);
        connect(pDao,
                &PlaylistDAO::playlistContentChanged,
                this,
                &CratePrefetcher::scheduleRefresh);
    }
    if (m_options.answerPendingTrackHook) {
        m_hookInstalled = true;
        AutoDJProcessor::setPendingTrackHook([this](const TrackPointer& pTrack) {
            return pTrack && isOnItsWay(pTrack->getLocation());
        });
    }
    scheduleRefresh();
}

CratePrefetcher::~CratePrefetcher() {
    if (m_hookInstalled) {
        AutoDJProcessor::setPendingTrackHook(nullptr);
    }
    m_pool.waitForDone();
}

qint64 CratePrefetcher::now() const {
    return m_options.nowMs ? m_options.nowMs() : QDateTime::currentMSecsSinceEpoch();
}

void CratePrefetcher::scheduleRefresh() {
    if (!m_settleTimer.isActive()) {
        m_settleTimer.start();
    }
}

void CratePrefetcher::refresh() {
    if (!m_pDbConnectionPool) {
        return;
    }
    auto* pWatcher = new QFutureWatcher<QStringList>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher] {
        pWatcher->deleteLater();
        apply(pWatcher->result());
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_pDbConnectionPool, count = m_options.count]() {
                const mixxx::DbConnectionPooler pooler(pPool);
                return readQueue(mixxx::DbConnectionPooled(pPool), count);
            }));
}

void CratePrefetcher::apply(const QStringList& locations) {
    QHash<QString, QPointer<cache::CacheTicket>> tickets;
    for (const QString& location : locations) {
        ensure(location, &tickets);
    }
    // The tracks that left the first places stop their downloads, unless
    // Auto DJ waits for them
    for (auto it = m_tickets.cbegin(); it != m_tickets.cend(); ++it) {
        if (!tickets.contains(it.key()) && it.value()) {
            it.value()->cancel();
        }
    }
    m_tickets = std::move(tickets);
    for (auto it = m_pendingTickets.begin(); it != m_pendingTickets.end();) {
        it = it.value().isNull() ? m_pendingTickets.erase(it) : std::next(it);
    }
    emit refreshed();
}

QString CratePrefetcher::ensure(
        const QString& location, QHash<QString, QPointer<cache::CacheTicket>>* pTickets) {
    if (!m_pCache) {
        return QString();
    }
    const std::optional<cache::TrackKey> track = m_pCache->trackOfFile(location);
    if (!track) {
        return QString();
    }
    const QString key = keyOf(*track);
    for (const auto* pOwn : {&m_tickets, &m_pendingTickets}) {
        if (const auto it = pOwn->constFind(key); it != pOwn->cend() && it.value()) {
            // Still on its way
            pTickets->insert(key, it.value());
            return key;
        }
    }
    if (m_pCache->cachedPath(track->serverId, track->entityUid) &&
            QFileInfo::exists(location)) {
        return key;
    }
    cache::TrackHints hints;
    // The extension the file had: the same format again
    hints.format = QFileInfo(location).suffix();
    cache::CacheTicket* pTicket = m_pCache->ensure(
            track->serverId, track->entityUid, cache::Priority::AutoDj, hints);
    connect(pTicket,
            &cache::CacheTicket::failed,
            this,
            [this, key](const crate::Error& error) {
                kLogger.warning() << "Cannot fetch the Auto DJ track" << key << ":"
                                  << error.message;
                m_failedAtMs.insert(key, now());
            });
    connect(pTicket, &cache::CacheTicket::ready, this, [this, key] {
        m_failedAtMs.remove(key);
    });
    pTickets->insert(key, pTicket);
    return key;
}

bool CratePrefetcher::isOnItsWay(const QString& location) {
    if (!m_pCache) {
        return false;
    }
    const std::optional<cache::TrackKey> track = m_pCache->trackOfFile(location);
    if (!track) {
        // Not a Crate track: Auto DJ skips it as before
        return false;
    }
    const QString key = keyOf(*track);
    if (const auto failed = m_failedAtMs.constFind(key);
            failed != m_failedAtMs.cend() && now() - failed.value() < kRetryAfterFailureMs) {
        return false;
    }
    for (const auto* pTickets : {&m_tickets, &m_pendingTickets}) {
        if (const auto it = pTickets->constFind(key); it != pTickets->cend() && it.value()) {
            return true;
        }
    }
    if (QFileInfo::exists(location)) {
        return true;
    }
    m_failedAtMs.remove(key);
    const QString ensured = ensure(location, &m_pendingTickets);
    return !ensured.isEmpty();
}

QStringList CratePrefetcher::prefetchedTracks() const {
    QStringList keys = m_tickets.keys();
    keys.sort();
    return keys;
}

} // namespace crate::library

#include "moc_crateprefetcher.cpp"
