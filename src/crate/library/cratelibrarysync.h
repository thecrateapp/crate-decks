#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "crate/client/crateresult.h"
#include "crate/library/cratelibrarystore.h"
#include "util/db/dbconnectionpool.h"

namespace crate {
class Client;
class Reply;
} // namespace crate

namespace crate::library {

/// Options of crate::library::LibrarySync.
struct LibrarySyncOptions {
    /// Tracks per page; the server allows 1 to 500.
    int pageSize = 500;
    /// Attempts of a write while the database is busy.
    int writeAttempts = 4;
    /// Wait before the first retry of a write; it grows with each one.
    std::chrono::milliseconds writeRetryDelay{25};
    /// Called on the worker thread before each answer is parsed. For tests.
    std::function<void()> onParse;
};

/// Mirrors the library of one Crate server into the `crateserver_` tables
/// (plan F3.C-PR1, server contract S3).
///
/// Lives on the main thread. The answers are fetched raw and parsed and
/// stored on a worker thread, one task at a time, retrying while the
/// database is busy (design §9, plan "Antes de F3"). The tracks of a
/// container are replaced in one transaction once its last page arrives, so
/// a sync that fails halfway leaves the old ones whole.
///
/// Every page is asked with the ETag it had in the last sync as
/// If-None-Match: the tree's ETag of a container only changes with its
/// membership, and a page's ETag also with its tracks' metadata. A 304 keeps
/// the stored tracks of that page.
class LibrarySync : public QObject {
    Q_OBJECT
  public:
    using Options = LibrarySyncOptions;

    LibrarySync(const QString& serverId,
            Client* pClient,
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            Options options = Options(),
            QObject* parent = nullptr);
    ~LibrarySync() override;

    const QString& serverId() const {
        return m_serverId;
    }

    /// Fetches the container tree. A call while one is running repeats it
    /// once that one ends.
    void syncContainers();
    bool isSyncingContainers() const {
        return m_treeSyncing;
    }

    /// Fetches the tracks of a container. A call while one is running for the
    /// same container repeats it once that one ends.
    void syncContainerTracks(qint64 containerId);
    bool isSyncing(qint64 containerId) const {
        return m_jobs.find(containerId) != m_jobs.end();
    }

    /// Marks the containers an invalidation event affects as needing a sync.
    /// A Tree or Everything invalidation also fetches the tree again.
    void markDirty(const Invalidation& invalidation);

  signals:
    /// The tree in the tables changed.
    void containersUpdated();
    /// The tracks of a container changed. Not emitted when the server said
    /// nothing changed.
    void containerUpdated(qint64 containerId);
    void containersDirty(const QList<qint64>& containerIds);
    /// `containerId` is 0 for the tree.
    void syncFailed(qint64 containerId, const crate::Error& error);

  private:
    struct Job;

    template<typename Result, typename Task, typename Done>
    void inWorker(Task&& task, Done&& done);

    void finishTree(const std::optional<Error>& error);
    Job* job(qint64 containerId) const;
    void startJob(qint64 containerId);
    void requestPage(qint64 containerId);
    void onPage(qint64 containerId, Reply* pReply, bool conditional, const Error* pError);
    void continueOrFinish(qint64 containerId, const QString& nextCursor);
    void finishJob(qint64 containerId, const std::optional<Error>& error, bool updated);

    const QString m_serverId;
    const QPointer<Client> m_pClient;
    const mixxx::DbConnectionPoolPtr m_pDbConnectionPool;
    const Options m_options;
    /// One worker thread: the writes of this server never contend with each
    /// other, and run in the order they were asked.
    QThreadPool m_pool;

    /// Counts the worker tasks that order syncs and dirty marks.
    quint64 m_tasks = 0;
    bool m_treeSyncing = false;
    bool m_treeAgain = false;
    QPointer<Reply> m_pTreeReply;
    std::map<qint64, std::unique_ptr<Job>> m_jobs;
};

} // namespace crate::library
