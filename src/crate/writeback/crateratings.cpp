#include "crate/writeback/crateratings.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QtConcurrentRun>

#include "crate/writeback/outbox.h"
#include "track/globaltrackcache.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::writeback {

namespace {

const mixxx::Logger kLogger("crate::writeback::CrateRatings");

QString keyOf(const QString& serverId, const QString& entityUid) {
    return serverId + QLatin1Char('\n') + entityUid.toLower();
}

} // namespace

CrateRatings::CrateRatings(Options options, QObject* pParent)
        : QObject(pParent),
          m_options(std::move(options)) {
    // One thread: the table's reads and writes run in the order they are made
    m_pool.setMaxThreadCount(1);
    if (!m_options.trackById) {
        m_options.trackById = [](TrackId trackId) {
            // Only tracks in memory: a rating changes on a track in use
            return GlobalTrackCacheLocker().lookupTrackById(trackId);
        };
    }
}

CrateRatings::~CrateRatings() {
    m_pool.waitForDone();
}

void CrateRatings::start() {
    if (m_loaded || m_loading) {
        return;
    }
    m_loading = true;
    auto* pWatcher = new QFutureWatcher<QHash<QString, int>>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher] {
        pWatcher->deleteLater();
        onLoaded(pWatcher->result());
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool, [pPool = m_options.pDbConnectionPool] {
        const mixxx::DbConnectionPooler pooler(pPool);
        const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
        QHash<QString, int> sent;
        QSqlQuery query(database);
        query.setForwardOnly(true);
        if (!query.exec(QStringLiteral(
                    "SELECT server_id, entity_uid, rating FROM crateserver_ratings"))) {
            kLogger.warning() << "Cannot read the ratings sent:" << query.lastError().text();
            return sent;
        }
        while (query.next()) {
            sent.insert(keyOf(query.value(0).toString(), query.value(1).toString()),
                    query.value(2).toInt());
        }
        return sent;
    }));
}

void CrateRatings::waitForStorage() {
    m_pool.waitForDone();
}

void CrateRatings::onLoaded(const QHash<QString, int>& sent) {
    m_sent = sent;
    m_loading = false;
    m_loaded = true;
    const QSet<TrackId> waiting = std::exchange(m_waiting, {});
    onTracksChanged(waiting);
}

void CrateRatings::onTracksChanged(const QSet<TrackId>& trackIds) {
    if (!m_loaded) {
        m_waiting.unite(trackIds);
        return;
    }
    for (const TrackId& trackId : trackIds) {
        if (const TrackPointer pTrack = m_options.trackById(trackId)) {
            check(pTrack);
        }
    }
}

void CrateRatings::check(const TrackPointer& pTrack) {
    if (!m_options.trackOfFile || !m_options.pOutbox) {
        return;
    }
    const std::optional<cache::TrackKey> crateTrack = m_options.trackOfFile(pTrack->getLocation());
    if (!crateTrack) {
        return;
    }
    const int rating = pTrack->getRating();
    const QString key = keyOf(crateTrack->serverId, crateTrack->entityUid);
    const auto sent = m_sent.constFind(key);
    if (sent == m_sent.cend() ? rating <= 0 : sent.value() == rating) {
        return;
    }
    m_sent.insert(key, rating);
    m_options.pOutbox->enqueue(crateTrack->serverId,
            outboxkind::kRate,
            QJsonObject{{QStringLiteral("entity_uid"), crateTrack->entityUid},
                    {QStringLiteral("rating"), rating}});
    m_pool.start([pPool = m_options.pDbConnectionPool,
                         serverId = crateTrack->serverId,
                         uid = crateTrack->entityUid.toLower(),
                         rating,
                         now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)] {
        const mixxx::DbConnectionPooler pooler(pPool);
        const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
        QSqlQuery query(database);
        query.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO crateserver_ratings "
                "(server_id, entity_uid, rating, sent_at) "
                "VALUES (:server, :uid, :rating, :sent)"));
        query.bindValue(QStringLiteral(":server"), serverId);
        query.bindValue(QStringLiteral(":uid"), uid);
        query.bindValue(QStringLiteral(":rating"), rating);
        query.bindValue(QStringLiteral(":sent"), now);
        if (!query.exec()) {
            kLogger.warning() << "Cannot keep the rating sent:"
                              << query.lastError().text();
        }
    });
}

} // namespace crate::writeback

#include "moc_crateratings.cpp"
