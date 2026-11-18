#include "crate/writeback/cratelikes.h"

#include <QFutureWatcher>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QtConcurrentRun>
#include <algorithm>

#include "crate/writeback/outbox.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::writeback {

namespace {

const mixxx::Logger kLogger("crate::writeback::CrateLikes");

QSet<QString> readLikes(const mixxx::DbConnectionPoolPtr& pPool, const QString& serverId) {
    const mixxx::DbConnectionPooler pooler(pPool);
    const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
    QSet<QString> uids;
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT ct.entity_uid FROM crateserver_container_tracks ct "
            "JOIN crateserver_containers c ON c.id = ct.container_id "
            "WHERE c.server_id = :server AND c.kind = 'likes'"));
    query.bindValue(QStringLiteral(":server"), serverId);
    if (!query.exec()) {
        kLogger.warning() << "Cannot read the likes:" << query.lastError().text();
        return uids;
    }
    while (query.next()) {
        uids.insert(query.value(0).toString().toLower());
    }
    return uids;
}

} // namespace

CrateLikes::CrateLikes(Options options, QObject* pParent)
        : QObject(pParent),
          m_options(std::move(options)) {
    m_pool.setMaxThreadCount(1);
}

CrateLikes::~CrateLikes() {
    m_pool.waitForDone();
}

bool CrateLikes::isLiked(const QString& serverId, const QString& entityUid) const {
    const QString uid = entityUid.toLower();
    const auto local = m_local.constFind(serverId);
    if (local != m_local.cend()) {
        const auto it = local->constFind(uid);
        if (it != local->cend()) {
            return it.value();
        }
    }
    return m_stored.value(serverId).contains(uid);
}

bool CrateLikes::allLiked(const QList<cache::TrackKey>& tracks) const {
    return !tracks.isEmpty() &&
            std::all_of(
                    tracks.cbegin(), tracks.cend(), [this](const auto& track) {
                        return isLiked(track.serverId, track.entityUid);
                    });
}

void CrateLikes::toggle(const QList<cache::TrackKey>& tracks) {
    setLiked(tracks, !allLiked(tracks));
}

void CrateLikes::setLiked(const QList<cache::TrackKey>& tracks, bool liked) {
    QSet<QString> changedServers;
    for (const cache::TrackKey& track : tracks) {
        if (track.serverId.isEmpty() || track.entityUid.isEmpty() ||
                isLiked(track.serverId, track.entityUid) == liked) {
            continue;
        }
        m_local[track.serverId].insert(track.entityUid.toLower(), liked);
        changedServers.insert(track.serverId);
        if (m_options.pOutbox) {
            m_options.pOutbox->enqueue(track.serverId,
                    liked ? outboxkind::kLike : outboxkind::kUnlike,
                    QJsonObject{{QStringLiteral("track_entity_uid"), track.entityUid}});
        }
    }
    for (const QString& serverId : std::as_const(changedServers)) {
        emit likesChanged(serverId);
    }
}

void CrateLikes::reload(const QString& serverId) {
    auto* pWatcher = new QFutureWatcher<QSet<QString>>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher, serverId] {
        pWatcher->deleteLater();
        onReloaded(serverId, pWatcher->result());
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_options.pDbConnectionPool, serverId] {
                return readLikes(pPool, serverId);
            }));
}

void CrateLikes::waitForReads() {
    m_pool.waitForDone();
}

void CrateLikes::onReloaded(const QString& serverId, const QSet<QString>& stored) {
    m_stored.insert(serverId, stored);
    // What the server agrees with needs no local state any more
    auto local = m_local.find(serverId);
    if (local != m_local.end()) {
        for (auto it = local->begin(); it != local->end();) {
            if (stored.contains(it.key()) == it.value()) {
                it = local->erase(it);
            } else {
                ++it;
            }
        }
    }
    emit likesChanged(serverId);
}

} // namespace crate::writeback

#include "moc_cratelikes.cpp"
