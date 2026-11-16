#include "crate/recs/crateradiosource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <utility>

#include "crate/client/crateclient.h"
#include "crate/recs/recommendationqueue.h"
#include "moc_crateradiosource.cpp"
#include "util/logger.h"

namespace crate::recs {

namespace {

const mixxx::Logger kLogger("crate::recs::CrateRadioSource");

const QString kStartPath = QStringLiteral("/api/radio/start");
const QString kNextPath = QStringLiteral("/api/radio/next");
const QString kFeedbackPath = QStringLiteral("/api/radio/feedback");
/// The most tracks one batch of the server holds.
constexpr int kServerMaxCount = 50;
/// The most `buffer_refs` the server takes.
constexpr int kServerMaxBufferRefs = 200;

QString sessionPath(const QString& sessionId) {
    return QStringLiteral("/api/radio/session/") +
            QString::fromLatin1(QUrl::toPercentEncoding(sessionId));
}

QString keyOf(const QString& entityUid) {
    return entityUid.toLower();
}

} // namespace

RadioSeed RadioSeed::track(const QString& entityUid, const QString& label) {
    return RadioSeed{QStringLiteral("track"), entityUid, label};
}

std::optional<RadioSeed> RadioSeed::container(
        const QString& kind, const QString& remoteId, const QString& label) {
    // The S3 containers' ids are what the radio's seeds take: a playlist's
    // number and a crate's uuid
    if (kind == QLatin1String("playlist") || kind == QLatin1String("crate")) {
        return RadioSeed{kind, remoteId, label};
    }
    return std::nullopt;
}

CrateRadioSource::CrateRadioSource(
        RecommendationQueue* pQueue, Options options, QObject* parent)
        : QObject(parent),
          m_pQueue(pQueue),
          m_options(std::move(options)) {
    if (!m_options.newSessionId) {
        m_options.newSessionId = [] {
            return QUuid::createUuid().toString(QUuid::WithoutBraces);
        };
    }
    if (!m_options.schedule) {
        m_options.schedule = [this](std::chrono::milliseconds delay,
                                     const std::function<void()>& callback) {
            QTimer::singleShot(delay, this, callback);
        };
    }
    connect(m_pQueue,
            &RecommendationQueue::queueChanged,
            this,
            &CrateRadioSource::onQueueChanged);
}

CrateRadioSource::~CrateRadioSource() {
    cancelReply();
}

void CrateRadioSource::start(Client* pClient, const QString& serverId, const RadioSeed& seed) {
    if (isRunning()) {
        finish(StopReason::Stopped);
    }
    if (!pClient) {
        emit stopped(StopReason::Failed);
        return;
    }
    ++m_session;
    m_pClient = pClient;
    m_serverId = serverId;
    m_seed = seed;
    m_sessionId = m_options.newSessionId();
    m_label = seed.label;
    m_batchIndex.reset();
    m_tracks.clear();
    m_trackIds.clear();
    m_ahead = 0;
    emit changed();
    requestBatch(std::max(1, m_options.ahead), {});
}

void CrateRadioSource::stop() {
    if (isRunning()) {
        finish(StopReason::Stopped);
    }
}

bool CrateRadioSource::hasTrack(const QString& serverId, const QString& entityUid) const {
    return isRunning() && serverId == m_serverId && m_trackIds.contains(keyOf(entityUid));
}

bool CrateRadioSource::sendFeedback(const QString& entityUid, Feedback feedback) {
    if (!isRunning() || !m_pClient || !m_trackIds.contains(keyOf(entityUid))) {
        return false;
    }
    const qint64 trackId = m_trackIds.value(keyOf(entityUid));
    if (trackId <= 0) {
        return false;
    }
    const QJsonObject body{
            {QStringLiteral("session_id"), m_sessionId},
            {QStringLiteral("track_id"), trackId},
            {QStringLiteral("action"),
                    feedback == Feedback::Like ? QStringLiteral("like")
                                               : QStringLiteral("dislike")},
    };
    Reply* pReply = m_pClient->post(kFeedbackPath, QJsonDocument(body));
    connect(pReply, &Reply::finished, this, [](const JsonResult& result) {
        if (!result.ok()) {
            kLogger.warning() << "The radio did not take the feedback:" << result.error().message;
        }
    });
    return true;
}

void CrateRadioSource::requestBatch(int count, const QStringList& bufferRefs) {
    if (!m_pClient) {
        finish(StopReason::Failed);
        return;
    }
    const int asked = std::clamp(count, 1, kServerMaxCount);
    QJsonObject body;
    QString path;
    if (!m_batchIndex) {
        // The indexed start: our session id, so that a retry gets the same
        // batch
        path = kStartPath;
        body = QJsonObject{
                {QStringLiteral("mode"), QStringLiteral("seeded")},
                {QStringLiteral("seed_type"), m_seed.type},
                {QStringLiteral("seed_value"), m_seed.value},
                {QStringLiteral("session_id"), m_sessionId},
                {QStringLiteral("count"), asked},
        };
    } else {
        path = kNextPath;
        body = QJsonObject{
                {QStringLiteral("session_id"), m_sessionId},
                {QStringLiteral("count"), asked},
                {QStringLiteral("batch_index"), *m_batchIndex + 1},
        };
        // What is still queued, so that the server steers away from it
        body.insert(QStringLiteral("buffer_refs"),
                QJsonArray::fromStringList(bufferRefs.mid(0, kServerMaxBufferRefs)));
    }
    RequestOptions options;
    options.rawBody = true;
    Reply* pReply = m_pClient->post(path, QJsonDocument(body), options);
    m_pReply = pReply;
    const quint64 session = m_session;
    connect(pReply, &Reply::finished, this, [this, pReply, session](const JsonResult& result) {
        if (m_pReply == pReply) {
            m_pReply = nullptr;
        }
        onBatch(session, result, pReply->body());
    });
}

void CrateRadioSource::onBatch(quint64 session, const JsonResult& result, const QByteArray& body) {
    if (session != m_session || !isRunning()) {
        return;
    }
    const auto retry = [this, session] {
        m_retryPending = true;
        m_options.schedule(m_options.retryDelay, [this, session] {
            m_retryPending = false;
            if (session == m_session && isRunning()) {
                readQueue();
            }
        });
    };
    if (!result.ok()) {
        const Error& error = result.error();
        if (error.kind == Error::Kind::Cancelled) {
            return;
        }
        // Busy with another request of the session, or the network: the
        // same batch again, later
        if (error.kind == Error::Kind::Network ||
                (error.kind == Error::Kind::Http &&
                        (error.httpStatus == 409 || error.httpStatus == 429 ||
                                error.httpStatus >= 500))) {
            retry();
            return;
        }
        kLogger.warning() << "The radio stopped:" << error.message;
        finish(StopReason::Failed);
        return;
    }
    const Result<RadioBatch> batch = parseRadioBatch(body);
    if (!batch.ok()) {
        kLogger.warning() << "Cannot read the radio's tracks:" << batch.error().message;
        finish(StopReason::Failed);
        return;
    }
    const RadioBatch& value = batch.value();
    if (!value.seedLabel.isEmpty() && value.seedLabel != m_label) {
        m_label = value.seedLabel;
        emit changed();
    }
    if (value.status == QLatin1String("exhausted") || value.exhausted) {
        finish(StopReason::Exhausted);
        return;
    }
    if (value.status == QLatin1String("auth_required")) {
        finish(StopReason::Failed);
        return;
    }
    // The server's last committed batch, also when it had nothing new
    if (value.batchIndex) {
        m_batchIndex = *value.batchIndex;
    } else if (!m_batchIndex) {
        m_batchIndex = 0;
    }
    if (value.status == QLatin1String("retryable_empty")) {
        retry();
        return;
    }
    QList<RecommendedTrack> fresh;
    for (const RecommendedTrack& track : value.tracks) {
        if (m_trackIds.contains(keyOf(track.entityUid))) {
            continue;
        }
        m_trackIds.insert(keyOf(track.entityUid), track.trackId);
        m_tracks.append(track);
        fresh.append(track);
    }
    if (fresh.isEmpty()) {
        retry();
        return;
    }
    // A read of the queue started before these tracks is out of date
    ++m_queueGeneration;
    m_ahead += static_cast<int>(fresh.size());
    m_pQueue->enqueue(m_serverId, fresh);
    emit tracksQueued(fresh);
    emit changed();
    readQueue();
}

void CrateRadioSource::onQueueChanged() {
    if (isRunning()) {
        readQueue();
    }
}

void CrateRadioSource::readQueue() {
    if (m_reading) {
        m_readAgain = true;
        return;
    }
    m_reading = true;
    m_readAgain = false;
    const quint64 session = m_session;
    const quint64 generation = m_queueGeneration;
    QPointer<CrateRadioSource> self(this);
    m_pQueue->readQueue([self, session, generation](const QList<cache::TrackKey>& queued) {
        if (self) {
            self->onQueue(session, generation, queued);
        }
    });
}

void CrateRadioSource::onQueue(
        quint64 session, quint64 generation, const QList<cache::TrackKey>& queued) {
    m_reading = false;
    if (session != m_session || !isRunning()) {
        return;
    }
    if (generation != m_queueGeneration || m_readAgain) {
        readQueue();
        return;
    }
    QStringList ahead;
    for (const cache::TrackKey& track : queued) {
        if (track.serverId == m_serverId && m_trackIds.contains(keyOf(track.entityUid))) {
            ahead.append(track.entityUid);
        }
    }
    const int before = m_ahead;
    m_ahead = static_cast<int>(ahead.size());
    // Auto DJ takes one track at a time: all of them gone at once is the DJ
    // clearing the queue
    if (m_ahead == 0 && before >= 2) {
        finish(StopReason::QueueCleared);
        return;
    }
    if (m_pReply || m_retryPending) {
        return;
    }
    const int wanted = std::max(1, m_options.ahead) - m_ahead;
    if (wanted > 0) {
        requestBatch(wanted, ahead);
    }
}

void CrateRadioSource::finish(StopReason reason) {
    cancelReply();
    const QString sessionId = m_sessionId;
    const QPointer<Client> pClient = m_pClient;
    ++m_session;
    m_sessionId.clear();
    m_retryPending = false;
    m_reading = false;
    m_readAgain = false;
    m_ahead = 0;
    if (pClient && !sessionId.isEmpty() && reason != StopReason::Failed) {
        // The server keeps a session for a day: end it now
        Reply* pReply = pClient->del(sessionPath(sessionId));
        connect(pReply, &Reply::finished, this, [](const JsonResult& result) {
            if (!result.ok()) {
                kLogger.debug() << "The radio session did not end:" << result.error().message;
            }
        });
    }
    emit stopped(reason);
    emit changed();
}

void CrateRadioSource::cancelReply() {
    if (Reply* pReply = m_pReply.data()) {
        m_pReply = nullptr;
        pReply->disconnect(this);
        pReply->cancel();
    }
}

} // namespace crate::recs
