#include "crate/analysis/profilestore.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUrlQuery>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

#include "crate/client/cratecapabilities.h"
#include "crate/client/crateclient.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::analysis {

namespace {

const mixxx::Logger kLogger("crate::analysis::ProfileStore");

const QString kBatchPath = QStringLiteral("/api/decks/mix-profiles");

// SQLITE_BUSY and SQLITE_LOCKED; the extended codes keep them in the low byte.
constexpr int kSqliteBusy = 5;
constexpr int kSqliteLocked = 6;

profiles::WriteStatus statusOf(const QSqlError& error) {
    bool ok = false;
    const int code = error.nativeErrorCode().toInt(&ok);
    if (ok && ((code & 0xff) == kSqliteBusy || (code & 0xff) == kSqliteLocked)) {
        return profiles::WriteStatus::Busy;
    }
    return profiles::WriteStatus::Failed;
}

QString now() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString singlePath(const QString& uid) {
    return QStringLiteral("/api/tracks/by-entity/%1/mix-profile")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(uid)));
}

/// Runs a write, again while the database is busy. On the worker thread.
template<typename Write>
profiles::WriteStatus withRetries(const ProfileStoreOptions& options, Write&& write) {
    profiles::WriteStatus status = profiles::WriteStatus::Busy;
    for (int attempt = 1; attempt <= std::max(1, options.writeAttempts); ++attempt) {
        status = write();
        if (status != profiles::WriteStatus::Busy) {
            return status;
        }
        QThread::msleep(static_cast<unsigned long>(options.writeRetryDelay.count() * attempt));
    }
    return status;
}

QJsonValue msValue(const std::optional<double>& ms) {
    return ms ? QJsonValue(*ms) : QJsonValue(QJsonValue::Null);
}

std::optional<double> msFrom(const QJsonValue& value) {
    return value.isDouble() ? std::optional<double>(value.toDouble()) : std::nullopt;
}

QJsonValue rangeValue(const std::optional<AppliedCues::Range>& range) {
    if (!range) {
        return QJsonValue(QJsonValue::Null);
    }
    return QJsonObject{
            {QStringLiteral("start_ms"), msValue(range->startMs)},
            {QStringLiteral("end_ms"), msValue(range->endMs)},
    };
}

std::optional<AppliedCues::Range> rangeFrom(const QJsonValue& value) {
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    return AppliedCues::Range{msFrom(object.value(QStringLiteral("start_ms"))),
            msFrom(object.value(QStringLiteral("end_ms")))};
}

/// What the worker makes of one answer.
struct Parsed {
    ProfileResults results;
    /// Every valid profile of the answer, also ones not asked for.
    QList<std::pair<MixProfile, QByteArray>> toStore;
};

/// `POST /api/decks/mix-profiles`: `{profiles: {uid: profile}, missing: [uid]}`.
Parsed parseBatch(const QByteArray& body, const QStringList& uids) {
    Parsed parsed;
    QJsonParseError parseError;
    const QJsonObject answer = QJsonDocument::fromJson(body, &parseError).object();
    const QJsonValue profilesValue = answer.value(QStringLiteral("profiles"));
    const QJsonValue missingValue = answer.value(QStringLiteral("missing"));
    if (parseError.error != QJsonParseError::NoError || !profilesValue.isObject() ||
            !missingValue.isArray()) {
        const Error error = Error::decode(QStringLiteral("Invalid batch of mix profiles"));
        for (const QString& uid : uids) {
            parsed.results.failed.insert(uid, error);
        }
        return parsed;
    }
    QHash<QString, Result<MixProfile>> byUid;
    const QJsonObject profiles = profilesValue.toObject();
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        const QString uid = it.key().toLower();
        Result<MixProfile> profile = parseMixProfile(it.value().toObject());
        if (profile.ok() && profile.value().entityUid != uid) {
            profile = Error::decode(QStringLiteral("A batch profile is keyed by another uid"));
        }
        if (profile.ok()) {
            parsed.toStore.append({profile.value(),
                    QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact)});
        }
        byUid.insert(uid, profile);
    }
    QSet<QString> missing;
    const QJsonArray missingUids = missingValue.toArray();
    for (const QJsonValue& value : missingUids) {
        missing.insert(value.toString().toLower());
    }
    for (const QString& uid : uids) {
        const auto it = byUid.constFind(uid);
        if (it != byUid.cend()) {
            if (it->ok()) {
                parsed.results.profiles.insert(uid, StoredProfile{it->value(), std::nullopt});
            } else {
                parsed.results.failed.insert(uid, it->error());
            }
        } else if (missing.contains(uid)) {
            parsed.results.missing.append(uid);
        } else {
            parsed.results.failed.insert(uid,
                    Error::decode(QStringLiteral("The batch answer leaves out a track")));
        }
    }
    return parsed;
}

/// `GET /api/tracks/by-entity/{uid}/mix-profile?detail=full`.
Parsed parseSingle(const QByteArray& body, const QString& uid) {
    Parsed parsed;
    Result<MixProfile> profile = parseMixProfile(body);
    if (profile.ok() && profile.value().entityUid != uid) {
        profile = Error::decode(QStringLiteral("The profile is of another track"));
    }
    if (!profile.ok()) {
        parsed.results.failed.insert(uid, profile.error());
        return parsed;
    }
    parsed.toStore.append({profile.value(), body});
    parsed.results.profiles.insert(uid, StoredProfile{profile.value(), std::nullopt});
    return parsed;
}

void merge(ProfileResults* pInto, const ProfileResults& from) {
    pInto->profiles.insert(from.profiles);
    pInto->missing += from.missing;
    pInto->failed.insert(from.failed);
}

} // namespace

struct ProfileStore::Fetch {
    QPointer<ProfileFetch> pFetch;
    ProfileResults results;
    /// The uids in the order they were asked, for `missing`.
    QStringList uids;
    /// The lookup and the requests that have not finished.
    int pending = 1;
};

struct ProfileStore::Request {
    quint64 fetchId = 0;
    bool batch = false;
    QStringList uids;
    /// The cues applied from the stored, now stale, profiles.
    QHash<QString, AppliedCues> appliedCues;
    QPointer<Reply> pReply;
};

ProfileStore::ProfileStore(const QString& serverId,
        Client* pClient,
        mixxx::DbConnectionPoolPtr pDbConnectionPool,
        Options options,
        QObject* parent)
        : QObject(parent),
          m_serverId(serverId),
          m_pClient(pClient),
          m_pDbConnectionPool(std::move(pDbConnectionPool)),
          m_options(std::move(options)) {
    m_pool.setMaxThreadCount(1);
}

ProfileStore::~ProfileStore() {
    // No answer reaches this object from here on.
    for (const auto& pRequest : std::as_const(m_inFlight)) {
        if (pRequest->pReply) {
            disconnect(pRequest->pReply, nullptr, this, nullptr);
            pRequest->pReply->cancel();
        }
    }
    m_pool.waitForDone();
}

void ProfileStore::setBatchAvailable(bool available) {
    m_batchAvailable = available;
}

void ProfileStore::setCapabilities(const Capabilities& capabilities) {
    setBatchAvailable(capabilities.has(DecksFeature::ProfilesBatch));
}

template<typename Result, typename Task, typename Done>
void ProfileStore::inWorker(Task&& task, Done&& done) {
    auto* pWatcher = new QFutureWatcher<Result>(this);
    connect(pWatcher,
            &QFutureWatcherBase::finished,
            this,
            [pWatcher, done = std::forward<Done>(done)]() mutable {
                pWatcher->deleteLater();
                done(pWatcher->result());
            });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_pDbConnectionPool, task = std::forward<Task>(task)]() mutable -> Result {
                const mixxx::DbConnectionPooler pooler(pPool);
                const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
                return task(database);
            }));
}

ProfileFetch* ProfileStore::get(const QStringList& entityUids) {
    QStringList uids;
    for (const QString& entityUid : entityUids) {
        const QString uid = entityUid.trimmed().toLower();
        if (!uid.isEmpty() && !uids.contains(uid)) {
            uids.append(uid);
        }
    }
    auto* pFetch = new ProfileFetch(this);
    const quint64 fetchId = m_nextFetchId++;
    auto pState = std::make_unique<Fetch>();
    pState->pFetch = pFetch;
    pState->uids = uids;
    m_fetches.emplace(fetchId, std::move(pState));

    inWorker<profiles::Lookup>(
            [serverId = m_serverId, uids](const QSqlDatabase& database) {
                return profiles::lookup(database, serverId, uids);
            },
            [this, fetchId](const profiles::Lookup& found) {
                const auto it = m_fetches.find(fetchId);
                if (it == m_fetches.end()) {
                    return;
                }
                Fetch* pState = it->second.get();
                pState->results.profiles.insert(found.current);
                pState->results.missing += found.missing;
                enqueue(fetchId, found.stale, found.staleAppliedCues);
                --pState->pending;
                finishFetchIfDone(fetchId);
                pump();
            });
    return pFetch;
}

void ProfileStore::enqueue(quint64 fetchId,
        const QStringList& uids,
        const QHash<QString, AppliedCues>& appliedCues) {
    if (uids.isEmpty()) {
        return;
    }
    Fetch* pState = m_fetches.at(fetchId).get();
    const auto add = [&](bool batch, const QStringList& part) {
        auto pRequest = std::make_shared<Request>();
        pRequest->fetchId = fetchId;
        pRequest->batch = batch;
        pRequest->uids = part;
        for (const QString& uid : part) {
            const auto it = appliedCues.constFind(uid);
            if (it != appliedCues.cend()) {
                pRequest->appliedCues.insert(uid, it.value());
            }
        }
        m_queue.append(pRequest);
        ++pState->pending;
    };
    if (m_batchAvailable) {
        const int size = std::clamp(m_options.batchSize, 1, 200);
        for (int first = 0; first < uids.size(); first += size) {
            add(true, uids.mid(first, size));
        }
    } else {
        for (const QString& uid : uids) {
            add(false, {uid});
        }
    }
}

void ProfileStore::pump() {
    while (!m_queue.isEmpty()) {
        // A batch is up to 200 profiles of work for the server: one at a
        // time. Single requests go a few at a time.
        const int limit = m_queue.first()->batch ? 1 : std::max(1, m_options.maxConcurrentRequests);
        if (m_inFlight.size() >= limit) {
            return;
        }
        const std::shared_ptr<Request> pRequest = m_queue.takeFirst();
        m_inFlight.append(pRequest);
        send(pRequest);
    }
}

void ProfileStore::send(const std::shared_ptr<Request>& pRequest) {
    if (!m_pClient) {
        finishRequest(pRequest,
                Error::network(QStringLiteral("The Crate server is gone")),
                ProfileResults());
        return;
    }
    RequestOptions options;
    options.rawBody = true;
    Reply* pReply = nullptr;
    if (pRequest->batch) {
        QJsonObject body;
        body.insert(QStringLiteral("entity_uids"), QJsonArray::fromStringList(pRequest->uids));
        body.insert(QStringLiteral("detail"), QStringLiteral("full"));
        pReply = m_pClient->post(kBatchPath, QJsonDocument(body), options);
    } else {
        options.query.addQueryItem(QStringLiteral("detail"), QStringLiteral("full"));
        pReply = m_pClient->get(singlePath(pRequest->uids.first()), options);
    }
    pRequest->pReply = pReply;
    connect(pReply, &Reply::finished, this, [this, pRequest, pReply](const JsonResult& result) {
        onReply(pRequest, pReply, result);
    });
}

void ProfileStore::onReply(const std::shared_ptr<Request>& pRequest,
        Reply* pReply,
        const JsonResult& result) {
    pRequest->pReply = nullptr;
    if (!result.ok()) {
        const Error& error = result.error();
        if (pRequest->batch && error.kind == Error::Kind::Http &&
                (error.httpStatus == 404 || error.httpStatus == 405)) {
            // A server that announced the batch and does not serve it, e.g.
            // from capabilities remembered before a downgrade.
            kLogger.warning() << "The Crate server does not serve batch profiles; "
                                 "asking one by one";
            m_batchAvailable = false;
            m_inFlight.removeOne(pRequest);
            Fetch* pState = m_fetches.at(pRequest->fetchId).get();
            --pState->pending;
            enqueue(pRequest->fetchId, pRequest->uids, pRequest->appliedCues);
            pump();
            return;
        }
        if (!pRequest->batch && error.kind == Error::Kind::Http && error.httpStatus == 404) {
            ProfileResults results;
            results.missing = pRequest->uids;
            finishRequest(pRequest, std::nullopt, results);
            return;
        }
        finishRequest(pRequest, error, ProfileResults());
        return;
    }
    inWorker<ProfileResults>(
            [body = pReply->body(),
                    pRequest,
                    serverId = m_serverId,
                    options = m_options](const QSqlDatabase& database) {
                Parsed parsed = pRequest->batch ? parseBatch(body, pRequest->uids)
                                                : parseSingle(body, pRequest->uids.first());
                const profiles::WriteStatus status = withRetries(options, [&] {
                    return profiles::storeProfiles(database, serverId, parsed.toStore, now());
                });
                if (status != profiles::WriteStatus::Ok) {
                    // The profiles are still good for this load.
                    kLogger.warning() << "Cannot store" << parsed.toStore.size()
                                      << "mix profiles";
                }
                for (auto it = parsed.results.profiles.begin();
                        it != parsed.results.profiles.end();
                        ++it) {
                    const auto applied = pRequest->appliedCues.constFind(it.key());
                    if (applied != pRequest->appliedCues.cend()) {
                        it->appliedCues = applied.value();
                    }
                }
                return parsed.results;
            },
            [this, pRequest](const ProfileResults& results) {
                finishRequest(pRequest, std::nullopt, results);
            });
}

void ProfileStore::finishRequest(const std::shared_ptr<Request>& pRequest,
        const std::optional<Error>& error,
        const ProfileResults& results) {
    m_inFlight.removeOne(pRequest);
    const auto it = m_fetches.find(pRequest->fetchId);
    if (it != m_fetches.end()) {
        Fetch* pState = it->second.get();
        if (error) {
            for (const QString& uid : std::as_const(pRequest->uids)) {
                pState->results.failed.insert(uid, *error);
            }
        }
        merge(&pState->results, results);
        --pState->pending;
        finishFetchIfDone(pRequest->fetchId);
    }
    pump();
}

void ProfileStore::finishFetchIfDone(quint64 fetchId) {
    const auto it = m_fetches.find(fetchId);
    if (it == m_fetches.end() || it->second->pending > 0) {
        return;
    }
    std::unique_ptr<Fetch> pState = std::move(it->second);
    m_fetches.erase(it);
    // `missing` in the order the uids were asked.
    QStringList missing;
    for (const QString& uid : std::as_const(pState->uids)) {
        if (pState->results.missing.contains(uid)) {
            missing.append(uid);
        }
    }
    pState->results.missing = missing;
    if (pState->pFetch) {
        emit pState->pFetch->finished(pState->results);
        pState->pFetch->deleteLater();
    }
}

void ProfileStore::recordAppliedCues(const QString& entityUid, const AppliedCues& appliedCues) {
    inWorker<profiles::WriteStatus>(
            [serverId = m_serverId,
                    uid = entityUid.trimmed().toLower(),
                    appliedCues,
                    options = m_options](const QSqlDatabase& database) {
                return withRetries(options, [&] {
                    return profiles::storeAppliedCues(database, serverId, uid, appliedCues);
                });
            },
            [this](profiles::WriteStatus status) {
                if (status != profiles::WriteStatus::Ok) {
                    emit writeFailed(Error::storage(
                            QStringLiteral("Cannot store the cues set from a mix profile")));
                }
            });
}

namespace profiles {

Lookup lookup(const QSqlDatabase& database, const QString& serverId, const QStringList& uids) {
    Lookup found;
    QSqlQuery catalogue(database);
    catalogue.prepare(QStringLiteral(
            "SELECT profile_revision FROM crateserver_tracks "
            "WHERE server_id = :server AND entity_uid = :uid"));
    QSqlQuery stored(database);
    stored.prepare(QStringLiteral(
            "SELECT profile_revision, json, applied_cues FROM crateserver_profiles "
            "WHERE server_id = :server AND entity_uid = :uid"));
    for (const QString& uid : uids) {
        catalogue.bindValue(QStringLiteral(":server"), serverId);
        catalogue.bindValue(QStringLiteral(":uid"), uid);
        bool listed = false;
        QString currentRevision;
        if (catalogue.exec() && catalogue.next()) {
            listed = true;
            currentRevision = catalogue.value(0).toString();
        }
        if (listed && currentRevision.isEmpty()) {
            // The library sync says the track has no current profile.
            found.missing.append(uid);
            continue;
        }
        stored.bindValue(QStringLiteral(":server"), serverId);
        stored.bindValue(QStringLiteral(":uid"), uid);
        std::optional<AppliedCues> appliedCues;
        if (stored.exec() && stored.next()) {
            appliedCues = appliedCuesFromJson(stored.value(2).toByteArray());
            if (listed && stored.value(0).toString() == currentRevision) {
                const Result<MixProfile> profile = parseMixProfile(stored.value(1).toByteArray());
                if (profile.ok()) {
                    found.current.insert(uid, StoredProfile{profile.value(), appliedCues});
                    continue;
                }
            }
        }
        found.stale.append(uid);
        if (appliedCues) {
            found.staleAppliedCues.insert(uid, *appliedCues);
        }
    }
    return found;
}

WriteStatus storeProfiles(const QSqlDatabase& database,
        const QString& serverId,
        const QList<std::pair<MixProfile, QByteArray>>& profilesWithJson,
        const QString& now) {
    if (profilesWithJson.isEmpty()) {
        return WriteStatus::Ok;
    }
    QSqlDatabase db = database;
    if (!db.transaction()) {
        return statusOf(db.lastError());
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT INTO crateserver_profiles "
            "(server_id, entity_uid, profile_revision, json, fetched_at) "
            "VALUES (:server, :uid, :revision, :json, :now) "
            "ON CONFLICT (server_id, entity_uid) DO UPDATE SET "
            "profile_revision = excluded.profile_revision, json = excluded.json, "
            "fetched_at = excluded.fetched_at"));
    for (const auto& [profile, json] : profilesWithJson) {
        query.bindValue(QStringLiteral(":server"), serverId);
        query.bindValue(QStringLiteral(":uid"), profile.entityUid);
        query.bindValue(QStringLiteral(":revision"), profile.profileRevision);
        query.bindValue(QStringLiteral(":json"), QString::fromUtf8(json));
        query.bindValue(QStringLiteral(":now"), now);
        if (!query.exec()) {
            const QSqlError error = query.lastError();
            db.rollback();
            const WriteStatus status = statusOf(error);
            if (status == WriteStatus::Failed) {
                kLogger.warning() << "Cannot store mix profiles:" << error.text();
            }
            return status;
        }
    }
    if (!db.commit()) {
        const QSqlError error = db.lastError();
        db.rollback();
        return statusOf(error);
    }
    return WriteStatus::Ok;
}

WriteStatus storeAppliedCues(const QSqlDatabase& database,
        const QString& serverId,
        const QString& entityUid,
        const AppliedCues& appliedCues) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "UPDATE crateserver_profiles SET applied_cues = :cues "
            "WHERE server_id = :server AND entity_uid = :uid"));
    query.bindValue(QStringLiteral(":cues"), QString::fromUtf8(appliedCuesToJson(appliedCues)));
    query.bindValue(QStringLiteral(":server"), serverId);
    query.bindValue(QStringLiteral(":uid"), entityUid);
    if (!query.exec()) {
        return statusOf(query.lastError());
    }
    if (query.numRowsAffected() < 1) {
        kLogger.warning() << "No stored mix profile for the cues of" << entityUid;
        return WriteStatus::Failed;
    }
    return WriteStatus::Ok;
}

QByteArray appliedCuesToJson(const AppliedCues& appliedCues) {
    const QJsonObject object{
            {QStringLiteral("profile_revision"), appliedCues.profileRevision},
            {QStringLiteral("intro"), rangeValue(appliedCues.intro)},
            {QStringLiteral("outro"), rangeValue(appliedCues.outro)},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<AppliedCues> appliedCuesFromJson(const QByteArray& json) {
    if (json.isEmpty()) {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    AppliedCues appliedCues;
    appliedCues.profileRevision = object.value(QStringLiteral("profile_revision")).toString();
    appliedCues.intro = rangeFrom(object.value(QStringLiteral("intro")));
    appliedCues.outro = rangeFrom(object.value(QStringLiteral("outro")));
    return appliedCues;
}

} // namespace profiles

} // namespace crate::analysis

#include "moc_profilestore.cpp"
