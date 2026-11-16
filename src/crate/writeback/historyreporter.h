#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <chrono>
#include <functional>
#include <optional>

#include "crate/cache/audiocache.h"
#include "track/track_decl.h"

namespace crate {

class Client;

namespace writeback {

class Outbox;

/// Tells a Crate server what Decks plays (plan F7.C-PR1, server S6): a play
/// event for each Crate track that played long enough, and the track on air
/// while it plays.
///
/// It follows PlayerInfo::currentPlayingTrackChanged, as the history of
/// upstream's SetlogFeature::slotPlayingTrackChanged does: a track begins
/// when it becomes the one on air, a null track (nothing on air) does not
/// end it, and the same track coming back on air goes on with it. When
/// another track takes the air, the previous one gets its play event:
/// `started_at` when it first took the air, `ended_at` when it last left
/// it, and `played_seconds` the time it was on air in between. A track on
/// air for less than the threshold (30 s by default) gets none, and neither
/// does a track that is not Crate's (its file is not in the audio cache).
///
/// The events go through the Outbox, each with a random UUID as
/// `client_event_id` that its retries keep, so the server counts it once.
/// They carry `device_type`, `app_platform` and `play_source_type`
/// `decks`. The now playing state (`POST /api/me/now-playing`) goes straight
/// to the server when a Crate track takes the air and every 60 s while it
/// stays, with the deck (1-4) and the run's mix session; when it leaves the
/// air, `{"playing": false}` clears it. Losing a heartbeat is harmless: the
/// server lets the state expire after 90 s.
class HistoryReporter : public QObject {
    Q_OBJECT
  public:
    using Schedule = std::function<void(
            std::chrono::milliseconds delay, const std::function<void()>& callback)>;

    struct Options {
        /// Not owned; required.
        Outbox* pOutbox = nullptr;
        /// The Crate track whose cached file is at `location`, if any.
        std::function<std::optional<cache::TrackKey>(const QString& location)> trackOfFile;
        /// The Client of a server, for now playing; nullptr while it has
        /// none.
        std::function<Client*(const QString& serverId)> clientFor;
        /// Defaults to a single-shot QTimer.
        Schedule schedule;
        /// Defaults to the system's UTC time.
        std::function<QDateTime()> now;
        /// A track on air for less than this gets no play event.
        std::chrono::seconds minimumPlayed{30};
        std::chrono::seconds nowPlayingInterval{60};
        /// Sent as `mix_session_id`; a new UUID when empty.
        QString mixSessionId;
    };

    explicit HistoryReporter(Options options, QObject* pParent = nullptr);
    /// Ends the track on air (finish()).
    ~HistoryReporter() override;

    const QString& mixSessionId() const {
        return m_options.mixSessionId;
    }

  public slots:
    /// PlayerInfo::currentPlayingTrackChanged.
    void setPlayingTrack(const TrackPointer& pTrack);
    /// PlayerInfo::currentPlayingDeckChanged: the deck's index, from 0, or
    /// -1 when none plays.
    void setPlayingDeck(int deckIndex);
    /// Ends the current track as if another one took the air: at shutdown,
    /// so that its play event is stored for the next run.
    void finish();

  private:
    struct Current {
        TrackPointer pTrack;
        std::optional<cache::TrackKey> crateTrack;
        QDateTime startedAt;
        /// While on air: when it took the air last.
        QDateTime onAirSince;
        /// When it left the air last; null while on air.
        QDateTime leftAirAt;
        qint64 playedMs = 0;
    };

    void begin(const TrackPointer& pTrack, const std::optional<cache::TrackKey>& crateTrack);
    /// Ends the current track, with its play event if it played long enough.
    /// `clearState` clears the now playing state too.
    void end(bool clearState);
    /// The current track leaves the air.
    void pause(bool clearState);
    void resume();
    void sendNowPlaying();
    void clearNowPlaying();
    void scheduleNowPlaying();
    Client* clientOf(const Current& current) const;
    QDateTime now() const;

    Options m_options;
    std::optional<Current> m_current;
    int m_deckIndex = -1;
    /// Tells the current heartbeat timer from abandoned ones.
    quint64 m_heartbeat = 0;
};

} // namespace writeback
} // namespace crate
