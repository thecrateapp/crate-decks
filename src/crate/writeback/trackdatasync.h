#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <array>
#include <chrono>
#include <functional>

#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

namespace crate {

class Client;
struct Error;

namespace writeback {

class Outbox;

/// What the syncs of the user's own data of Crate tracks share (plan F8):
/// CrateCueSync for cues, CrateCorrections for BPM, grid and key.
///
/// The server keeps one revision per user, track and kind of data (S7), and
/// takes a whole new content with the revision it replaces
/// (`base_revision`), answering 409 with what it has when that revision is
/// not the current one. For each track this class keeps, in
/// `crateserver_user_sync`, the last revision Decks read or wrote and the
/// content it counts as in sync with it ("baseline"), across runs.
///
/// - attach(): a Crate track taken for a deck. Its data is read from the
///   server with the other tracks taken at about the same time, up to 200
///   in one query (kBatchWindow), and handed to reconcile() once the stored
///   states are read.
/// - Changes of the track (cues, beats, key) go to trackChanged(), except
///   those of Crate's analysis and of upstream's: when the analysis
///   finishes (Track::analyzed), when a cue or beats import that waited for
///   the file completes, and when absorbAutomaticChanges() says so, it is
///   absorb() instead.
/// - enqueuePut() sends a new content through the Outbox (kept across
///   restarts, retried with backoff). A retry repeats the same body, which
///   the server answers 200 even if the first attempt was stored. A 409
///   resolves as plan F8.C-PR1 says: the server's content wins if it was
///   written after the edit here and by another installation, and is
///   applied to the track with localEditsReplaced(); otherwise the content
///   is sent again on top of the server's revision. "After" is on the
///   server's clock: every answer carries `server_time`, and edits are
///   stamped with the offset of the latest one (editStamp()).
/// - Only for servers that announce the `cues` feature (enabledFor).
///
/// On the main thread; the table is read and written on a thread of its own.
class TrackDataSync : public QObject {
    Q_OBJECT
  public:
    using Schedule = std::function<void(
            std::chrono::milliseconds delay, const std::function<void()>& callback)>;

    struct Options {
        /// Not owned; required.
        Outbox* pOutbox = nullptr;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// The Client of a signed-in server; nullptr otherwise.
        std::function<Client*(const QString& serverId)> clientFor;
        /// Whether the server announces the `cues` feature, which serves cues
        /// and corrections. Without it every server is taken.
        std::function<bool(const QString& serverId)> enabledFor;
        /// `client_id` of the writes: the installation's
        /// X-Device-Fingerprint, which tells its own writes from another
        /// machine's.
        QString clientId;
        /// Runs `callback` after `delay`. Defaults to a single-shot QTimer;
        /// tests replace it to control time.
        Schedule schedule;
        /// The current time. Defaults to the system's UTC time.
        std::function<QDateTime()> now;
    };

    /// The most entity UIDs of one query.
    static constexpr int kBatchLimit = 200;
    /// How long attach() waits for more tracks before it queries.
    static constexpr std::chrono::milliseconds kBatchWindow{100};
    /// How long a track has to stay unchanged before an edit counts.
    static constexpr std::chrono::milliseconds kDebounce{2000};
    /// The waits before asking again after a query failed and may pass (no
    /// network, 408, 425, 429, 5xx); the last one repeats while the tracks
    /// stay attached.
    static constexpr std::array<std::chrono::milliseconds, 3> kQueryRetry{
            std::chrono::seconds(5), std::chrono::seconds(30), std::chrono::minutes(5)};

    ~TrackDataSync() override;

    /// Reads the stored states; tracks attached before it ends wait for it.
    void start();
    bool isLoaded() const {
        return m_loaded;
    }
    /// Waits for the table's reads and writes in flight. For tests.
    void waitForStorage();

    /// A Crate track taken for a deck: reads the user's data of it and
    /// applies it. Nothing happens for a server without the feature.
    void attach(const TrackPointer& pTrack, const QString& serverId, const QString& entityUid);
    /// Crate's analysis was just applied to the track: what it changed, and
    /// what an import it left pending changes when the file is opened, is
    /// not the user's edit.
    void absorbAutomaticChanges(const TrackPointer& pTrack);

    /// The last revision Decks read or wrote of the track; 0 if none.
    qint64 revision(const QString& serverId, const QString& entityUid) const;

  signals:
    void loaded();
    /// The server's content replaced the track's.
    void serverDataApplied(const QString& serverId, const QString& entityUid);
    /// A conflict went the server's way: the edits made here to the track
    /// were replaced by the ones made on another computer since.
    void localEditsReplaced(
            const QString& serverId, const QString& entityUid, const QString& trackTitle);

  protected:
    /// The table's `kind` and the routes of the data.
    struct Endpoint {
        QString kind;
        QString queryPath;
        /// The object of the query's answer, keyed by entity UID.
        QString answerKey;
        QString outboxKind;
    };
    struct State {
        qint64 revision = 0;
        QJsonObject baseline;
        /// The table has it (or will).
        bool known = false;
    };
    enum class Change {
        Cues,
        Beats,
        Key,
    };

    TrackDataSync(Endpoint endpoint, Options options, QObject* pParent);

    /// The server's entry of an attached track came: `serverEntry` is empty
    /// for a track in `missing` (revision 0). Returns false when the track
    /// cannot be synced yet: its changes are not edits until the next load.
    virtual bool reconcile(const QString& key,
            const TrackPointer& pTrack,
            qint64 serverRevision,
            const QJsonObject& serverEntry) = 0;
    /// The content of a server entry or answer, as the baseline keeps it.
    virtual QJsonObject contentOf(const QJsonObject& serverEntry) const = 0;
    /// Puts the server's content on the track; returns the baseline after.
    virtual QJsonObject applyServerContent(
            const QString& key, const TrackPointer& pTrack, const QJsonObject& content) = 0;
    /// What to send again when a 409 goes this installation's way.
    virtual QJsonObject contentToResend(const QString& key, const QJsonObject& sent) = 0;
    /// The track changed, not by Crate's or upstream's analysis.
    virtual void trackChanged(const QString& key, const TrackPointer& pTrack, Change change) = 0;
    /// Whatever the track has now came from the analysis.
    virtual void absorb(const QString& key, const TrackPointer& pTrack) = 0;
    /// The server stored the content sent last of `key` (its new revision is
    /// the state's). Nothing by default.
    virtual void delivered(const QString& key) {
        Q_UNUSED(key);
    }

    static QString keyOf(const QString& serverId, const QString& entityUid);
    /// The attached track of `key`, if it is still in memory.
    TrackPointer trackOf(const QString& key) const;
    QString serverIdOf(const QString& key) const;
    QString entityUidOf(const QString& key) const;
    bool isReconciled(const QString& key) const;

    State& state(const QString& key) {
        return m_states[key];
    }
    const State stateOf(const QString& key) const {
        return m_states.value(key);
    }
    /// Stores the state of `key`.
    void storeState(const QString& key);
    /// Sends `content` as the new content of the track on top of the
    /// state's revision, and makes it the baseline.
    void enqueuePut(const QString& key, const QJsonObject& content, const QDateTime& editedAt);
    /// Whether the outbox still has a write of this track.
    bool uploadPending(const QString& key) const;
    /// Runs `callback` once the track has been left alone for kDebounce;
    /// a newer call replaces it.
    void debounce(const QString& key, std::function<void()> callback);
    /// Runs the debounced callbacks now.
    void runDebounced();
    /// Applies the server's content with the track's signals ignored, makes
    /// it the baseline with `revision` and stores it.
    void applyFromServer(const QString& key,
            const TrackPointer& pTrack,
            const QJsonObject& content,
            qint64 revision);
    QDateTime now() const;
    /// When an edit made now happened, on the server's clock: the local
    /// time plus the offset of the server's latest `server_time` (Diego,
    /// 2026-11-16). On the local clock while no answer has told it.
    QDateTime editStamp(const QString& key) const;
    /// Makes a change to the track that is not the user's edit: its signals
    /// are ignored and a debounce waiting is void.
    void changeQuietly(const QString& key, const std::function<void()>& change);

  private:
    struct Attached {
        TrackWeakPointer pTrack;
        QString serverId;
        QString entityUid;
        bool reconciled = false;
        bool cueImportPending = false;
        bool beatsImportPending = false;
        quint64 generation = 0;
    };
    struct Due {
        quint64 generation = 0;
        std::function<void()> callback;
    };

    void onLoaded(const QHash<QString, State>& stored);
    void queueQuery(const QString& key);
    void query(const QString& serverId);
    /// Queries the batch's tracks again later, if the failure may pass.
    void retryQuery(const QString& serverId, const QStringList& batch, const Error& error);
    void onAnswer(const QStringList& keys, const QJsonObject& answer);
    void onSignal(const QString& key, Change change);
    /// Updates the import flags; returns whether an import just completed.
    bool importCompleted(Attached& attached, const TrackPointer& pTrack);
    /// Keeps the offset of the server's clock from an answer's `server_time`.
    void observeServerTime(const QString& serverId, const QJsonObject& answer);
    void onDelivered(const QString& serverId,
            const QString& kind,
            const QJsonObject& payload,
            const QJsonDocument& answer);
    void onConflicted(const QString& serverId,
            const QString& kind,
            const QJsonObject& payload,
            const QJsonObject& answer);

    const Endpoint m_endpoint;
    Options m_options;
    QThreadPool m_pool;
    bool m_loaded = false;
    bool m_loading = false;
    QHash<QString, State> m_states;
    QHash<QString, Attached> m_attached;
    /// Attached before the states were read.
    QStringList m_waiting;
    /// Keys to query, by server, and whether its query is scheduled.
    QHash<QString, QStringList> m_queued;
    QHash<QString, bool> m_queryScheduled;
    /// Failed queries in a row, by server.
    QHash<QString, int> m_queryFailures;
    QHash<QString, Due> m_due;
    /// The server's clock minus this computer's, in ms, by server.
    QHash<QString, qint64> m_clockOffsets;
    bool m_applying = false;
};

} // namespace writeback
} // namespace crate
