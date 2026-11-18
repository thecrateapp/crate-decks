#include "crate/writeback/trackdatasync.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

#include "crate/client/crateclient.h"
#include "crate/writeback/outbox.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::writeback {

namespace {

const mixxx::Logger kLogger("crate::writeback::TrackDataSync");

const QString kEntityUidKey = QStringLiteral("entity_uid");
const QString kEditedAtKey = QStringLiteral("edited_at");
const QString kBodyKey = QStringLiteral("body");
const QString kBaseRevisionKey = QStringLiteral("base_revision");
const QString kClientIdKey = QStringLiteral("client_id");
const QString kRevisionKey = QStringLiteral("revision");
const QString kUpdatedAtKey = QStringLiteral("updated_at");
const QString kServerTimeKey = QStringLiteral("server_time");
const QString kCurrentKey = QStringLiteral("current");

QString isoOf(const QDateTime& time) {
    return time.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime timeOf(const QJsonValue& value) {
    QDateTime time = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    if (!time.isValid()) {
        time = QDateTime::fromString(value.toString(), Qt::ISODate);
    }
    return time;
}

/// The content of a PUT body: the body without its revision and client.
QJsonObject contentOfBody(QJsonObject body) {
    body.remove(kBaseRevisionKey);
    body.remove(kClientIdKey);
    return body;
}

} // namespace

TrackDataSync::TrackDataSync(Endpoint endpoint, Options options, QObject* pParent)
        : QObject(pParent),
          m_endpoint(std::move(endpoint)),
          m_options(std::move(options)) {
    // One thread: the table's reads and writes run in the order they are made
    m_pool.setMaxThreadCount(1);
    if (!m_options.schedule) {
        m_options.schedule = [this](std::chrono::milliseconds delay,
                                     const std::function<void()>& callback) {
            QTimer::singleShot(delay, this, callback);
        };
    }
    if (m_options.pOutbox) {
        connect(m_options.pOutbox, &Outbox::delivered, this, &TrackDataSync::onDelivered);
        connect(m_options.pOutbox, &Outbox::conflicted, this, &TrackDataSync::onConflicted);
    }
}

TrackDataSync::~TrackDataSync() {
    m_pool.waitForDone();
}

QString TrackDataSync::keyOf(const QString& serverId, const QString& entityUid) {
    return serverId + QLatin1Char('\n') + entityUid.toLower();
}

QString TrackDataSync::serverIdOf(const QString& key) const {
    return key.section(QLatin1Char('\n'), 0, 0);
}

QString TrackDataSync::entityUidOf(const QString& key) const {
    return key.section(QLatin1Char('\n'), 1);
}

QDateTime TrackDataSync::now() const {
    return m_options.now ? m_options.now() : QDateTime::currentDateTimeUtc();
}

void TrackDataSync::start() {
    if (m_loaded || m_loading) {
        return;
    }
    m_loading = true;
    auto* pWatcher = new QFutureWatcher<QHash<QString, State>>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher] {
        pWatcher->deleteLater();
        onLoaded(pWatcher->result());
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_options.pDbConnectionPool, kind = m_endpoint.kind] {
                const mixxx::DbConnectionPooler pooler(pPool);
                const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
                QHash<QString, State> stored;
                QSqlQuery query(database);
                query.setForwardOnly(true);
                query.prepare(QStringLiteral(
                        "SELECT server_id, entity_uid, revision, baseline "
                        "FROM crateserver_user_sync WHERE kind = :kind"));
                query.bindValue(QStringLiteral(":kind"), kind);
                if (!query.exec()) {
                    kLogger.warning() << "Cannot read the synced" << kind << ":"
                                      << query.lastError().text();
                    return stored;
                }
                while (query.next()) {
                    State state;
                    state.revision = query.value(2).toLongLong();
                    state.baseline = QJsonDocument::fromJson(query.value(3).toByteArray()).object();
                    state.known = true;
                    stored.insert(keyOf(query.value(0).toString(), query.value(1).toString()),
                            state);
                }
                return stored;
            }));
}

void TrackDataSync::waitForStorage() {
    m_pool.waitForDone();
}

void TrackDataSync::onLoaded(const QHash<QString, State>& stored) {
    // What this run learnt before the table was read is newer
    for (auto it = stored.cbegin(); it != stored.cend(); ++it) {
        if (!m_states.contains(it.key())) {
            m_states.insert(it.key(), it.value());
        }
    }
    m_loading = false;
    m_loaded = true;
    emit loaded();
    const QStringList waiting = std::exchange(m_waiting, {});
    for (const QString& key : waiting) {
        queueQuery(key);
    }
}

void TrackDataSync::attach(
        const TrackPointer& pTrack, const QString& serverId, const QString& entityUid) {
    if (!pTrack || serverId.isEmpty() || entityUid.isEmpty()) {
        return;
    }
    if (m_options.enabledFor && !m_options.enabledFor(serverId)) {
        return;
    }
    const QString key = keyOf(serverId, entityUid);
    auto it = m_attached.find(key);
    const bool sameTrack = it != m_attached.end() && it->pTrack.lock() == pTrack;
    if (!sameTrack) {
        Attached attached;
        attached.pTrack = pTrack;
        attached.serverId = serverId;
        attached.entityUid = entityUid.toLower();
        attached.cueImportPending =
                pTrack->getCueImportStatus() == Track::ImportStatus::Pending;
        attached.beatsImportPending =
                pTrack->getBeatsImportStatus() == Track::ImportStatus::Pending;
        m_attached.insert(key, attached);
        Track* const pRaw = pTrack.get();
        connect(pRaw, &Track::cuesUpdated, this, [this, key] {
            onSignal(key, Change::Cues);
        });
        connect(pRaw, &Track::beatsUpdated, this, [this, key] {
            onSignal(key, Change::Beats);
        });
        connect(pRaw, &Track::keyChanged, this, [this, key] {
            onSignal(key, Change::Key);
        });
        // Upstream's analysis stored its results just before
        connect(pRaw, &Track::analyzed, this, [this, key] {
            if (const TrackPointer pTrack = trackOf(key)) {
                absorbAutomaticChanges(pTrack);
            }
        });
    } else {
        it->reconciled = false;
    }
    if (!m_loaded) {
        if (!m_waiting.contains(key)) {
            m_waiting.append(key);
        }
        return;
    }
    queueQuery(key);
}

void TrackDataSync::absorbAutomaticChanges(const TrackPointer& pTrack) {
    if (!pTrack) {
        return;
    }
    for (auto it = m_attached.begin(); it != m_attached.end(); ++it) {
        if (it->pTrack.lock() != pTrack) {
            continue;
        }
        // An import left pending completes when the file is opened
        it->cueImportPending = pTrack->getCueImportStatus() == Track::ImportStatus::Pending;
        it->beatsImportPending = pTrack->getBeatsImportStatus() == Track::ImportStatus::Pending;
        ++it->generation;
        m_due.remove(it.key());
        if (it->reconciled) {
            absorb(it.key(), pTrack);
        }
    }
}

qint64 TrackDataSync::revision(const QString& serverId, const QString& entityUid) const {
    return m_states.value(keyOf(serverId, entityUid)).revision;
}

TrackPointer TrackDataSync::trackOf(const QString& key) const {
    const auto it = m_attached.constFind(key);
    return it == m_attached.cend() ? TrackPointer() : it->pTrack.lock();
}

bool TrackDataSync::isReconciled(const QString& key) const {
    const auto it = m_attached.constFind(key);
    return it != m_attached.cend() && it->reconciled;
}

void TrackDataSync::queueQuery(const QString& key) {
    const QString serverId = serverIdOf(key);
    QStringList& queued = m_queued[serverId];
    if (!queued.contains(key)) {
        queued.append(key);
    }
    if (m_queryScheduled.value(serverId)) {
        return;
    }
    m_queryScheduled.insert(serverId, true);
    m_options.schedule(kBatchWindow, [this, serverId] {
        m_queryScheduled.insert(serverId, false);
        query(serverId);
    });
}

void TrackDataSync::query(const QString& serverId) {
    Client* pClient = m_options.clientFor ? m_options.clientFor(serverId) : nullptr;
    QStringList keys = m_queued.take(serverId);
    if (!pClient) {
        // Signed out: the next load asks again
        return;
    }
    while (!keys.isEmpty()) {
        const QStringList batch = keys.mid(0, kBatchLimit);
        keys = keys.mid(batch.size());
        QJsonArray uids;
        for (const QString& key : batch) {
            uids.append(entityUidOf(key));
        }
        Reply* pReply = pClient->post(m_endpoint.queryPath,
                QJsonDocument(QJsonObject{{QStringLiteral("entity_uids"), uids}}));
        connect(pReply,
                &Reply::finished,
                this,
                [this, serverId, batch](const JsonResult& result) {
                    if (!result.ok()) {
                        kLogger.warning() << "Cannot read the user's" << m_endpoint.kind << "of"
                                          << batch.size() << "tracks:" << result.error().message;
                        retryQuery(serverId, batch, result.error());
                        return;
                    }
                    m_queryFailures.remove(serverId);
                    observeServerTime(serverId, result.value().object());
                    onAnswer(batch, result.value().object());
                });
    }
}

void TrackDataSync::retryQuery(
        const QString& serverId, const QStringList& batch, const Error& error) {
    const bool mayPass = error.kind == Error::Kind::Network || error.kind == Error::Kind::Auth ||
            (error.kind == Error::Kind::Http &&
                    (error.httpStatus == 408 || error.httpStatus == 425 ||
                            error.httpStatus == 429 || error.httpStatus >= 500));
    if (!mayPass) {
        // The next load of the tracks asks again
        return;
    }
    QStringList& queued = m_queued[serverId];
    for (const QString& key : batch) {
        const auto it = m_attached.constFind(key);
        if (it != m_attached.cend() && !it->reconciled && !queued.contains(key)) {
            queued.append(key);
        }
    }
    if (queued.isEmpty() || m_queryScheduled.value(serverId)) {
        return;
    }
    const int failures = m_queryFailures.value(serverId);
    m_queryFailures.insert(serverId, failures + 1);
    const auto delay = kQueryRetry[static_cast<size_t>(
            std::min<int>(failures, static_cast<int>(kQueryRetry.size()) - 1))];
    m_queryScheduled.insert(serverId, true);
    m_options.schedule(delay, [this, serverId] {
        m_queryScheduled.insert(serverId, false);
        query(serverId);
    });
}

void TrackDataSync::onAnswer(const QStringList& keys, const QJsonObject& answer) {
    const QJsonObject entries = answer.value(m_endpoint.answerKey).toObject();
    for (const QString& key : keys) {
        const TrackPointer pTrack = trackOf(key);
        if (!pTrack) {
            continue;
        }
        const QJsonObject entry = entries.value(entityUidOf(key)).toObject();
        const qint64 serverRevision = entry.value(kRevisionKey).toInteger(0);
        const bool reconciled = reconcile(key, pTrack, serverRevision, entry);
        auto it = m_attached.find(key);
        if (it != m_attached.end()) {
            it->reconciled = reconciled;
        }
    }
}

bool TrackDataSync::importCompleted(Attached& attached, const TrackPointer& pTrack) {
    const bool cuesPending = pTrack->getCueImportStatus() == Track::ImportStatus::Pending;
    const bool beatsPending = pTrack->getBeatsImportStatus() == Track::ImportStatus::Pending;
    const bool completed = (attached.cueImportPending && !cuesPending) ||
            (attached.beatsImportPending && !beatsPending);
    attached.cueImportPending = cuesPending;
    attached.beatsImportPending = beatsPending;
    return completed;
}

void TrackDataSync::onSignal(const QString& key, Change change) {
    if (m_applying) {
        return;
    }
    auto it = m_attached.find(key);
    if (it == m_attached.end()) {
        return;
    }
    const TrackPointer pTrack = it->pTrack.lock();
    if (!pTrack) {
        return;
    }
    if (importCompleted(*it, pTrack)) {
        // The file was opened and an import of the analysis went in
        if (it->reconciled) {
            absorb(key, pTrack);
        }
        return;
    }
    if (!it->reconciled) {
        // reconcile() looks at what the track has by then
        return;
    }
    trackChanged(key, pTrack, change);
}

void TrackDataSync::storeState(const QString& key) {
    State& state = m_states[key];
    state.known = true;
    m_pool.start([pPool = m_options.pDbConnectionPool,
                         serverId = serverIdOf(key),
                         uid = entityUidOf(key),
                         kind = m_endpoint.kind,
                         revision = state.revision,
                         baseline = QJsonDocument(state.baseline).toJson(QJsonDocument::Compact),
                         updatedAt = isoOf(now())] {
        const mixxx::DbConnectionPooler pooler(pPool);
        const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
        QSqlQuery query(database);
        query.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO crateserver_user_sync "
                "(server_id, entity_uid, kind, revision, baseline, updated_at) "
                "VALUES (:server, :uid, :kind, :revision, :baseline, :updated)"));
        query.bindValue(QStringLiteral(":server"), serverId);
        query.bindValue(QStringLiteral(":uid"), uid);
        query.bindValue(QStringLiteral(":kind"), kind);
        query.bindValue(QStringLiteral(":revision"), revision);
        query.bindValue(QStringLiteral(":baseline"), QString::fromUtf8(baseline));
        query.bindValue(QStringLiteral(":updated"), updatedAt);
        if (!query.exec()) {
            kLogger.warning() << "Cannot keep the synced" << kind << ":"
                              << query.lastError().text();
        }
    });
}

void TrackDataSync::enqueuePut(
        const QString& key, const QJsonObject& content, const QDateTime& editedAt) {
    if (!m_options.pOutbox) {
        return;
    }
    State& state = m_states[key];
    QJsonObject body = content;
    body.insert(kBaseRevisionKey, state.revision);
    if (!m_options.clientId.isEmpty()) {
        body.insert(kClientIdKey, m_options.clientId);
    }
    state.baseline = content;
    storeState(key);
    m_options.pOutbox->enqueue(serverIdOf(key),
            m_endpoint.outboxKind,
            QJsonObject{{kEntityUidKey, entityUidOf(key)},
                    {kEditedAtKey, isoOf(editedAt)},
                    {kBodyKey, body}});
}

bool TrackDataSync::uploadPending(const QString& key) const {
    if (!m_options.pOutbox) {
        return false;
    }
    const QString uid = entityUidOf(key);
    const QList<QJsonObject> payloads =
            m_options.pOutbox->pendingPayloads(serverIdOf(key), m_endpoint.outboxKind);
    return std::any_of(payloads.cbegin(), payloads.cend(), [&uid](const QJsonObject& payload) {
        return payload.value(kEntityUidKey).toString().compare(uid, Qt::CaseInsensitive) == 0;
    });
}

void TrackDataSync::debounce(const QString& key, std::function<void()> callback) {
    auto it = m_attached.find(key);
    if (it == m_attached.end()) {
        return;
    }
    const quint64 generation = ++it->generation;
    m_due.insert(key, Due{generation, std::move(callback)});
    m_options.schedule(kDebounce, [this, key, generation] {
        const auto due = m_due.constFind(key);
        if (due == m_due.cend() || due->generation != generation) {
            return;
        }
        const std::function<void()> callback = due->callback;
        m_due.erase(due);
        callback();
    });
}

void TrackDataSync::runDebounced() {
    const QHash<QString, Due> due = std::exchange(m_due, {});
    for (const Due& entry : due) {
        entry.callback();
    }
}

void TrackDataSync::applyFromServer(const QString& key,
        const TrackPointer& pTrack,
        const QJsonObject& content,
        qint64 revision) {
    State& state = m_states[key];
    m_applying = true;
    state.baseline = applyServerContent(key, pTrack, content);
    m_applying = false;
    state.revision = revision;
    storeState(key);
    // Its own changes are not edits; a debounce of older ones is void
    auto it = m_attached.find(key);
    if (it != m_attached.end()) {
        ++it->generation;
        m_due.remove(key);
    }
    emit serverDataApplied(serverIdOf(key), entityUidOf(key));
}

void TrackDataSync::onDelivered(const QString& serverId,
        const QString& kind,
        const QJsonObject& payload,
        const QJsonDocument& answer) {
    if (kind != m_endpoint.outboxKind) {
        return;
    }
    observeServerTime(serverId, answer.object());
    const QString key = keyOf(serverId, payload.value(kEntityUidKey).toString());
    const qint64 revision = answer.object().value(kRevisionKey).toInteger(-1);
    if (revision < 0) {
        return;
    }
    // The baseline is the content sent last, set when it was queued
    State& state = m_states[key];
    state.revision = revision;
    storeState(key);
}

void TrackDataSync::onConflicted(const QString& serverId,
        const QString& kind,
        const QJsonObject& payload,
        const QJsonObject& answer) {
    if (kind != m_endpoint.outboxKind) {
        return;
    }
    observeServerTime(serverId, answer);
    const QJsonObject current = answer.value(kCurrentKey).toObject();
    const QString key = keyOf(serverId, payload.value(kEntityUidKey).toString());
    const qint64 serverRevision = current.value(kRevisionKey).toInteger(0);
    const QDateTime editedAt = timeOf(payload.value(kEditedAtKey));
    const QDateTime writtenAt = timeOf(current.value(kUpdatedAtKey));
    const QString writer = current.value(kClientIdKey).toString();
    const bool ownWrite = !writer.isEmpty() && writer == m_options.clientId;
    // Both on the server's clock, when the edit was stamped with it
    // (editStamp()); on this computer's otherwise, as before Diego's answer
    const bool serverNewer = !ownWrite && writtenAt.isValid() && editedAt.isValid() &&
            writtenAt > editedAt;
    State& state = m_states[key];
    if (!serverNewer) {
        // An older write of this installation, or one made before the edit
        // here: the edit goes on top of it
        state.revision = serverRevision;
        if (uploadPending(key)) {
            // A newer edit is on its way, and resolves the same way
            storeState(key);
            return;
        }
        kLogger.info() << "Sending the" << m_endpoint.kind << "of" << entityUidOf(key)
                       << "again on top of revision" << serverRevision;
        enqueuePut(key,
                contentToResend(key, contentOfBody(payload.value(kBodyKey).toObject())),
                editedAt.isValid() ? editedAt : editStamp(key));
        return;
    }
    kLogger.info() << "The" << m_endpoint.kind << "of" << entityUidOf(key)
                   << "changed on another computer after the edit here: it wins";
    const TrackPointer pTrack = trackOf(key);
    QString title = entityUidOf(key);
    if (pTrack) {
        if (!pTrack->getTitle().isEmpty()) {
            title = pTrack->getTitle();
        }
        applyFromServer(key, pTrack, contentOf(current), serverRevision);
        absorb(key, pTrack);
    }
    // Without the track in memory the revision stays: the next load finds
    // the server's newer and applies it
    emit localEditsReplaced(serverId, entityUidOf(key), title);
}

void TrackDataSync::observeServerTime(const QString& serverId, const QJsonObject& answer) {
    const QDateTime serverTime = timeOf(answer.value(kServerTimeKey));
    if (!serverTime.isValid()) {
        return;
    }
    m_clockOffsets.insert(serverId, now().msecsTo(serverTime));
}

QDateTime TrackDataSync::editStamp(const QString& key) const {
    const auto it = m_clockOffsets.constFind(serverIdOf(key));
    if (it == m_clockOffsets.cend()) {
        return now();
    }
    return now().addMSecs(it.value());
}

void TrackDataSync::changeQuietly(const QString& key, const std::function<void()>& change) {
    m_applying = true;
    change();
    m_applying = false;
    auto it = m_attached.find(key);
    if (it != m_attached.end()) {
        ++it->generation;
        m_due.remove(key);
    }
}

} // namespace crate::writeback

#include "moc_trackdatasync.cpp"
