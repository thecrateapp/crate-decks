#include "crate/library/cratelibrarysync.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QThread>
#include <QtConcurrent>
#include <algorithm>
#include <utility>

#include "crate/client/crateclient.h"
#include "crate/library/cratelibraryjson.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::LibrarySync");

QString now() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

/// Runs a write, again while the database is busy. On the worker thread.
template<typename Write>
WriteStatus withRetries(const LibrarySync::Options& options, Write&& write) {
    WriteStatus status = WriteStatus::Busy;
    for (int attempt = 1; attempt <= std::max(1, options.writeAttempts); ++attempt) {
        status = write();
        if (status != WriteStatus::Busy) {
            return status;
        }
        QThread::msleep(static_cast<unsigned long>(options.writeRetryDelay.count() * attempt));
    }
    return status;
}

std::optional<Error> storageError(WriteStatus status, const char* what) {
    switch (status) {
    case WriteStatus::Ok:
        return std::nullopt;
    case WriteStatus::Busy:
        return Error::storage(QStringLiteral("The database stayed busy: cannot %1")
                        .arg(QLatin1String(what)));
    case WriteStatus::Failed:
        break;
    }
    return Error::storage(QStringLiteral("Cannot %1").arg(QLatin1String(what)));
}

/// What the worker makes of a page of tracks.
struct PageOutcome {
    std::optional<Error> error;
    QStringList uids;
    QString nextCursor;
};

/// What the worker answers to markDirty().
struct DirtyOutcome {
    std::optional<Error> error;
    QList<qint64> marked;
};

} // namespace

struct LibrarySync::Job {
    StoredContainer container;
    /// The tracks and pages of this sync so far.
    QStringList uids;
    QList<PageState> pages;
    /// The cursor of the page being asked; empty for the first.
    QString cursor;
    /// Whether any page came with new content.
    bool changed = false;
    /// Asked again while running: repeat once done.
    bool again = false;
    /// Off after the stored pages turned out inconsistent: no If-None-Match.
    bool conditional = true;
    /// The worker task that loaded the container: a dirty mark asked after
    /// it may have come too late for this sync.
    quint64 loadedAt = 0;
    QPointer<Reply> pReply;
};

LibrarySync::LibrarySync(const QString& serverId,
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

LibrarySync::~LibrarySync() {
    // No answer reaches this object from here on.
    const auto drop = [this](Reply* pReply) {
        if (pReply) {
            disconnect(pReply, nullptr, this, nullptr);
            pReply->cancel();
        }
    };
    drop(m_pTreeReply);
    for (const auto& [id, pJob] : m_jobs) {
        drop(pJob->pReply);
    }
    m_pool.waitForDone();
}

template<typename Result, typename Task, typename Done>
void LibrarySync::inWorker(Task&& task, Done&& done) {
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

void LibrarySync::syncContainers() {
    if (m_treeSyncing) {
        m_treeAgain = true;
        return;
    }
    if (!m_pClient) {
        emit syncFailed(0, Error::network(QStringLiteral("The Crate server is gone")));
        return;
    }
    m_treeSyncing = true;
    RequestOptions options;
    options.rawBody = true;
    Reply* pReply = m_pClient->get(containerTreePath(), options);
    m_pTreeReply = pReply;
    connect(pReply, &Reply::finished, this, [this, pReply](const JsonResult& result) {
        m_pTreeReply = nullptr;
        if (!result.ok()) {
            finishTree(result.error());
            return;
        }
        inWorker<std::optional<Error>>(
                [body = pReply->body(), serverId = m_serverId, options = m_options](
                        const QSqlDatabase& database) -> std::optional<Error> {
                    if (options.onParse) {
                        options.onParse();
                    }
                    const Result<ContainerTree> tree = parseContainerTree(body);
                    if (!tree.ok()) {
                        return tree.error();
                    }
                    return storageError(withRetries(options,
                                                [&] {
                                                    return storeContainerTree(database,
                                                            serverId,
                                                            tree.value());
                                                }),
                            "store the Crate containers");
                },
                [this](const std::optional<Error>& error) {
                    finishTree(error);
                });
    });
}

void LibrarySync::finishTree(const std::optional<Error>& error) {
    m_treeSyncing = false;
    const bool again = std::exchange(m_treeAgain, false);
    const QPointer<LibrarySync> self(this);
    if (error) {
        kLogger.warning() << "Cannot sync the Crate containers:" << error->message;
        emit syncFailed(0, *error);
    } else {
        emit containersUpdated();
    }
    if (self && again) {
        syncContainers();
    }
}

LibrarySync::Job* LibrarySync::job(qint64 containerId) const {
    const auto it = m_jobs.find(containerId);
    return it == m_jobs.end() ? nullptr : it->second.get();
}

void LibrarySync::syncContainerTracks(qint64 containerId) {
    if (Job* pJob = job(containerId)) {
        pJob->again = true;
        return;
    }
    m_jobs.emplace(containerId, std::make_unique<Job>());
    startJob(containerId);
}

void LibrarySync::startJob(qint64 containerId) {
    const quint64 task = ++m_tasks;
    if (Job* pJob = job(containerId)) {
        pJob->loadedAt = task;
    }
    inWorker<std::optional<StoredContainer>>(
            [containerId](const QSqlDatabase& database) {
                return loadContainer(database, containerId);
            },
            [this, containerId](const std::optional<StoredContainer>& container) {
                Job* pJob = job(containerId);
                if (!pJob) {
                    return;
                }
                if (!container || container->serverId != m_serverId) {
                    finishJob(containerId,
                            Error::storage(QStringLiteral("No Crate container %1")
                                            .arg(containerId)),
                            false);
                    return;
                }
                const bool conditional = pJob->conditional;
                const bool again = pJob->again;
                const quint64 loadedAt = pJob->loadedAt;
                *pJob = Job();
                pJob->conditional = conditional;
                pJob->again = again;
                pJob->loadedAt = loadedAt;
                pJob->container = *container;
                // Never synced: whatever arrives is new.
                pJob->changed = container->fetchedAt.isEmpty();
                requestPage(containerId);
            });
}

void LibrarySync::requestPage(qint64 containerId) {
    Job* pJob = job(containerId);
    if (!pJob) {
        return;
    }
    if (!m_pClient) {
        finishJob(containerId, Error::network(QStringLiteral("The Crate server is gone")), false);
        return;
    }
    RequestOptions options;
    options.rawBody = true;
    options.query.addQueryItem(QStringLiteral("limit"), QString::number(m_options.pageSize));
    if (!pJob->cursor.isEmpty()) {
        options.query.addQueryItem(QStringLiteral("cursor"), pJob->cursor);
    }
    // The page the last sync got with this same cursor, if any.
    const qsizetype index = pJob->pages.size();
    const QList<PageState>& stored = pJob->container.pages;
    const bool conditional = pJob->conditional && !pJob->container.fetchedAt.isEmpty() &&
            index < stored.size() && stored[index].cursor == pJob->cursor &&
            !stored[index].etag.isEmpty();
    if (conditional) {
        options.headers.append({QByteArrayLiteral("If-None-Match"), stored[index].etag.toUtf8()});
    }
    Reply* pReply = m_pClient->get(
            containerTracksPath(pJob->container.kind, pJob->container.remoteId), options);
    pJob->pReply = pReply;
    connect(pReply,
            &Reply::finished,
            this,
            [this, containerId, pReply, conditional](const JsonResult& result) {
                onPage(containerId, pReply, conditional, result.ok() ? nullptr : &result.error());
            });
}

void LibrarySync::onPage(
        qint64 containerId, Reply* pReply, bool conditional, const Error* pError) {
    Job* pJob = job(containerId);
    if (!pJob) {
        return;
    }
    pJob->pReply = nullptr;
    if (pError) {
        finishJob(containerId, *pError, false);
        return;
    }
    const qsizetype index = pJob->pages.size();
    if (pReply->httpStatus() == 304) {
        if (!conditional) {
            // Not asked for: start over without If-None-Match.
            pJob->conditional = false;
            startJob(containerId);
            return;
        }
        // The page is as the last sync stored it: same tracks, same cursor.
        const QList<PageState>& stored = pJob->container.pages;
        int first = 0;
        for (qsizetype i = 0; i < index; ++i) {
            first += stored[i].trackCount;
        }
        const PageState page = stored[index];
        const QString nextCursor = index + 1 < stored.size() ? stored[index + 1].cursor : QString();
        inWorker<QStringList>(
                [containerId, first, count = page.trackCount](const QSqlDatabase& database) {
                    return loadContainerTrackUids(database, containerId, first, count);
                },
                [this, containerId, page, nextCursor](const QStringList& uids) {
                    Job* pJob = job(containerId);
                    if (!pJob) {
                        return;
                    }
                    if (uids.size() != page.trackCount) {
                        kLogger.warning() << "The stored tracks of the Crate container"
                                          << containerId << "do not match its pages";
                        pJob->conditional = false;
                        startJob(containerId);
                        return;
                    }
                    pJob->uids += uids;
                    pJob->pages.append(page);
                    continueOrFinish(containerId, nextCursor);
                });
        return;
    }
    const PageState page{pJob->cursor, QString::fromLatin1(pReply->rawHeader("ETag")), 0};
    inWorker<PageOutcome>(
            [body = pReply->body(), serverId = m_serverId, options = m_options](
                    const QSqlDatabase& database) {
                PageOutcome outcome;
                if (options.onParse) {
                    options.onParse();
                }
                const Result<TrackPage> parsed = parseTrackPage(body);
                if (!parsed.ok()) {
                    outcome.error = parsed.error();
                    return outcome;
                }
                outcome.error = storageError(withRetries(options,
                                                     [&] {
                                                         return storeTracks(database,
                                                                 serverId,
                                                                 parsed.value().tracks,
                                                                 now());
                                                     }),
                        "store the Crate tracks");
                outcome.uids.reserve(parsed.value().tracks.size());
                for (const RemoteTrack& track : parsed.value().tracks) {
                    outcome.uids.append(track.entityUid);
                }
                outcome.nextCursor = parsed.value().nextCursor;
                return outcome;
            },
            [this, containerId, page](const PageOutcome& outcome) {
                Job* pJob = job(containerId);
                if (!pJob) {
                    return;
                }
                if (outcome.error) {
                    finishJob(containerId, outcome.error, false);
                    return;
                }
                PageState received = page;
                received.trackCount = static_cast<int>(outcome.uids.size());
                pJob->uids += outcome.uids;
                pJob->pages.append(received);
                pJob->changed = true;
                continueOrFinish(containerId, outcome.nextCursor);
            });
}

void LibrarySync::continueOrFinish(qint64 containerId, const QString& nextCursor) {
    Job* pJob = job(containerId);
    if (!pJob) {
        return;
    }
    if (!nextCursor.isEmpty()) {
        pJob->cursor = nextCursor;
        requestPage(containerId);
        return;
    }
    SyncRecord record{pJob->pages, pJob->container.etag, now()};
    const bool changed = pJob->changed || pJob->pages.size() != pJob->container.pages.size();
    inWorker<std::optional<Error>>(
            [containerId, changed, uids = pJob->uids, record, options = m_options](
                    const QSqlDatabase& database) {
                return storageError(withRetries(options,
                                            [&] {
                                                return changed
                                                        ? replaceContainerTracks(database,
                                                                  containerId,
                                                                  uids,
                                                                  record)
                                                        : markContainerSynced(database,
                                                                  containerId,
                                                                  record);
                                            }),
                        "store the tracks of a Crate container");
            },
            [this, containerId, changed](const std::optional<Error>& error) {
                finishJob(containerId, error, changed);
            });
}

void LibrarySync::finishJob(
        qint64 containerId, const std::optional<Error>& error, bool updated) {
    const auto it = m_jobs.find(containerId);
    if (it == m_jobs.end()) {
        return;
    }
    const bool again = it->second->again;
    m_jobs.erase(it);
    const QPointer<LibrarySync> self(this);
    if (error) {
        kLogger.warning() << "Cannot sync the Crate container" << containerId << ":"
                          << error->message;
        emit syncFailed(containerId, *error);
    } else if (updated) {
        emit containerUpdated(containerId);
    }
    if (self && again) {
        syncContainerTracks(containerId);
    }
}

void LibrarySync::markDirty(const Invalidation& invalidation) {
    const quint64 task = ++m_tasks;
    inWorker<DirtyOutcome>(
            [serverId = m_serverId, invalidation, options = m_options](
                    const QSqlDatabase& database) {
                DirtyOutcome outcome;
                outcome.error = storageError(withRetries(options,
                                                     [&] {
                                                         outcome.marked.clear();
                                                         return library::markDirty(database,
                                                                 serverId,
                                                                 invalidation,
                                                                 &outcome.marked);
                                                     }),
                        "mark Crate containers dirty");
                return outcome;
            },
            [this, task, scope = invalidation.scope](const DirtyOutcome& outcome) {
                if (outcome.error) {
                    kLogger.warning() << outcome.error->message;
                }
                // A sync that loaded its container before the mark may have
                // missed the change; one that loaded it after did not.
                for (const qint64 id : outcome.marked) {
                    Job* pJob = job(id);
                    if (pJob && pJob->loadedAt < task) {
                        pJob->again = true;
                    }
                }
                const QPointer<LibrarySync> self(this);
                if (!outcome.marked.isEmpty()) {
                    emit containersDirty(outcome.marked);
                }
                if (self &&
                        (scope == Invalidation::Scope::Tree ||
                                scope == Invalidation::Scope::Everything)) {
                    syncContainers();
                }
            });
}

} // namespace crate::library

#include "moc_cratelibrarysync.cpp"
