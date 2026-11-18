#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <chrono>
#include <functional>
#include <optional>

#include "crate/cache/audiocache.h"
#include "crate/client/crateresult.h"
#include "crate/recs/recsjson.h"

namespace crate {
class Client;
class Reply;
} // namespace crate

namespace crate::recs {

class RecommendationQueue;

/// What a radio starts from: `POST /api/radio/start`'s `seed_type` and
/// `seed_value`.
struct RadioSeed {
    /// `track` (an entity uid), `playlist` (a playlist id) or `crate` (a
    /// crate uuid). The server also takes album, artist and genre seeds,
    /// which Decks does not offer yet.
    QString type;
    QString value;
    /// What the sidebar calls the radio until the server names it.
    QString label;

    static RadioSeed track(const QString& entityUid, const QString& label = QString());
    /// The radio of a container of the Crate section, if its kind can seed
    /// one: a playlist or a crate. Genres and moods cannot: the server takes
    /// a genre slug, which the containers do not carry.
    static std::optional<RadioSeed> container(
            const QString& kind, const QString& remoteId, const QString& label);
};

/// Options of crate::recs::CrateRadioSource.
struct CrateRadioSourceOptions {
    /// Radio tracks kept in Auto DJ's queue ahead of the one playing.
    int ahead = 5;
    /// Wait before asking again when the server is busy or has nothing yet;
    /// it doubles with each retry in a row, up to 32 times.
    std::chrono::milliseconds retryDelay{1000};
    /// The session id of a new radio; a random UUID by default. Tests fix it.
    std::function<QString()> newSessionId;
    /// Runs `callback` after `delay`. Defaults to a single-shot QTimer;
    /// tests replace it to control time.
    std::function<void(std::chrono::milliseconds delay, const std::function<void()>& callback)>
            schedule;
};

/// Crate's Radio as a source of Auto DJ (plan F5.C-PR2, design §7.4): a
/// radio started from a track or a container fills Auto DJ's queue and keeps
/// `ahead` of its tracks queued, asking the server for more as Auto DJ plays
/// them. The DJ's likes and dislikes of its tracks go to the server, which
/// steers the radio with them. Clearing the queue stops the radio.
///
/// The session is the server's shaped radio in its indexed mode: the client
/// picks the session id, and each batch has an index, so that a retried
/// request gets the same batch (`radio/start` with `session_id`, then
/// `radio/next` with `batch_index` and the tracks still queued as
/// `buffer_refs`). One radio at a time; starting another stops the first.
/// Lives on the main thread.
class CrateRadioSource : public QObject {
    Q_OBJECT
  public:
    using Options = CrateRadioSourceOptions;

    enum class Feedback {
        Like,
        Dislike,
    };
    Q_ENUM(Feedback)

    /// Batches in a row without a new track before the radio stops as
    /// exhausted: the server keeps giving what is already queued.
    static constexpr int kMaxEmptyBatches = 5;

    enum class StopReason {
        /// stop(), or another radio started.
        Stopped,
        /// The DJ cleared Auto DJ's queue.
        QueueCleared,
        /// The server has no more tracks for this radio.
        Exhausted,
        /// The server could not start it, or the session ended.
        Failed,
    };
    Q_ENUM(StopReason)

    /// Not owned: `pQueue` must outlive it.
    explicit CrateRadioSource(
            RecommendationQueue* pQueue, Options options = Options(), QObject* parent = nullptr);
    ~CrateRadioSource() override;

    /// Starts a radio from `seed` on the server of `pClient`.
    void start(Client* pClient, const QString& serverId, const RadioSeed& seed);
    /// Stops the radio: no more tracks are queued, and the server ends the
    /// session. The tracks already queued stay.
    void stop();

    bool isRunning() const {
        return !m_sessionId.isEmpty();
    }
    const QString& serverId() const {
        return m_serverId;
    }
    const QString& sessionId() const {
        return m_sessionId;
    }
    /// The server's name of the radio, or the seed's label.
    const QString& label() const {
        return m_label;
    }
    /// Every track this radio has queued, in order.
    const QList<RecommendedTrack>& tracks() const {
        return m_tracks;
    }
    /// Whether `entityUid` is a track of this radio.
    bool hasTrack(const QString& serverId, const QString& entityUid) const;

    /// The DJ likes or dislikes a track of the radio. Returns false for a
    /// track the radio did not queue.
    bool sendFeedback(const QString& entityUid, Feedback feedback);

  signals:
    /// The radio queued more tracks.
    void tracksQueued(const QList<crate::recs::RecommendedTrack>& tracks);
    /// The radio stopped; isRunning() is false.
    void stopped(crate::recs::CrateRadioSource::StopReason reason);
    /// Running, its label or its tracks changed: for the sidebar.
    void changed();

  private:
    void requestBatch(int count, const QStringList& bufferRefs);
    void onBatch(quint64 session, const JsonResult& result, const QByteArray& body);
    void onQueueChanged();
    void readQueue();
    void onQueue(quint64 session, quint64 generation, const QList<cache::TrackKey>& queued);
    void finish(StopReason reason);
    void cancelReply();

    RecommendationQueue* const m_pQueue;
    Options m_options;
    QPointer<Client> m_pClient;
    QString m_serverId;
    QString m_sessionId;
    QString m_label;
    RadioSeed m_seed;
    /// Bumped by every start() and stop(), so that late answers of an
    /// earlier radio are dropped.
    quint64 m_session = 0;
    /// The index of the last batch the server gave.
    std::optional<int> m_batchIndex;
    QPointer<Reply> m_pReply;
    bool m_retryPending = false;
    /// Retries in a row since the last batch with new tracks; each one
    /// doubles the wait, up to 32 times retryDelay.
    int m_retries = 0;
    /// Batches in a row with no new track; kMaxEmptyBatches of them end the
    /// radio as exhausted.
    int m_emptyBatches = 0;
    QList<RecommendedTrack> m_tracks;
    /// By lowercase uid: the server's numeric id, for feedback.
    QHash<QString, qint64> m_trackIds;
    /// Bumped by every enqueue, so that a read of the queue started before
    /// it is not taken as the queue after it.
    quint64 m_queueGeneration = 0;
    bool m_reading = false;
    bool m_readAgain = false;
    /// Radio tracks in the queue at the last read.
    int m_ahead = 0;
};

} // namespace crate::recs
