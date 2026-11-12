#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <chrono>
#include <map>
#include <memory>
#include <optional>

#include "crate/analysis/analysisimporter.h"
#include "crate/analysis/mixprofile.h"
#include "crate/client/crateresult.h"
#include "crate/library/cratelibrarystore.h"
#include "util/db/dbconnectionpool.h"

namespace crate {
class Capabilities;
class Client;
class Reply;
} // namespace crate

namespace crate::analysis {

/// A profile from the store, with the cues Decks set from an earlier one.
struct StoredProfile {
    MixProfile profile;
    std::optional<AppliedCues> appliedCues;
};

/// What ProfileStore::get() found, by lowercase entity uid.
struct ProfileResults {
    QHash<QString, StoredProfile> profiles;
    /// The server has no current profile of these tracks.
    QStringList missing;
    /// These could not be fetched or stored; asking again may work.
    QHash<QString, Error> failed;
};

/// One ProfileStore::get(). It emits finished() once and then deletes itself;
/// deleting it earlier only stops the answer, and the profiles still reach
/// the store.
class ProfileFetch : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;

  signals:
    void finished(const crate::analysis::ProfileResults& results);
};

/// Options of crate::analysis::ProfileStore.
struct ProfileStoreOptions {
    /// Uids per S4 batch request; the server takes at most 200.
    int batchSize = 200;
    /// Single profile requests in flight, without the batch endpoint.
    int maxConcurrentRequests = 4;
    /// Attempts of a write while the database is busy.
    int writeAttempts = 4;
    /// Wait before the first retry of a write; it grows with each one.
    std::chrono::milliseconds writeRetryDelay{25};
};

/// The mix profiles of one Crate server, cached in `crateserver_profiles`
/// (design §6, plan F4.C-PR2).
///
/// A stored profile is current while its revision is the one the library
/// sync recorded for the track (`crateserver_tracks.profile_revision`); any
/// other is fetched again. A track the catalogue lists without a profile is
/// missing without asking the server.
///
/// Fetches use the S4 batch (`POST /api/decks/mix-profiles`, up to 200 uids)
/// when the server announces `profiles_batch`, and otherwise single
/// `GET /api/tracks/by-entity/{uid}/mix-profile?detail=full` requests, a few
/// at a time. Lives on the main thread; answers are parsed and stored on one
/// worker thread, retrying while the database is busy (design §9).
class ProfileStore : public QObject {
    Q_OBJECT
  public:
    using Options = ProfileStoreOptions;

    ProfileStore(const QString& serverId,
            Client* pClient,
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            Options options = Options(),
            QObject* parent = nullptr);
    ~ProfileStore() override;

    /// Whether to use the S4 batch endpoint.
    void setBatchAvailable(bool available);
    void setCapabilities(const Capabilities& capabilities);
    bool batchAvailable() const {
        return m_batchAvailable;
    }

    /// The profiles of `entityUids`, from the store or the server.
    ProfileFetch* get(const QStringList& entityUids);

    /// Records the cues AnalysisImporter set from a profile of the track.
    void recordAppliedCues(const QString& entityUid, const AppliedCues& appliedCues);

  signals:
    /// A recordAppliedCues() that could not be stored.
    void writeFailed(const crate::Error& error);

  private:
    struct Fetch;
    struct Request;

    template<typename Result, typename Task, typename Done>
    void inWorker(Task&& task, Done&& done);

    void enqueue(quint64 fetchId,
            const QStringList& uids,
            const QHash<QString, AppliedCues>& appliedCues);
    void pump();
    void send(const std::shared_ptr<Request>& pRequest);
    void onReply(const std::shared_ptr<Request>& pRequest, Reply* pReply, const JsonResult& result);
    void finishRequest(const std::shared_ptr<Request>& pRequest,
            const std::optional<Error>& error,
            const ProfileResults& results);
    void finishFetchIfDone(quint64 fetchId);

    const QString m_serverId;
    const QPointer<Client> m_pClient;
    const mixxx::DbConnectionPoolPtr m_pDbConnectionPool;
    const Options m_options;
    bool m_batchAvailable = false;
    /// One worker thread, so the writes never contend with each other.
    QThreadPool m_pool;

    quint64 m_nextFetchId = 1;
    std::map<quint64, std::unique_ptr<Fetch>> m_fetches;
    QList<std::shared_ptr<Request>> m_queue;
    QList<std::shared_ptr<Request>> m_inFlight;
};

/// The store's own reads and writes, on the worker thread. Each write is one
/// transaction. Exposed for the tests.
namespace profiles {

using WriteStatus = crate::library::WriteStatus;

/// What the store has for some tracks: the current profiles, the tracks the
/// catalogue lists without one, and the rest, which the server is asked for
/// (with the cues applied from an older revision, if any).
struct Lookup {
    QHash<QString, StoredProfile> current;
    QStringList missing;
    QStringList stale;
    QHash<QString, AppliedCues> staleAppliedCues;
};

Lookup lookup(const QSqlDatabase& database, const QString& serverId, const QStringList& uids);

/// Adds or replaces the profiles, keeping the applied cues of each track.
WriteStatus storeProfiles(const QSqlDatabase& database,
        const QString& serverId,
        const QList<std::pair<MixProfile, QByteArray>>& profilesWithJson,
        const QString& now);

WriteStatus storeAppliedCues(const QSqlDatabase& database,
        const QString& serverId,
        const QString& entityUid,
        const AppliedCues& appliedCues);

QByteArray appliedCuesToJson(const AppliedCues& appliedCues);
std::optional<AppliedCues> appliedCuesFromJson(const QByteArray& json);

} // namespace profiles

} // namespace crate::analysis

Q_DECLARE_METATYPE(crate::analysis::ProfileResults)
