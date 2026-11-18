#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <array>
#include <chrono>
#include <functional>

#include "crate/client/crateresult.h"
#include "util/db/dbconnectionpool.h"

namespace crate {

class Client;
class Reply;

namespace writeback {

/// The kinds of entry the outbox sends (plan F7).
namespace outboxkind {
/// `POST /api/me/play-events` with the payload as its body.
inline const QString kPlayEvent = QStringLiteral("play_event");
/// `POST /api/me/likes` with the payload (`track_entity_uid`).
inline const QString kLike = QStringLiteral("like");
/// `DELETE /api/me/likes` with the payload (`track_entity_uid`).
inline const QString kUnlike = QStringLiteral("unlike");
/// `POST /api/track/rate`. The payload has `entity_uid` and `rating`
/// (0-5); the endpoint takes the numeric `track_id`, which the outbox looks
/// up first with `GET /api/tracks/by-entity/{uid}/info` unless the payload
/// already has it.
inline const QString kRate = QStringLiteral("rate");
/// `PUT /api/decks/cues/{entity_uid}` (plan F8.C-PR1). The payload has
/// `entity_uid`, `edited_at` (when the edit was made, ISO 8601 UTC) and
/// `body`, the request's body, which carries its `base_revision`.
inline const QString kCueSet = QStringLiteral("cue_set");
/// `PUT /api/decks/corrections/{entity_uid}` (plan F8.C-PR2), with the
/// payload of kCueSet.
inline const QString kCorrection = QStringLiteral("correction");
} // namespace outboxkind

/// Delivers Decks' writes to the Crate servers (plan F7.C-PR1): play
/// events, likes and ratings, and since F8 cue sets and corrections.
///
/// An entry is stored in `crateserver_outbox` as soon as it is enqueued, so
/// it survives a restart, and leaves it once the server took it. Each
/// server's entries go out one at a time and in the order they were made. A
/// failure that may pass (no network, no session, 408, 429 or a 5xx) is
/// retried after 1 s, 5 s, 30 s, 5 min and then every 30 min; any other
/// answer of the server (a 4xx) drops the entry with a log line, since
/// sending it again would get the same answer. Order only matters between
/// the writes of one subject (the same track and family: likes, rating, cue
/// set, correction; each play event is its own): an entry the server keeps
/// answering with a retryable status (5xx, 408, 425, 429) is set aside, with
/// the later writes of its subject, behind the others once its backoff
/// reaches 30 min, so one bad write does not hold up the rest. An entry older than seven
/// days is dropped with a log line instead of being sent. flush() sends at
/// once whatever waits, for when the network or the session comes back.
///
/// The cue sets and corrections (F8) are compare-and-set writes: a 409
/// leaves the outbox through conflicted(), with the stored content the
/// server answered, for their sync to resolve.
///
/// The main thread never touches SQLite (design §9): the entries are kept
/// in memory and the table is read and written on a worker thread of its
/// own, in order.
class Outbox : public QObject {
    Q_OBJECT
  public:
    using Schedule = std::function<void(
            std::chrono::milliseconds delay, const std::function<void()>& callback)>;

    struct Options {
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// The Client of a server; nullptr while it has none, and its entries
        /// wait for flush().
        std::function<Client*(const QString& serverId)> clientFor;
        /// Runs `callback` after `delay`. Defaults to a single-shot QTimer;
        /// tests replace it to control time.
        Schedule schedule;
        /// The current time. Defaults to the system's UTC time.
        std::function<QDateTime()> now;
    };

    /// The waits after the first, second, ... failure; the last one repeats.
    static const std::array<std::chrono::milliseconds, 5> kBackoff;
    /// An entry older than this is dropped instead of being sent.
    static constexpr std::chrono::hours kMaxAge{24 * 7};

    explicit Outbox(Options options, QObject* pParent = nullptr);
    /// Waits for the table's writes in flight.
    ~Outbox() override;

    /// Reads the entries a previous run left and starts sending them, before
    /// any enqueued since. Entries enqueued earlier wait for it.
    void start();
    bool isLoaded() const {
        return m_loaded;
    }

    /// Stores an entry for `serverId` and sends it when its turn comes.
    /// Returns its id.
    QString enqueue(const QString& serverId, const QString& kind, const QJsonObject& payload);

    /// The network or a session came back: the servers waiting for a retry
    /// send now, and their backoff starts again.
    void flush();

    /// The entries not delivered yet, of one server or of all.
    int pendingCount(const QString& serverId = QString()) const;
    /// The payloads of a server's entries of `kind` not delivered yet, in
    /// order, including the one in flight.
    QList<QJsonObject> pendingPayloads(const QString& serverId, const QString& kind) const;

    /// Waits until the table has every write made so far. For shutdown and
    /// tests.
    void waitForStorage();

  signals:
    /// start() has read the stored entries.
    void loaded();
    /// The server took an entry; `answer` is the body of its answer.
    void delivered(const QString& serverId,
            const QString& kind,
            const QJsonObject& payload,
            const QJsonDocument& answer);
    /// The server answered 409 to a kCueSet or kCorrection entry, which
    /// leaves the outbox. `answer` is the body of the 409: `current`, the
    /// content and revision the server has, and its `server_time`.
    void conflicted(const QString& serverId,
            const QString& kind,
            const QJsonObject& payload,
            const QJsonObject& answer);
    /// An entry was dropped without being delivered: too old, or refused.
    void dropped(const QString& serverId, const QString& kind, const QString& reason);

  private:
    struct Entry {
        QString id;
        QString serverId;
        QString kind;
        QJsonObject payload;
        QDateTime createdAt;
    };
    struct ServerState {
        QList<Entry> entries;
        bool inFlight = false;
        /// A retry is scheduled.
        bool waiting = false;
        /// Failures in a row of the first entry.
        int failures = 0;
        /// Tells the current scheduled retry from an abandoned one.
        quint64 generation = 0;
        QPointer<Reply> pReply;
    };
    enum class Outcome {
        Delivered,
        /// It may pass: no network, no session.
        Retry,
        /// The server answered, with a status that may pass (5xx, 408, 425,
        /// 429): a retry, or setting the entry aside.
        RetryAnswered,
        Drop,
    };

    void onLoaded(const QList<Entry>& stored);
    void pump(const QString& serverId);
    void send(const QString& serverId, Client* pClient, const Entry& entry);
    void sendRate(const QString& serverId, Client* pClient, const Entry& entry);
    void sendPut(const QString& serverId, Client* pClient, const Entry& entry);
    /// Waits for the request of the first entry. Without `onFinished`, its
    /// answer delivers the entry, or not.
    void watch(const QString& serverId,
            Reply* pReply,
            const std::function<void(const JsonResult&)>& onFinished = nullptr);
    void apply(const QString& serverId,
            Outcome outcome,
            const QString& reason,
            const QJsonDocument& answer = QJsonDocument());
    void retryLater(const QString& serverId);
    /// Moves the first entry and the later ones of its subject behind the
    /// others. False when there are no others.
    bool setAside(const QString& serverId);
    static QString subjectOf(const Entry& entry);
    void remove(const QString& serverId,
            const QString& reason,
            bool delivered,
            const QJsonDocument& answer = QJsonDocument());
    static Outcome outcomeOf(const Error& error);

    void storeInsert(const Entry& entry);
    void storeDelete(const QString& id);

    QDateTime now() const;

    Options m_options;
    QThreadPool m_pool;
    bool m_loaded = false;
    bool m_loading = false;
    QHash<QString, ServerState> m_servers;
};

} // namespace writeback
} // namespace crate
