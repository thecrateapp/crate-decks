#include "crate/writeback/historyreporter.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUuid>
#include <algorithm>

#include "crate/client/crateclient.h"
#include "crate/writeback/outbox.h"
#include "track/track.h"
#include "util/logger.h"

namespace crate::writeback {

namespace {

const mixxx::Logger kLogger("crate::writeback::HistoryReporter");

const QString kNowPlayingPath = QStringLiteral("/api/me/now-playing");
/// What Decks calls itself in the server's play events and now playing (S6).
const QString kDecks = QStringLiteral("decks");
/// The server takes decks 1 to 4.
constexpr int kMaxDeck = 4;

QString isoOf(const QDateTime& time) {
    return time.toUTC().toString(Qt::ISODateWithMs);
}

} // namespace

HistoryReporter::HistoryReporter(Options options, QObject* pParent)
        : QObject(pParent),
          m_options(std::move(options)) {
    if (m_options.mixSessionId.isEmpty()) {
        m_options.mixSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!m_options.schedule) {
        m_options.schedule = [this](std::chrono::milliseconds delay,
                                     const std::function<void()>& callback) {
            QTimer::singleShot(delay, this, callback);
        };
    }
}

HistoryReporter::~HistoryReporter() {
    finish();
}

QDateTime HistoryReporter::now() const {
    return (m_options.now ? m_options.now() : QDateTime::currentDateTimeUtc()).toUTC();
}

void HistoryReporter::setPlayingDeck(int deckIndex) {
    m_deckIndex = deckIndex;
}

void HistoryReporter::setPlayingTrack(const TrackPointer& pTrack) {
    if (!pTrack) {
        // As upstream's history: nothing on air does not end the track
        pause(true);
        return;
    }
    if (m_current && m_current->pTrack == pTrack) {
        resume();
        return;
    }
    std::optional<cache::TrackKey> crateTrack;
    if (m_options.trackOfFile) {
        crateTrack = m_options.trackOfFile(pTrack->getLocation());
    }
    // Another Crate track of the same server replaces the now playing
    // state: no need to clear it. Another server's does not.
    const bool sameServer = crateTrack && m_current && m_current->crateTrack &&
            m_current->crateTrack->serverId == crateTrack->serverId;
    end(!sameServer);
    begin(pTrack, crateTrack);
}

void HistoryReporter::finish() {
    end(true);
}

void HistoryReporter::begin(
        const TrackPointer& pTrack, const std::optional<cache::TrackKey>& crateTrack) {
    Current current;
    current.pTrack = pTrack;
    current.crateTrack = crateTrack;
    current.startedAt = now();
    current.onAirSince = current.startedAt;
    m_current = std::move(current);
    if (m_current->crateTrack) {
        sendNowPlaying();
        scheduleNowPlaying();
    }
}

void HistoryReporter::pause(bool clearState) {
    if (!m_current || m_current->onAirSince.isNull()) {
        return;
    }
    const QDateTime time = now();
    m_current->playedMs += std::max<qint64>(0, m_current->onAirSince.msecsTo(time));
    m_current->onAirSince = QDateTime();
    m_current->leftAirAt = time;
    ++m_heartbeat;
    if (clearState && m_current->crateTrack) {
        clearNowPlaying();
    }
}

void HistoryReporter::resume() {
    if (!m_current || !m_current->onAirSince.isNull()) {
        return;
    }
    m_current->onAirSince = now();
    m_current->leftAirAt = QDateTime();
    if (m_current->crateTrack) {
        sendNowPlaying();
        scheduleNowPlaying();
    }
}

void HistoryReporter::end(bool clearState) {
    if (!m_current) {
        return;
    }
    pause(clearState);
    const Current ended = std::move(*m_current);
    m_current.reset();
    if (!ended.crateTrack || !m_options.pOutbox) {
        return;
    }
    const double playedSeconds = static_cast<double>(ended.playedMs) / 1000.0;
    if (ended.playedMs <
            std::chrono::duration_cast<std::chrono::milliseconds>(m_options.minimumPlayed)
                    .count()) {
        return;
    }
    const Track& track = *ended.pTrack;
    QJsonObject event{
            {QStringLiteral("client_event_id"),
                    QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("track_entity_uid"), ended.crateTrack->entityUid},
            {QStringLiteral("title"), track.getTitle()},
            {QStringLiteral("artist"), track.getArtist()},
            {QStringLiteral("album"), track.getAlbum()},
            {QStringLiteral("started_at"), isoOf(ended.startedAt)},
            {QStringLiteral("ended_at"), isoOf(ended.leftAirAt)},
            {QStringLiteral("played_seconds"), playedSeconds},
            {QStringLiteral("device_type"), kDecks},
            {QStringLiteral("app_platform"), kDecks},
            {QStringLiteral("play_source_type"), kDecks},
    };
    const double duration = track.getDuration();
    if (duration > 0) {
        event.insert(QStringLiteral("track_duration_seconds"), duration);
        event.insert(QStringLiteral("completion_ratio"), std::min(1.0, playedSeconds / duration));
    }
    m_options.pOutbox->enqueue(ended.crateTrack->serverId, outboxkind::kPlayEvent, event);
}

Client* HistoryReporter::clientOf(const Current& current) const {
    if (!current.crateTrack || !m_options.clientFor) {
        return nullptr;
    }
    return m_options.clientFor(current.crateTrack->serverId);
}

void HistoryReporter::sendNowPlaying() {
    Client* pClient = m_current ? clientOf(*m_current) : nullptr;
    if (!pClient) {
        return;
    }
    const Track& track = *m_current->pTrack;
    QJsonObject body{
            {QStringLiteral("playing"), true},
            {QStringLiteral("track_entity_uid"), m_current->crateTrack->entityUid},
            {QStringLiteral("title"), track.getTitle()},
            {QStringLiteral("artist"), track.getArtist()},
            {QStringLiteral("album"), track.getAlbum()},
            {QStringLiteral("started_at"), isoOf(m_current->startedAt)},
            {QStringLiteral("device_type"), kDecks},
            {QStringLiteral("app_platform"), kDecks},
            {QStringLiteral("mix_session_id"), m_options.mixSessionId},
    };
    const int deck = m_deckIndex + 1;
    if (deck >= 1 && deck <= kMaxDeck) {
        body.insert(QStringLiteral("deck"), deck);
    }
    Reply* pReply = pClient->post(kNowPlayingPath, QJsonDocument(body));
    connect(pReply, &Reply::finished, this, [](const JsonResult& result) {
        if (!result.ok()) {
            kLogger.debug() << "Now playing not sent:" << result.error().message;
        }
    });
}

void HistoryReporter::clearNowPlaying() {
    Client* pClient = m_current ? clientOf(*m_current) : nullptr;
    if (!pClient) {
        return;
    }
    Reply* pReply = pClient->post(
            kNowPlayingPath, QJsonDocument(QJsonObject{{QStringLiteral("playing"), false}}));
    connect(pReply, &Reply::finished, this, [](const JsonResult& result) {
        if (!result.ok()) {
            kLogger.debug() << "Now playing not cleared:" << result.error().message;
        }
    });
}

void HistoryReporter::scheduleNowPlaying() {
    const quint64 heartbeat = ++m_heartbeat;
    m_options.schedule(m_options.nowPlayingInterval, [this, heartbeat] {
        if (heartbeat != m_heartbeat || !m_current || m_current->onAirSince.isNull()) {
            return;
        }
        sendNowPlaying();
        scheduleNowPlaying();
    });
}

} // namespace crate::writeback

#include "moc_historyreporter.cpp"
