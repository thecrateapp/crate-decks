#include "crate/writeback/outbox.h"

#include <QFutureWatcher>
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QtConcurrentRun>

#include "crate/client/crateclient.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::writeback {

namespace {

const mixxx::Logger kLogger("crate::writeback::Outbox");

const QString kPlayEventsPath = QStringLiteral("/api/me/play-events");
const QString kLikesPath = QStringLiteral("/api/me/likes");
const QString kRatePath = QStringLiteral("/api/track/rate");
const QString kTrackIdKey = QStringLiteral("track_id");
const QString kEntityUidKey = QStringLiteral("entity_uid");
const QString kRatingKey = QStringLiteral("rating");
const QString kBodyKey = QStringLiteral("body");
const QString kCurrentKey = QStringLiteral("current");

QString isoOf(const QDateTime& time) {
    return time.toUTC().toString(Qt::ISODateWithMs);
}

QString trackInfoPath(const QString& entityUid) {
    return QStringLiteral("/api/tracks/by-entity/%1/info")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(entityUid)));
}

/// Runs `task` with a connection of the worker thread.
template<typename Task>
auto withDatabase(const mixxx::DbConnectionPoolPtr& pPool, Task&& task) {
    const mixxx::DbConnectionPooler pooler(pPool);
    const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
    return task(database);
}

QString putPath(const QString& kind, const QString& entityUid) {
    const QString resource = kind == outboxkind::kCueSet ? QStringLiteral("cues")
                                                         : QStringLiteral("corrections");
    return QStringLiteral("/api/decks/%1/%2")
            .arg(resource, QString::fromLatin1(QUrl::toPercentEncoding(entityUid)));
}

} // namespace

const std::array<std::chrono::milliseconds, 5> Outbox::kBackoff = {
        std::chrono::seconds(1),
        std::chrono::seconds(5),
        std::chrono::seconds(30),
        std::chrono::minutes(5),
        std::chrono::minutes(30),
};

Outbox::Outbox(Options options, QObject* pParent)
        : QObject(pParent),
          m_options(std::move(options)) {
    // One thread: the table's reads and writes run in the order they are made
    m_pool.setMaxThreadCount(1);
    if (!m_options.schedule) {
        m_options.schedule = [this](std::chrono::milliseconds delay,
                                     const std::function<void()>& callback) {
            QTimer::singleShot(delay, this, callback);
        };
    }
}

Outbox::~Outbox() {
    m_pool.waitForDone();
}

QDateTime Outbox::now() const {
    return m_options.now ? m_options.now() : QDateTime::currentDateTimeUtc();
}

void Outbox::start() {
    if (m_loaded || m_loading) {
        return;
    }
    m_loading = true;
    auto* pWatcher = new QFutureWatcher<QList<Entry>>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher] {
        pWatcher->deleteLater();
        onLoaded(pWatcher->result());
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool, [pPool = m_options.pDbConnectionPool] {
        return withDatabase(pPool, [](const QSqlDatabase& database) {
            QList<Entry> stored;
            QSqlQuery query(database);
            query.setForwardOnly(true);
            if (!query.exec(QStringLiteral(
                        "SELECT id, server_id, kind, payload, created_at "
                        "FROM crateserver_outbox ORDER BY rowid"))) {
                kLogger.warning() << "Cannot read the outbox:" << query.lastError().text();
                return stored;
            }
            while (query.next()) {
                Entry entry;
                entry.id = query.value(0).toString();
                entry.serverId = query.value(1).toString();
                entry.kind = query.value(2).toString();
                entry.payload = QJsonDocument::fromJson(query.value(3).toByteArray()).object();
                entry.createdAt =
                        QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs);
                stored.append(entry);
            }
            return stored;
        });
    }));
}

void Outbox::onLoaded(const QList<Entry>& stored) {
    // The stored entries come before those enqueued while they were read
    QHash<QString, QList<Entry>> byServer;
    QStringList servers;
    for (const Entry& entry : stored) {
        if (!byServer.contains(entry.serverId)) {
            servers.append(entry.serverId);
        }
        byServer[entry.serverId].append(entry);
    }
    for (auto it = m_servers.cbegin(); it != m_servers.cend(); ++it) {
        if (!servers.contains(it.key())) {
            servers.append(it.key());
        }
    }
    for (const QString& serverId : std::as_const(servers)) {
        ServerState& state = m_servers[serverId];
        // An entry enqueued before start() was stored before the read, and is
        // already in memory
        QSet<QString> inMemory;
        for (const Entry& entry : std::as_const(state.entries)) {
            inMemory.insert(entry.id);
        }
        QList<Entry> entries = byServer.value(serverId);
        entries.removeIf([&inMemory](const Entry& entry) {
            return inMemory.contains(entry.id);
        });
        state.entries = entries + state.entries;
    }
    m_loading = false;
    m_loaded = true;
    if (!stored.isEmpty()) {
        kLogger.info() << stored.size() << "writes left from an earlier run";
    }
    emit loaded();
    for (const QString& serverId : std::as_const(servers)) {
        pump(serverId);
    }
}

QString Outbox::enqueue(const QString& serverId, const QString& kind, const QJsonObject& payload) {
    Entry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.serverId = serverId;
    entry.kind = kind;
    entry.payload = payload;
    entry.createdAt = now().toUTC();
    m_servers[serverId].entries.append(entry);
    storeInsert(entry);
    pump(serverId);
    return entry.id;
}

void Outbox::flush() {
    const QStringList servers = m_servers.keys();
    for (const QString& serverId : servers) {
        ServerState& state = m_servers[serverId];
        if (state.inFlight) {
            continue;
        }
        // The retry it had scheduled is abandoned
        ++state.generation;
        state.waiting = false;
        state.failures = 0;
        pump(serverId);
    }
}

QList<QJsonObject> Outbox::pendingPayloads(const QString& serverId, const QString& kind) const {
    QList<QJsonObject> payloads;
    const auto it = m_servers.constFind(serverId);
    if (it == m_servers.cend()) {
        return payloads;
    }
    for (const Entry& entry : it->entries) {
        if (entry.kind == kind) {
            payloads.append(entry.payload);
        }
    }
    return payloads;
}

int Outbox::pendingCount(const QString& serverId) const {
    if (!serverId.isEmpty()) {
        const auto it = m_servers.constFind(serverId);
        return it == m_servers.cend() ? 0 : static_cast<int>(it->entries.size());
    }
    int count = 0;
    for (const ServerState& state : m_servers) {
        count += static_cast<int>(state.entries.size());
    }
    return count;
}

void Outbox::waitForStorage() {
    m_pool.waitForDone();
}

void Outbox::pump(const QString& serverId) {
    if (!m_loaded) {
        return;
    }
    ServerState& state = m_servers[serverId];
    if (state.inFlight || state.waiting) {
        return;
    }
    const QDateTime current = now();
    while (!state.entries.isEmpty() &&
            state.entries.first().createdAt.msecsTo(current) >
                    std::chrono::duration_cast<std::chrono::milliseconds>(kMaxAge).count()) {
        const Entry& expired = state.entries.first();
        kLogger.warning() << "Dropping a" << expired.kind << "write for the Crate server"
                          << serverId << "made on" << isoOf(expired.createdAt)
                          << ": not delivered in seven days";
        remove(serverId, QStringLiteral("expired"), false);
    }
    if (state.entries.isEmpty()) {
        return;
    }
    Client* pClient = m_options.clientFor ? m_options.clientFor(serverId) : nullptr;
    if (!pClient) {
        // Signed out or gone: it waits for flush()
        return;
    }
    send(serverId, pClient, state.entries.first());
}

void Outbox::send(const QString& serverId, Client* pClient, const Entry& entry) {
    const QJsonDocument body(entry.payload);
    if (entry.kind == outboxkind::kPlayEvent) {
        watch(serverId, pClient->post(kPlayEventsPath, body));
    } else if (entry.kind == outboxkind::kLike) {
        watch(serverId, pClient->post(kLikesPath, body));
    } else if (entry.kind == outboxkind::kUnlike) {
        watch(serverId, pClient->del(kLikesPath, body));
    } else if (entry.kind == outboxkind::kRate) {
        sendRate(serverId, pClient, entry);
    } else if (entry.kind == outboxkind::kCueSet || entry.kind == outboxkind::kCorrection) {
        sendPut(serverId, pClient, entry);
    } else {
        kLogger.warning() << "Dropping a write of unknown kind" << entry.kind;
        remove(serverId, QStringLiteral("unknown kind"), false);
        pump(serverId);
    }
}

void Outbox::sendRate(const QString& serverId, Client* pClient, const Entry& entry) {
    const qint64 trackId = entry.payload.value(kTrackIdKey).toInteger();
    const int rating = entry.payload.value(kRatingKey).toInt();
    if (trackId > 0) {
        watch(serverId,
                pClient->post(kRatePath,
                        QJsonDocument(QJsonObject{{kTrackIdKey, trackId}, {kRatingKey, rating}})));
        return;
    }
    // The endpoint takes the numeric id, which the track's info has
    watch(serverId,
            pClient->get(trackInfoPath(entry.payload.value(kEntityUidKey).toString())),
            [this, serverId](const JsonResult& result) {
                if (!result.ok()) {
                    apply(serverId, outcomeOf(result.error()), result.error().message);
                    return;
                }
                const qint64 trackId =
                        result.value().object().value(QStringLiteral("id")).toInteger();
                ServerState& state = m_servers[serverId];
                if (trackId <= 0 || state.entries.isEmpty()) {
                    apply(serverId, Outcome::Drop, QStringLiteral("the track's info has no id"));
                    return;
                }
                // Kept for the retries
                state.entries.first().payload.insert(kTrackIdKey, trackId);
                pump(serverId);
            });
}

void Outbox::sendPut(const QString& serverId, Client* pClient, const Entry& entry) {
    Reply* pReply = pClient->put(putPath(entry.kind, entry.payload.value(kEntityUidKey).toString()),
            QJsonDocument(entry.payload.value(kBodyKey).toObject()));
    watch(serverId,
            pReply,
            [this, serverId, pReply = QPointer<Reply>(pReply)](const JsonResult& result) {
                if (result.ok()) {
                    apply(serverId, Outcome::Delivered, QString(), result.value());
                    return;
                }
                const Error& error = result.error();
                ServerState& state = m_servers[serverId];
                if (error.kind != Error::Kind::Http || error.httpStatus != 409 ||
                        state.entries.isEmpty()) {
                    apply(serverId, outcomeOf(error), error.message);
                    return;
                }
                // The server has another revision: its sync resolves it
                const QJsonObject answer = pReply
                        ? QJsonDocument::fromJson(pReply->body()).object()
                        : QJsonObject();
                const Entry entry = state.entries.first();
                state.failures = 0;
                state.entries.removeFirst();
                storeDelete(entry.id);
                kLogger.info() << "A" << entry.kind << "write for the Crate server" << serverId
                               << "conflicts with revision"
                               << answer.value(kCurrentKey)
                                          .toObject()
                                          .value(QStringLiteral("revision"))
                                          .toInteger();
                emit conflicted(serverId, entry.kind, entry.payload, answer);
                pump(serverId);
            });
}

void Outbox::watch(const QString& serverId,
        Reply* pReply,
        const std::function<void(const JsonResult&)>& onFinished) {
    ServerState& state = m_servers[serverId];
    state.inFlight = true;
    state.pReply = pReply;
    connect(pReply, &Reply::finished, this, [this, serverId, onFinished](const JsonResult& result) {
        ServerState& state = m_servers[serverId];
        state.inFlight = false;
        state.pReply.clear();
        if (onFinished) {
            onFinished(result);
        } else if (result.ok()) {
            apply(serverId, Outcome::Delivered, QString(), result.value());
        } else {
            apply(serverId, outcomeOf(result.error()), result.error().message);
        }
    });
    // A Client deleted with its requests drops them without a signal
    connect(pReply, &QObject::destroyed, this, [this, serverId] {
        ServerState& state = m_servers[serverId];
        if (state.inFlight && !state.pReply) {
            state.inFlight = false;
            retryLater(serverId);
        }
    });
}

Outbox::Outcome Outbox::outcomeOf(const Error& error) {
    switch (error.kind) {
    case Error::Kind::Http: {
        const int status = error.httpStatus;
        if (status == 408 || status == 425 || status == 429 || status >= 500) {
            return Outcome::RetryAnswered;
        }
        return Outcome::Drop;
    }
    case Error::Kind::Decode:
        // The server answered, with something this client does not read
        return Outcome::Drop;
    case Error::Kind::Network:
    case Error::Kind::Auth:
    case Error::Kind::Cancelled:
    case Error::Kind::Storage:
        return Outcome::Retry;
    }
    return Outcome::Retry;
}

void Outbox::apply(const QString& serverId,
        Outcome outcome,
        const QString& reason,
        const QJsonDocument& answer) {
    ServerState& state = m_servers[serverId];
    if (state.entries.isEmpty()) {
        return;
    }
    switch (outcome) {
    case Outcome::Delivered:
        state.failures = 0;
        remove(serverId, QString(), true, answer);
        pump(serverId);
        return;
    case Outcome::Drop:
        kLogger.warning() << "The Crate server" << serverId << "refused a"
                          << state.entries.first().kind << "write, dropped:" << reason;
        state.failures = 0;
        remove(serverId, reason, false);
        pump(serverId);
        return;
    case Outcome::RetryAnswered:
        // Its backoff would reach the longest wait: the others go first
        if (state.failures >= static_cast<int>(kBackoff.size()) - 1 && setAside(serverId)) {
            kLogger.warning() << "The Crate server" << serverId << "keeps failing a"
                              << state.entries.last().kind << "write (" << reason
                              << "): the other writes go first";
            pump(serverId);
            return;
        }
        [[fallthrough]];
    case Outcome::Retry:
        kLogger.info() << "A" << state.entries.first().kind << "write for the Crate server"
                       << serverId << "failed, retrying later:" << reason;
        retryLater(serverId);
        return;
    }
}

QString Outbox::subjectOf(const Entry& entry) {
    if (entry.kind == outboxkind::kPlayEvent) {
        return entry.kind + QLatin1Char(':') + entry.id;
    }
    // A like and the unlike after it are one subject
    const QString family = entry.kind == outboxkind::kUnlike ? outboxkind::kLike : entry.kind;
    QString track = entry.payload.value(kEntityUidKey).toString();
    if (track.isEmpty()) {
        track = entry.payload.value(QStringLiteral("track_entity_uid")).toString();
    }
    return family + QLatin1Char(':') + track;
}

bool Outbox::setAside(const QString& serverId) {
    ServerState& state = m_servers[serverId];
    if (state.entries.isEmpty()) {
        return false;
    }
    const QString subject = subjectOf(state.entries.first());
    QList<Entry> others;
    QList<Entry> aside;
    for (const Entry& entry : std::as_const(state.entries)) {
        (subjectOf(entry) == subject ? aside : others).append(entry);
    }
    if (others.isEmpty()) {
        return false;
    }
    state.entries = others + aside;
    // The next entry starts its own backoff; this one starts again at its turn
    state.failures = 0;
    return true;
}

void Outbox::retryLater(const QString& serverId) {
    ServerState& state = m_servers[serverId];
    const auto index = static_cast<size_t>(std::min<int>(state.failures, kBackoff.size() - 1));
    ++state.failures;
    state.waiting = true;
    const quint64 generation = ++state.generation;
    m_options.schedule(kBackoff[index], [this, serverId, generation] {
        ServerState& state = m_servers[serverId];
        if (state.generation != generation) {
            return;
        }
        state.waiting = false;
        pump(serverId);
    });
}

void Outbox::remove(const QString& serverId,
        const QString& reason,
        bool delivered,
        const QJsonDocument& answer) {
    ServerState& state = m_servers[serverId];
    const Entry entry = state.entries.takeFirst();
    storeDelete(entry.id);
    if (delivered) {
        emit this->delivered(serverId, entry.kind, entry.payload, answer);
    } else {
        emit dropped(serverId, entry.kind, reason);
    }
}

void Outbox::storeInsert(const Entry& entry) {
    m_pool.start([pPool = m_options.pDbConnectionPool,
                         entry,
                         payload = QJsonDocument(entry.payload).toJson(QJsonDocument::Compact)] {
        withDatabase(pPool, [&](const QSqlDatabase& database) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral(
                    "INSERT OR REPLACE INTO crateserver_outbox "
                    "(id, server_id, kind, payload, created_at) "
                    "VALUES (:id, :server, :kind, :payload, :created)"));
            query.bindValue(QStringLiteral(":id"), entry.id);
            query.bindValue(QStringLiteral(":server"), entry.serverId);
            query.bindValue(QStringLiteral(":kind"), entry.kind);
            query.bindValue(QStringLiteral(":payload"), QString::fromUtf8(payload));
            query.bindValue(QStringLiteral(":created"), isoOf(entry.createdAt));
            if (!query.exec()) {
                kLogger.warning() << "Cannot store a write in the outbox:"
                                  << query.lastError().text();
            }
            return true;
        });
    });
}

void Outbox::storeDelete(const QString& id) {
    m_pool.start([pPool = m_options.pDbConnectionPool, id] {
        withDatabase(pPool, [&](const QSqlDatabase& database) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral("DELETE FROM crateserver_outbox WHERE id = :id"));
            query.bindValue(QStringLiteral(":id"), id);
            if (!query.exec()) {
                kLogger.warning() << "Cannot remove a write from the outbox:"
                                  << query.lastError().text();
            }
            return true;
        });
    });
}

} // namespace crate::writeback

#include "moc_outbox.cpp"
