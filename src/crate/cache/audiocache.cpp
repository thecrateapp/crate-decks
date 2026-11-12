#include "crate/cache/audiocache.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>
#include <unordered_map>
#include <utility>

#include "crate/cache/cachepaths.h"
#include "crate/client/crateclient.h"
#include "util/assert.h"
#include "util/logger.h"

namespace crate::cache {

namespace {

const mixxx::Logger kLogger("crate::cache::AudioCache");

const QString kStreamPath = QStringLiteral("/api/tracks/by-entity/%1/stream");

class SystemStorageInfo : public StorageInfo {
  public:
    qint64 bytesAvailable(const QString& directory) const override {
        // The directory may not exist yet: its nearest existing parent is on
        // the same volume.
        QDir existing(directory);
        while (!existing.exists() && !existing.isRoot()) {
            if (!existing.cdUp()) {
                break;
            }
        }
        const QStorageInfo storage(existing.absolutePath());
        if (!storage.isValid() || !storage.isReady()) {
            return -1;
        }
        return storage.bytesAvailable();
    }
};

std::optional<qint64> parseLength(const QByteArray& value) {
    bool ok = false;
    const qint64 length = value.trimmed().toLongLong(&ok);
    if (!ok || length < 0) {
        return std::nullopt;
    }
    return length;
}

struct ContentRange {
    qint64 first = 0;
    /// -1 if the server does not say ("*").
    qint64 total = -1;
};

/// "bytes <first>-<last>/<total>"
std::optional<ContentRange> parseContentRange(const QByteArray& value) {
    const QByteArray trimmed = value.trimmed();
    if (!trimmed.startsWith("bytes ")) {
        return std::nullopt;
    }
    const QByteArray spec = trimmed.mid(6);
    const qsizetype dash = spec.indexOf('-');
    const qsizetype slash = spec.indexOf('/');
    if (dash <= 0 || slash <= dash) {
        return std::nullopt;
    }
    ContentRange range;
    const std::optional<qint64> first = parseLength(spec.left(dash));
    if (!first) {
        return std::nullopt;
    }
    range.first = *first;
    const QByteArray total = spec.mid(slash + 1);
    if (total != "*") {
        const std::optional<qint64> length = parseLength(total);
        if (!length) {
            return std::nullopt;
        }
        range.total = *length;
    }
    return range;
}

/// The last step of a download, on the worker.
struct Finalized {
    std::optional<Error> error;
    qint64 bytes = 0;
};

/// The size of the companion files of the track at `trackPath`.
qint64 companionBytesOf(const std::function<QStringList(const QString&)>& companionsOf,
        const QString& trackPath) {
    qint64 bytes = 0;
    const QStringList companions = companionsOf(trackPath);
    for (const QString& companion : companions) {
        const QFileInfo info(companion);
        if (info.isFile()) {
            bytes += info.size();
        }
    }
    return bytes;
}

} // namespace

/// What only the worker thread touches.
struct AudioCache::Worker {
    explicit Worker(const QString& directory)
            : index(directory) {
    }

    void closePart(quint64 downloadId) {
        const auto it = parts.find(downloadId);
        if (it != parts.end()) {
            it->second->close();
            parts.erase(it);
        }
    }

    AudioCacheIndex index;
    /// The part files being written, by download.
    std::unordered_map<quint64, std::unique_ptr<QFile>> parts;
    /// The first write error of a download.
    std::unordered_map<quint64, QString> writeErrors;
};

struct AudioCache::Download {
    enum class Phase {
        Queued,
        /// The worker looks for a part file to resume.
        Probing,
        Transferring,
        /// The worker checks the length and renames the file.
        Finalizing,
    };

    quint64 id = 0;
    QString serverId;
    QString entityUid;
    Key key;
    Priority priority = Priority::Prefetch;
    TrackHints hints;
    QList<QPointer<CacheTicket>> tickets;
    Phase phase = Phase::Queued;
    QPointer<Reply> pReply;
    int attempts = 0;
    /// Where the current request continues the part file from.
    qint64 offset = 0;
    /// Bytes the current request has received.
    qint64 received = 0;
    /// The size of the whole file; -1 if unknown.
    qint64 expected = -1;
    /// The final file, relative to the cache directory.
    QString relativePath;
    /// The file of an older revision with another extension, deleted once
    /// the new one is complete.
    QString previousPath;
    bool partOpen = false;
    /// Ignore any part file: the server's answer did not match it.
    bool restartFromZero = false;
    bool restartedFromZero = false;
};

CacheTicket::CacheTicket(AudioCache* pCache,
        const QString& serverId,
        const QString& entityUid,
        Priority priority,
        const TrackHints& hints)
        : QObject(pCache),
          m_pCache(pCache),
          m_serverId(serverId),
          m_entityUid(entityUid),
          m_priority(priority),
          m_hints(hints) {
}

CacheTicket::~CacheTicket() {
    if (!m_finished && m_pCache) {
        m_pCache->detach(this);
    }
}

void CacheTicket::cancel() {
    if (m_finished) {
        return;
    }
    m_finished = true;
    if (m_pCache) {
        m_pCache->detach(this);
    }
    deleteLater();
}

void CacheTicket::finishReady(const QString& path) {
    if (m_finished) {
        return;
    }
    m_finished = true;
    // Posted before the signal and nothing after it: a slot may delete this.
    deleteLater();
    emit ready(path);
}

void CacheTicket::finishFailed(const Error& error) {
    if (m_finished) {
        return;
    }
    m_finished = true;
    deleteLater();
    emit failed(error);
}

const QString AudioCache::kSessionPinPrefix = QStringLiteral("session:");

QString AudioCache::defaultDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("crate-audio"));
}

template<typename Result, typename Task, typename Done>
void AudioCache::inWorker(Task&& task, Done&& done) {
    auto* pWatcher = new QFutureWatcher<Result>(this);
    connect(pWatcher,
            &QFutureWatcherBase::finished,
            this,
            [pWatcher, done = std::forward<Done>(done)]() mutable {
                pWatcher->deleteLater();
                done(pWatcher->result());
            });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pWorker = m_pWorker, task = std::forward<Task>(task)]() mutable -> Result {
                return task(*pWorker);
            }));
}

template<typename Task>
void AudioCache::onWorker(Task&& task) {
    m_pool.start([pWorker = m_pWorker, task = std::forward<Task>(task)]() mutable {
        task(*pWorker);
    });
}

AudioCache::AudioCache(Options options, QObject* parent)
        : QObject(parent),
          m_options(std::move(options)) {
    if (m_options.directory.isEmpty()) {
        m_options.directory = defaultDirectory();
    }
    m_options.directory = QDir::cleanPath(QDir(m_options.directory).absolutePath());
    if (!m_options.pStorageInfo) {
        m_options.pStorageInfo = std::make_shared<SystemStorageInfo>();
    }
    m_options.maxDownloadsPerServer = std::max(1, m_options.maxDownloadsPerServer);
    m_options.maxAttempts = std::max(1, m_options.maxAttempts);
    m_options.limitBytes = std::max<qint64>(0, m_options.limitBytes);

    // One thread, in order, that never expires: the index's connection
    // belongs to it.
    m_pool.setMaxThreadCount(1);
    m_pool.setExpiryTimeout(-1);
    m_pWorker = std::make_shared<Worker>(m_options.directory);

    inWorker<Reconciliation>(
            [companionsOf = m_options.companionsOf,
                    directory = m_options.directory](Worker& worker) -> Reconciliation {
                if (!worker.index.open()) {
                    return Reconciliation();
                }
                Reconciliation reconciliation = worker.index.reconcile();
                if (companionsOf) {
                    for (CacheEntry& entry : reconciliation.entries) {
                        if (entry.state == CacheEntry::State::Ready) {
                            entry.companionBytes = companionBytesOf(
                                    companionsOf,
                                    QDir(directory).filePath(entry.path));
                        }
                    }
                }
                return reconciliation;
            },
            [this](const Reconciliation& reconciliation) {
                for (const CacheEntry& entry : reconciliation.entries) {
                    m_entries.insert(keyOf(entry.serverId, entry.entityUid), entry);
                }
                m_loaded = true;
                const QList<std::function<void()>> afterLoad = std::exchange(m_afterLoad, {});
                for (const auto& call : afterLoad) {
                    call();
                }
                makeRoom(0);
                const QList<QPointer<CacheTicket>> waiting = std::exchange(m_waitingForIndex, {});
                for (const QPointer<CacheTicket>& pTicket : waiting) {
                    if (pTicket && !pTicket->m_finished) {
                        resolve(pTicket);
                    }
                }
                emit loaded();
                emit usageChanged();
            });
}

AudioCache::~AudioCache() {
    // No answer reaches this object from here on; part files stay, to
    // resume at the next start.
    for (const DownloadPtr& pDownload : m_downloads) {
        dropReply(pDownload);
        closePart(pDownload);
    }
    m_downloads.clear();
    onWorker([](Worker& worker) {
        worker.parts.clear();
        worker.index.close();
    });
    m_pool.waitForDone();
}

AudioCache::Key AudioCache::keyOf(const QString& serverId, const QString& entityUid) {
    return serverId + QLatin1Char('\n') + entityUid;
}

qint64 AudioCache::now() const {
    return m_options.nowMs ? m_options.nowMs() : QDateTime::currentMSecsSinceEpoch();
}

QString AudioCache::absolutePath(const QString& relativePath) const {
    return QDir(m_options.directory).filePath(relativePath);
}

CacheEntry& AudioCache::entryOf(const QString& serverId, const QString& entityUid) {
    const Key key = keyOf(serverId, entityUid);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        CacheEntry entry;
        entry.serverId = serverId;
        entry.entityUid = entityUid;
        it = m_entries.insert(key, entry);
    }
    return it.value();
}

void AudioCache::store(const CacheEntry& entry) {
    onWorker([entry](Worker& worker) {
        worker.index.upsert(entry);
    });
}

CacheTicket* AudioCache::ensure(const QString& serverId,
        const QString& entityUid,
        Priority priority,
        const TrackHints& hints) {
    auto* pTicket = new CacheTicket(this, serverId, entityUid, priority, hints);
    if (!isSafePathId(serverId) || !isSafePathId(entityUid)) {
        const Error error = Error::decode(QStringLiteral("Not a track id: %1").arg(entityUid));
        QTimer::singleShot(0, pTicket, [pTicket, error] {
            pTicket->finishFailed(error);
        });
        return pTicket;
    }
    if (!m_loaded) {
        m_waitingForIndex.append(pTicket);
        return pTicket;
    }
    resolve(pTicket);
    return pTicket;
}

void AudioCache::resolve(CacheTicket* pTicket) {
    const Key key = keyOf(pTicket->serverId(), pTicket->entityUid());
    for (const DownloadPtr& pDownload : m_downloads) {
        if (pDownload->key == key) {
            pDownload->tickets.append(pTicket);
            pDownload->priority = std::max(pDownload->priority, pTicket->priority());
            pump();
            return;
        }
    }

    const auto it = m_entries.find(key);
    if (it != m_entries.end() && it->state == CacheEntry::State::Ready) {
        const QString& wanted = pTicket->m_hints.sourceRevision;
        const bool current = wanted.isEmpty() || it->sourceRevision.isEmpty() ||
                wanted == it->sourceRevision;
        const QString path = absolutePath(it->path);
        if (current && QFileInfo::exists(path)) {
            it->lastUsedAtMs = now();
            store(*it);
            QTimer::singleShot(0, pTicket, [pTicket, path] {
                pTicket->finishReady(path);
            });
            return;
        }
        if (current) {
            kLogger.info() << "A cached file is gone; downloading it again";
        } else {
            kLogger.info() << "The track changed on the server; downloading it again";
        }
    }

    auto pDownload = std::make_shared<Download>();
    pDownload->id = m_nextDownloadId++;
    pDownload->serverId = pTicket->serverId();
    pDownload->entityUid = pTicket->entityUid();
    pDownload->key = key;
    pDownload->priority = pTicket->priority();
    pDownload->hints = pTicket->m_hints;
    pDownload->tickets.append(pTicket);
    m_downloads.push_back(pDownload);
    pump();
    emit usageChanged();
    emit trackChanged(pDownload->serverId, pDownload->entityUid);
}

void AudioCache::detach(CacheTicket* pTicket) {
    const auto isTicket = [pTicket](const QPointer<CacheTicket>& pOther) {
        return pOther.isNull() || pOther.data() == pTicket;
    };
    m_waitingForIndex.removeIf(isTicket);
    for (const DownloadPtr& pDownload : m_downloads) {
        if (pDownload->tickets.removeIf(isTicket) == 0) {
            continue;
        }
        if (!pDownload->tickets.isEmpty()) {
            Priority priority = Priority::OfflineSet;
            for (const QPointer<CacheTicket>& pOther : std::as_const(pDownload->tickets)) {
                priority = std::max(priority, pOther->priority());
            }
            pDownload->priority = priority;
            return;
        }
        if (pDownload->phase != Download::Phase::Finalizing) {
            // Copied: stop() removes it from m_downloads.
            const DownloadPtr pStopped = pDownload;
            stop(pStopped);
        }
        return;
    }
}

int AudioCache::activeDownloads(const QString& serverId) const {
    return static_cast<int>(std::count_if(m_downloads.cbegin(),
            m_downloads.cend(),
            [&serverId](const DownloadPtr& pDownload) {
                return pDownload->serverId == serverId &&
                        (pDownload->phase == Download::Phase::Probing ||
                                pDownload->phase == Download::Phase::Transferring);
            }));
}

void AudioCache::pump() {
    if (!m_loaded) {
        return;
    }
    std::vector<DownloadPtr> queued;
    for (const DownloadPtr& pDownload : m_downloads) {
        if (pDownload->phase == Download::Phase::Queued) {
            queued.push_back(pDownload);
        }
    }
    // The most urgent first, then in order of arrival.
    std::stable_sort(queued.begin(), queued.end(), [](const DownloadPtr& a, const DownloadPtr& b) {
        return a->priority > b->priority;
    });
    for (const DownloadPtr& pDownload : queued) {
        if (activeDownloads(pDownload->serverId) < m_options.maxDownloadsPerServer) {
            start(pDownload);
        }
    }
}

void AudioCache::start(const DownloadPtr& pDownload) {
    pDownload->phase = Download::Phase::Probing;
    QString partPath;
    bool discard = pDownload->restartFromZero;
    const auto it = m_entries.constFind(pDownload->key);
    if (it != m_entries.cend() && !it->path.isEmpty() && it->state != CacheEntry::State::Ready) {
        partPath = absolutePath(partPathOf(it->path));
        const QString& wanted = pDownload->hints.sourceRevision;
        const bool revisionChanged = !wanted.isEmpty() && !it->sourceRevision.isEmpty() &&
                wanted != it->sourceRevision;
        // Without its ETag a part file cannot be resumed safely.
        discard = discard || it->etag.isEmpty() || revisionChanged;
    }
    const std::weak_ptr<Download> weakDownload = pDownload;
    inWorker<qint64>(
            [partPath, discard](Worker&) -> qint64 {
                if (partPath.isEmpty() || !QFileInfo(partPath).isFile()) {
                    return 0;
                }
                if (discard) {
                    QFile::remove(partPath);
                    return 0;
                }
                return QFileInfo(partPath).size();
            },
            [this, weakDownload](qint64 offset) {
                const DownloadPtr pDownload = weakDownload.lock();
                if (pDownload && pDownload->phase == Download::Phase::Probing) {
                    request(pDownload, offset);
                }
            });
}

void AudioCache::request(const DownloadPtr& pDownload, qint64 offset) {
    Client* const pClient =
            m_options.clientFor ? m_options.clientFor(pDownload->serverId) : nullptr;
    if (!pClient) {
        finish(pDownload, Error::network(QStringLiteral("The Crate server is not available")));
        return;
    }
    RequestOptions options;
    options.streamBody = true;
    const auto it = m_entries.constFind(pDownload->key);
    if (offset > 0 && it != m_entries.cend()) {
        options.headers = {
                {QByteArrayLiteral("Range"),
                        QByteArrayLiteral("bytes=") + QByteArray::number(offset) + '-'},
                {QByteArrayLiteral("If-Range"), it->etag.toUtf8()},
        };
    } else {
        offset = 0;
    }
    pDownload->phase = Download::Phase::Transferring;
    pDownload->offset = offset;
    pDownload->received = 0;
    pDownload->restartFromZero = false;
    ++pDownload->attempts;

    Reply* const pReply = pClient->get(kStreamPath.arg(pDownload->entityUid), options);
    pDownload->pReply = pReply;
    const std::weak_ptr<Download> weakDownload = pDownload;
    connect(pReply, &Reply::headersReceived, this, [this, weakDownload, pReply] {
        if (const DownloadPtr pActive = weakDownload.lock()) {
            onHeaders(pActive, pReply);
        }
    });
    connect(pReply, &Reply::bodyReceived, this, [this, weakDownload](const QByteArray& chunk) {
        if (const DownloadPtr pActive = weakDownload.lock()) {
            onBody(pActive, chunk);
        }
    });
    connect(pReply, &Reply::finished, this, [this, weakDownload](const JsonResult& result) {
        if (const DownloadPtr pActive = weakDownload.lock()) {
            onFinished(pActive, result);
        }
    });
    // A Client that goes away deletes its Replies without an answer.
    connect(pReply, &QObject::destroyed, this, [this, weakDownload] {
        if (const DownloadPtr pActive = weakDownload.lock()) {
            pActive->pReply = nullptr;
            finish(pActive, Error::network(QStringLiteral("The Crate server is gone")));
        }
    });
}

void AudioCache::onHeaders(const DownloadPtr& pDownload, Reply* pReply) {
    const int status = pReply->httpStatus();
    qint64 offset = 0;
    qint64 expected = -1;
    if (status == 206) {
        const std::optional<ContentRange> range =
                parseContentRange(pReply->rawHeader(QByteArrayLiteral("Content-Range")));
        if (!range || range->first != pDownload->offset) {
            kLogger.warning() << "The server resumed a download at another offset";
            dropReply(pDownload);
            closePart(pDownload);
            if (pDownload->restartedFromZero) {
                finish(pDownload, Error::decode(QStringLiteral("The server sent another range")));
                return;
            }
            pDownload->restartedFromZero = true;
            pDownload->restartFromZero = true;
            start(pDownload);
            return;
        }
        offset = range->first;
        expected = range->total;
    } else {
        // The whole file, also when If-Range did not match: it changed.
        if (const std::optional<qint64> length =
                        parseLength(pReply->rawHeader(QByteArrayLiteral("Content-Length")))) {
            expected = *length;
        }
    }
    if (expected < 0 && pDownload->hints.byteLength) {
        expected = *pDownload->hints.byteLength;
    }
    pDownload->offset = offset;
    pDownload->expected = expected;

    CacheEntry& entry = entryOf(pDownload->serverId, pDownload->entityUid);
    QString relativePath = entry.path;
    if (relativePath.isEmpty() || offset == 0) {
        const QString extension = extensionFor(
                pDownload->hints.format, pReply->rawHeader(QByteArrayLiteral("Content-Type")));
        if (extension.isEmpty()) {
            dropReply(pDownload);
            finish(pDownload, Error::decode(QStringLiteral("The track's audio format is unknown")));
            return;
        }
        relativePath = relativeAudioPath(pDownload->serverId, pDownload->entityUid, extension);
    }
    if (!entry.path.isEmpty() && entry.path != relativePath) {
        pDownload->previousPath = entry.path;
    }
    pDownload->relativePath = relativePath;
    // Not Ready from here on, so that it is never evicted to make room for
    // itself.
    entry.state = CacheEntry::State::Downloading;

    if (!makeRoom(expected > 0 ? expected - offset : 0, pDownload.get())) {
        dropReply(pDownload);
        finish(pDownload,
                Error::storage(QStringLiteral(
                        "Not enough free space for the track on the cache's disk")));
        return;
    }
    CacheEntry& stored = entryOf(pDownload->serverId, pDownload->entityUid);
    stored.path = relativePath;
    stored.bytes = std::max<qint64>(0, expected);
    stored.etag = QString::fromLatin1(pReply->rawHeader(QByteArrayLiteral("ETag")));
    if (offset == 0) {
        stored.sourceRevision = pDownload->hints.sourceRevision;
    }
    stored.lastUsedAtMs = now();
    store(stored);

    const quint64 id = pDownload->id;
    const QString partPath = absolutePath(partPathOf(relativePath));
    const bool truncate = offset == 0;
    pDownload->partOpen = true;
    onWorker([id, partPath, truncate](Worker& worker) {
        worker.closePart(id);
        worker.writeErrors.erase(id);
        QDir().mkpath(QFileInfo(partPath).absolutePath());
        auto pFile = std::make_unique<QFile>(partPath);
        const QIODevice::OpenMode mode = QIODevice::WriteOnly |
                (truncate ? QIODevice::Truncate : QIODevice::Append);
        if (!pFile->open(mode)) {
            worker.writeErrors.emplace(id, pFile->errorString());
            return;
        }
        worker.parts.emplace(id, std::move(pFile));
    });
    emit usageChanged();
    emit trackChanged(pDownload->serverId, pDownload->entityUid);
}

void AudioCache::onBody(const DownloadPtr& pDownload, const QByteArray& chunk) {
    if (!pDownload->partOpen) {
        return;
    }
    pDownload->received += chunk.size();
    const quint64 id = pDownload->id;
    onWorker([this, id, chunk](Worker& worker) {
        const auto it = worker.parts.find(id);
        if (it == worker.parts.end()) {
            return;
        }
        QFile* const pFile = it->second.get();
        if (pFile->write(chunk) == chunk.size() && pFile->flush()) {
            return;
        }
        worker.writeErrors.emplace(id, pFile->errorString());
        worker.closePart(id);
        // The destructor waits for this thread, so this object is alive.
        QMetaObject::invokeMethod(
                this, [this, id] { onWriteFailed(id); }, Qt::QueuedConnection);
    });
    const qint64 bytesReceived = pDownload->offset + pDownload->received;
    const QList<QPointer<CacheTicket>> tickets = pDownload->tickets;
    for (const QPointer<CacheTicket>& pTicket : tickets) {
        if (pTicket) {
            emit pTicket->progress(bytesReceived, pDownload->expected);
        }
    }
    emit trackChanged(pDownload->serverId, pDownload->entityUid);
}

void AudioCache::onWriteFailed(quint64 downloadId) {
    for (const DownloadPtr& pDownload : m_downloads) {
        if (pDownload->id == downloadId && pDownload->phase == Download::Phase::Transferring) {
            const DownloadPtr pFailed = pDownload;
            dropReply(pFailed);
            // It reports the write error.
            finalize(pFailed);
            return;
        }
    }
}

void AudioCache::onFinished(const DownloadPtr& pDownload, const JsonResult& result) {
    if (Reply* const pReply = pDownload->pReply) {
        disconnect(pReply, nullptr, this, nullptr);
    }
    pDownload->pReply = nullptr;
    if (result.ok()) {
        finalize(pDownload);
        return;
    }
    closePart(pDownload);
    const Error& error = result.error();
    if (error.kind == Error::Kind::Http && error.httpStatus == 416 &&
            !pDownload->restartedFromZero) {
        // The part file is not a prefix of the server's file.
        pDownload->restartedFromZero = true;
        pDownload->restartFromZero = true;
        start(pDownload);
        return;
    }
    if (error.kind == Error::Kind::Network && pDownload->received > 0 &&
            pDownload->attempts < m_options.maxAttempts) {
        kLogger.info() << "A download broke off; resuming it";
        start(pDownload);
        return;
    }
    finish(pDownload, error);
}

void AudioCache::finalize(const DownloadPtr& pDownload) {
    pDownload->phase = Download::Phase::Finalizing;
    pDownload->partOpen = false;
    if (pDownload->relativePath.isEmpty()) {
        finish(pDownload, Error::decode(QStringLiteral("The server sent no audio")));
        return;
    }
    const quint64 id = pDownload->id;
    const QString path = absolutePath(pDownload->relativePath);
    const QString partPath = partPathOf(path);
    const QString previousPath =
            pDownload->previousPath.isEmpty() ? QString() : absolutePath(pDownload->previousPath);
    const qint64 expected = pDownload->expected;
    const std::weak_ptr<Download> weakDownload = pDownload;
    inWorker<Finalized>(
            [id, path, partPath, previousPath, expected](Worker& worker) -> Finalized {
                worker.closePart(id);
                Finalized finalized;
                const auto writeError = worker.writeErrors.find(id);
                if (writeError != worker.writeErrors.end()) {
                    finalized.error = Error::storage(
                            QStringLiteral("Cannot write the track: %1").arg(writeError->second));
                    worker.writeErrors.erase(writeError);
                    QFile::remove(partPath);
                    return finalized;
                }
                const qint64 size = QFileInfo(partPath).size();
                if (expected >= 0 && size != expected) {
                    QFile::remove(partPath);
                    finalized.error = Error::decode(
                            QStringLiteral("The download has %1 bytes instead of %2")
                                    .arg(size)
                                    .arg(expected));
                    return finalized;
                }
                // An older file of the track (another revision) gives way.
                if (QFileInfo::exists(path)) {
                    QFile::remove(path);
                }
                if (!QFile::rename(partPath, path)) {
                    finalized.error = Error::storage(QStringLiteral("Cannot rename %1").arg(path));
                    return finalized;
                }
                if (!previousPath.isEmpty() && previousPath != path) {
                    QFile::remove(previousPath);
                }
                finalized.bytes = size;
                return finalized;
            },
            [this, weakDownload](const Finalized& finalized) {
                const DownloadPtr pDownload = weakDownload.lock();
                if (!pDownload) {
                    return;
                }
                if (finalized.error) {
                    finish(pDownload, finalized.error);
                    return;
                }
                CacheEntry& entry = entryOf(pDownload->serverId, pDownload->entityUid);
                entry.state = CacheEntry::State::Ready;
                entry.path = pDownload->relativePath;
                entry.bytes = finalized.bytes;
                // A new file of the track: what was made from the old one
                // is measured again
                entry.companionBytes = m_options.companionsOf
                        ? companionBytesOf(m_options.companionsOf, absolutePath(entry.path))
                        : 0;
                entry.lastUsedAtMs = now();
                store(entry);
                finish(pDownload, std::nullopt);
            });
}

void AudioCache::finish(const DownloadPtr& pDownload, const std::optional<Error>& error) {
    dropReply(pDownload);
    closePart(pDownload);
    QString path;
    const auto it = m_entries.find(pDownload->key);
    if (error) {
        kLogger.warning() << "The download of" << pDownload->entityUid
                          << "failed:" << error->message;
        if (it != m_entries.end() && it->state != CacheEntry::State::Ready) {
            it->state = CacheEntry::State::Failed;
            store(*it);
        }
    } else if (it != m_entries.end()) {
        path = absolutePath(it->path);
    }
    const QList<QPointer<CacheTicket>> tickets = pDownload->tickets;
    forget(pDownload);
    for (const QPointer<CacheTicket>& pTicket : tickets) {
        if (!pTicket) {
            continue;
        }
        if (error) {
            pTicket->finishFailed(*error);
        } else {
            pTicket->finishReady(path);
        }
    }
    pump();
    emit usageChanged();
    emit trackChanged(pDownload->serverId, pDownload->entityUid);
}

void AudioCache::stop(const DownloadPtr& pDownload) {
    dropReply(pDownload);
    closePart(pDownload);
    const auto it = m_entries.find(pDownload->key);
    if (it != m_entries.end() && it->state == CacheEntry::State::Downloading) {
        it->state = CacheEntry::State::Pending;
        store(*it);
    }
    const QString serverId = pDownload->serverId;
    const QString entityUid = pDownload->entityUid;
    forget(pDownload);
    pump();
    emit usageChanged();
    emit trackChanged(serverId, entityUid);
}

void AudioCache::dropReply(const DownloadPtr& pDownload) {
    Reply* const pReply = pDownload->pReply;
    pDownload->pReply = nullptr;
    if (pReply) {
        disconnect(pReply, nullptr, this, nullptr);
        pReply->cancel();
    }
}

void AudioCache::closePart(const DownloadPtr& pDownload) {
    if (!pDownload->partOpen) {
        return;
    }
    pDownload->partOpen = false;
    const quint64 id = pDownload->id;
    onWorker([id](Worker& worker) {
        worker.closePart(id);
        worker.writeErrors.erase(id);
    });
}

void AudioCache::forget(const DownloadPtr& pDownload) {
    // Keep it alive while it is removed.
    const DownloadPtr pKept = pDownload;
    m_downloads.erase(std::remove(m_downloads.begin(), m_downloads.end(), pKept),
            m_downloads.end());
}

std::optional<QString> AudioCache::cachedPath(
        const QString& serverId, const QString& entityUid) const {
    const auto it = m_entries.constFind(keyOf(serverId, entityUid));
    if (it == m_entries.cend() || it->state != CacheEntry::State::Ready) {
        return std::nullopt;
    }
    return absolutePath(it->path);
}

std::optional<QString> AudioCache::cachedSourceRevision(
        const QString& serverId, const QString& entityUid) const {
    const auto it = m_entries.constFind(keyOf(serverId, entityUid));
    if (it == m_entries.cend() || it->state != CacheEntry::State::Ready) {
        return std::nullopt;
    }
    return it->sourceRevision;
}

std::optional<QString> AudioCache::filePathFor(
        const QString& serverId, const QString& entityUid, const QString& format) const {
    if (!isSafePathId(serverId) || !isSafePathId(entityUid)) {
        return std::nullopt;
    }
    // A file the cache has, or a part file a download resumes into
    const auto it = m_entries.constFind(keyOf(serverId, entityUid));
    if (it != m_entries.cend() && !it->path.isEmpty()) {
        return absolutePath(it->path);
    }
    const QString extension = extensionFor(format, QByteArray());
    if (extension.isEmpty()) {
        return std::nullopt;
    }
    return absolutePath(relativeAudioPath(serverId, entityUid, extension));
}

TrackState AudioCache::trackState(const QString& serverId, const QString& entityUid) const {
    TrackState state;
    const Key key = keyOf(serverId, entityUid);
    for (const DownloadPtr& pDownload : m_downloads) {
        if (pDownload->key == key) {
            state.kind = pDownload->phase == Download::Phase::Queued
                    ? TrackState::Kind::Queued
                    : TrackState::Kind::Downloading;
            state.bytesReceived = pDownload->offset + pDownload->received;
            state.bytesTotal = pDownload->expected;
            return state;
        }
    }
    const auto it = m_entries.constFind(key);
    if (it == m_entries.cend()) {
        return state;
    }
    switch (it->state) {
    case CacheEntry::State::Ready:
        state.kind = TrackState::Kind::Ready;
        state.bytesReceived = it->bytes;
        state.bytesTotal = it->bytes;
        break;
    case CacheEntry::State::Failed:
        state.kind = TrackState::Kind::Failed;
        break;
    case CacheEntry::State::Pending:
    case CacheEntry::State::Downloading:
        break;
    }
    return state;
}

std::optional<TrackKey> AudioCache::trackOfFile(const QString& path) const {
    const QString relativePath = QDir::isAbsolutePath(path)
            ? QDir(m_options.directory).relativeFilePath(path)
            : path;
    if (relativePath.startsWith(QStringLiteral(".."))) {
        return std::nullopt;
    }
    const auto track = trackOfRelativePath(QDir::fromNativeSeparators(relativePath));
    if (!track) {
        return std::nullopt;
    }
    return TrackKey{track->first, track->second};
}

bool AudioCache::remove(const QString& serverId, const QString& entityUid) {
    const Key key = keyOf(serverId, entityUid);
    for (const DownloadPtr& pDownload : m_downloads) {
        if (pDownload->key == key) {
            return false;
        }
    }
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        return false;
    }
    const bool hadFile = it->state == CacheEntry::State::Ready;
    evict(key);
    emit usageChanged();
    return hadFile;
}

void AudioCache::pin(const QString& serverId, const QString& entityUid, const QString& reason) {
    if (!isSafePathId(serverId) || !isSafePathId(entityUid) || reason.isEmpty()) {
        return;
    }
    if (reason.startsWith(kSessionPinPrefix)) {
        // Never in the index: nothing of it is left after a restart
        QStringList& reasons = m_sessionPins[keyOf(serverId, entityUid)];
        if (!reasons.contains(reason)) {
            reasons.append(reason);
            emit usageChanged();
        }
        return;
    }
    if (!m_loaded) {
        m_afterLoad.append([this, serverId, entityUid, reason] {
            pin(serverId, entityUid, reason);
        });
        return;
    }
    CacheEntry& entry = entryOf(serverId, entityUid);
    if (entry.pinnedReasons.contains(reason)) {
        return;
    }
    entry.pinnedReasons.append(reason);
    store(entry);
    emit usageChanged();
}

int AudioCache::unpinAll(const QString& serverId, const QString& reason) {
    if (!m_loaded) {
        m_afterLoad.append([this, serverId, reason] {
            unpinAll(serverId, reason);
        });
        return 0;
    }
    int unpinned = 0;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->serverId == serverId && it->pinnedReasons.removeAll(reason) > 0) {
            store(*it);
            ++unpinned;
        }
    }
    if (unpinned > 0) {
        makeRoom(0);
        emit usageChanged();
    }
    return unpinned;
}

void AudioCache::unpin(const QString& serverId, const QString& entityUid, const QString& reason) {
    if (reason.startsWith(kSessionPinPrefix)) {
        const auto session = m_sessionPins.find(keyOf(serverId, entityUid));
        if (session == m_sessionPins.end() || session->removeAll(reason) == 0) {
            return;
        }
        if (session->isEmpty()) {
            m_sessionPins.erase(session);
        }
        if (m_loaded) {
            // What the limit left over for it goes now
            makeRoom(0);
        }
        emit usageChanged();
        return;
    }
    if (!m_loaded) {
        m_afterLoad.append([this, serverId, entityUid, reason] {
            unpin(serverId, entityUid, reason);
        });
        return;
    }
    const auto it = m_entries.find(keyOf(serverId, entityUid));
    if (it == m_entries.end() || it->pinnedReasons.removeAll(reason) == 0) {
        return;
    }
    store(*it);
    makeRoom(0);
    emit usageChanged();
}

void AudioCache::setLimitBytes(qint64 limitBytes) {
    m_options.limitBytes = std::max<qint64>(0, limitBytes);
    if (m_loaded) {
        makeRoom(0);
        emit usageChanged();
    }
}

void AudioCache::refreshCompanions(const QString& serverId, const QString& entityUid) {
    const auto it = m_entries.find(keyOf(serverId, entityUid));
    if (it == m_entries.end() || it->state != CacheEntry::State::Ready ||
            !m_options.companionsOf) {
        return;
    }
    const qint64 bytes = companionBytesOf(m_options.companionsOf, absolutePath(it->path));
    if (bytes == it->companionBytes) {
        return;
    }
    it->companionBytes = bytes;
    makeRoom(0);
    emit usageChanged();
    emit trackChanged(serverId, entityUid);
}

Usage AudioCache::usage() const {
    Usage usage;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        const CacheEntry& entry = it.value();
        if (entry.state != CacheEntry::State::Ready) {
            continue;
        }
        ++usage.readyFiles;
        usage.readyBytes += entry.totalBytes();
        if (entry.isPinned()) {
            usage.pinnedBytes += entry.totalBytes();
        } else if (m_sessionPins.contains(it.key())) {
            usage.inUseBytes += entry.totalBytes();
        }
    }
    for (const DownloadPtr& pDownload : m_downloads) {
        if (pDownload->phase == Download::Phase::Queued) {
            ++usage.queuedDownloads;
        } else {
            ++usage.activeDownloads;
        }
    }
    return usage;
}

QList<DownloadInfo> AudioCache::downloads() const {
    QList<DownloadInfo> downloads;
    for (const DownloadPtr& pDownload : m_downloads) {
        DownloadInfo download;
        download.serverId = pDownload->serverId;
        download.entityUid = pDownload->entityUid;
        download.priority = pDownload->priority;
        download.active = pDownload->phase != Download::Phase::Queued;
        download.bytesReceived = pDownload->offset + pDownload->received;
        download.bytesTotal = pDownload->expected;
        downloads.append(download);
    }
    // On their way first, then as pump() takes them
    std::stable_sort(downloads.begin(),
            downloads.end(),
            [](const DownloadInfo& a, const DownloadInfo& b) {
                if (a.active != b.active) {
                    return a.active;
                }
                return a.priority > b.priority;
            });
    return downloads;
}

QList<PinnedSet> AudioCache::pinnedSets(const QString& reasonPrefix) const {
    QList<PinnedSet> sets;
    QHash<QString, qsizetype> indexOf;
    for (const CacheEntry& entry : m_entries) {
        for (const QString& reason : entry.pinnedReasons) {
            if (!reason.startsWith(reasonPrefix)) {
                continue;
            }
            const QString key = entry.serverId + QLatin1Char('\n') + reason;
            const auto found = indexOf.constFind(key);
            qsizetype index = 0;
            if (found == indexOf.cend()) {
                index = sets.size();
                indexOf.insert(key, index);
                sets.append(PinnedSet{entry.serverId, reason});
            } else {
                index = found.value();
            }
            PinnedSet& set = sets[index];
            ++set.tracks;
            if (entry.state == CacheEntry::State::Ready) {
                ++set.readyTracks;
                set.readyBytes += entry.totalBytes();
            }
        }
    }
    std::sort(sets.begin(), sets.end(), [](const PinnedSet& a, const PinnedSet& b) {
        return a.serverId != b.serverId ? a.serverId < b.serverId : a.reason < b.reason;
    });
    return sets;
}

int AudioCache::clear() {
    QSet<Key> downloading;
    for (const DownloadPtr& pDownload : m_downloads) {
        downloading.insert(pDownload->key);
    }
    QList<Key> keys;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->state == CacheEntry::State::Ready && !isKept(it.key(), it.value()) &&
                !downloading.contains(it.key())) {
            keys.append(it.key());
        }
    }
    for (const Key& key : std::as_const(keys)) {
        evict(key);
    }
    if (!keys.isEmpty()) {
        emit usageChanged();
    }
    return static_cast<int>(keys.size());
}

qint64 AudioCache::reservedBytes(const Download* pExcept) const {
    qint64 reserved = 0;
    for (const DownloadPtr& pDownload : m_downloads) {
        if (pDownload.get() == pExcept || !pDownload->partOpen || pDownload->expected <= 0) {
            continue;
        }
        // What is on the disk, rather than what came in: the worker may
        // not have written all of it yet
        const qint64 written =
                QFileInfo(absolutePath(partPathOf(pDownload->relativePath))).size();
        reserved += std::max<qint64>(0, pDownload->expected - written);
    }
    return reserved;
}

bool AudioCache::makeRoom(qint64 incomingBytes, const Download* pFor) {
    struct Candidate {
        Key key;
        qint64 lastUsedAtMs;
        qint64 bytes;
    };
    qint64 readyBytes = 0;
    std::vector<Candidate> candidates;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->state != CacheEntry::State::Ready) {
            continue;
        }
        readyBytes += it->totalBytes();
        if (!isKept(it.key(), it.value())) {
            candidates.push_back({it.key(), it->lastUsedAtMs, it->totalBytes()});
        }
    }
    // The free space only bounds what comes in: a disk that others fill
    // does not empty the cache.
    const qint64 freeBytes = incomingBytes > 0
            ? m_options.pStorageInfo->bytesAvailable(m_options.directory)
            : -1;
    // The other downloads under way will write the rest of their files too
    const qint64 reserved = reservedBytes(pFor);
    qint64 freedBytes = 0;
    const auto overLimit = [&] {
        return readyBytes - freedBytes + incomingBytes + reserved > m_options.limitBytes;
    };
    const auto shortOfSpace = [&] {
        return freeBytes >= 0 &&
                freeBytes + freedBytes - incomingBytes - reserved < m_options.minFreeBytes;
    };
    if (!overLimit() && !shortOfSpace()) {
        return true;
    }
    std::stable_sort(candidates.begin(),
            candidates.end(),
            [](const Candidate& a, const Candidate& b) {
                return a.lastUsedAtMs < b.lastUsedAtMs;
            });
    for (const Candidate& candidate : candidates) {
        if (!overLimit() && !shortOfSpace()) {
            break;
        }
        evict(candidate.key);
        freedBytes += candidate.bytes;
    }
    if (overLimit()) {
        kLogger.info() << "The cache is over its limit: the rest of it is pinned";
    }
    return !shortOfSpace();
}

bool AudioCache::isKept(const Key& key, const CacheEntry& entry) const {
    return entry.isPinned() || m_sessionPins.contains(key);
}

void AudioCache::evict(const Key& key) {
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        return;
    }
    const CacheEntry entry = it.value();
    m_entries.erase(it);
    const QString path = absolutePath(entry.path);
    const QStringList companions =
            m_options.companionsOf && !entry.path.isEmpty() ? m_options.companionsOf(path)
                                                            : QStringList();
    onWorker([entry, path, companions](Worker& worker) {
        QFile::remove(path);
        for (const QString& companion : companions) {
            QFile::remove(companion);
        }
        worker.index.remove(entry.serverId, entry.entityUid);
    });
    emit evicted(entry.serverId, entry.entityUid);
    emit trackChanged(entry.serverId, entry.entityUid);
}

} // namespace crate::cache

#include "moc_audiocache.cpp"
