#include "crate/stems/stemjobs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QThread>
#include <QUrlQuery>
#include <QtConcurrentRun>

#include "control/controlobject.h"
#include "crate/client/crateclient.h"
#include "crate/stems/stemanalyzer.h"
#include "crate/stems/stemsidecar.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::StemJobs");

constexpr int kDbAttempts = 4;
constexpr unsigned long kDbRetryMillis = 25;

/// Whether any deck plays, from the decks' `play` controls.
bool decksPlay() {
    const ControlObject* pDecks = ControlObject::getControl(
            ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_decks")),
            ControlFlag::NoAssertIfMissing);
    const int decks = pDecks ? static_cast<int>(pDecks->get()) : 4;
    for (int deck = 1; deck <= decks; ++deck) {
        const ControlObject* pPlay = ControlObject::getControl(
                ConfigKey(QStringLiteral("[Channel%1]").arg(deck), QStringLiteral("play")),
                ControlFlag::NoAssertIfMissing);
        if (pPlay && pPlay->get() > 0.0) {
            return true;
        }
    }
    return false;
}

/// The stems of a separation to the analyzer and the sidecar at once.
class TeeSink : public StemSink {
  public:
    TeeSink(StemSidecarWriter::Options options)
            : writer(std::move(options)) {
    }

    bool begin(const StemSeparationSource& source, QString* pError) override {
        return analyzer.begin(source, pError) && writer.begin(source, pError);
    }
    bool write(SINT firstFrame,
            SINT frameCount,
            std::span<const float> stems,
            QString* pError) override {
        return analyzer.write(firstFrame, frameCount, stems, pError) &&
                writer.write(firstFrame, frameCount, stems, pError);
    }

    StemAnalyzer analyzer;
    StemSidecarWriter writer;
};

} // namespace

QString StemJobs::pinReasonOf(qint64 playlistId) {
    return QStringLiteral("offline-set:playlist:%1").arg(playlistId);
}

QString StemJobs::keyOf(const QString& serverId, const QString& entityUid) {
    return serverId + QLatin1Char('\n') + entityUid.toLower();
}

StemJobs::StemJobs(Options options, QObject* parent)
        : QObject(parent),
          m_options(std::move(options)) {
    m_pool.setMaxThreadCount(1);
    m_pollTimer.setInterval(m_options.pollInterval);
    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        const QStringList servers = m_servers.keys();
        for (const QString& serverId : servers) {
            syncPlaylists(serverId);
        }
    });
    m_pauseTimer.setInterval(m_options.pauseCheckInterval);
    connect(&m_pauseTimer, &QTimer::timeout, this, &StemJobs::checkPause);
}

StemJobs::~StemJobs() {
    if (m_separating && m_options.pSeparator) {
        m_options.pSeparator->cancel();
    }
    m_pool.waitForDone();
}

void StemJobs::startServer(const QString& serverId) {
    if (m_servers.contains(serverId)) {
        return;
    }
    m_servers.insert(serverId, Server());
    loadStems(serverId);
    if (!m_pollTimer.isActive()) {
        m_pollTimer.start();
    }
}

void StemJobs::stopServer(const QString& serverId) {
    m_servers.remove(serverId);
    std::erase_if(m_queue, [&serverId](const Job& job) {
        return job.track.serverId == serverId;
    });
    if (m_servers.isEmpty()) {
        m_pollTimer.stop();
    }
}

void StemJobs::loadStems(const QString& serverId) {
    auto* pWatcher = new QFutureWatcher<QHash<QString, QString>>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher, serverId] {
        pWatcher->deleteLater();
        const auto it = m_servers.find(serverId);
        if (it == m_servers.end()) {
            return;
        }
        it->stems = pWatcher->result();
        it->loaded = true;
        for (auto stem = it->stems.cbegin(); stem != it->stems.cend(); ++stem) {
            m_states.insert(keyOf(serverId, stem.key()), TrackState::Ready);
        }
        syncPlaylists(serverId);
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_options.pDbConnectionPool, serverId]() {
                QHash<QString, QString> stems;
                if (!pPool) {
                    return stems;
                }
                const mixxx::DbConnectionPooler pooler(pPool);
                const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
                for (int attempt = 0; attempt < kDbAttempts; ++attempt) {
                    if (attempt > 0) {
                        QThread::msleep(kDbRetryMillis * static_cast<unsigned long>(attempt));
                    }
                    QSqlQuery query(database);
                    query.prepare(QStringLiteral(
                            "SELECT entity_uid, source_revision FROM crateserver_stems "
                            "WHERE server_id = :server"));
                    query.bindValue(QStringLiteral(":server"), serverId);
                    if (!query.exec()) {
                        continue;
                    }
                    while (query.next()) {
                        stems.insert(query.value(0).toString().toLower(),
                                query.value(1).toString());
                    }
                    return stems;
                }
                kLogger.warning() << "Cannot read the stems of" << serverId;
                return stems;
            }));
}

void StemJobs::syncPlaylists(const QString& serverId) {
    const auto it = m_servers.find(serverId);
    Client* pClient = m_options.clientFor ? m_options.clientFor(serverId) : nullptr;
    if (it == m_servers.end() || !it->loaded || it->polling || !pClient) {
        return;
    }
    it->polling = true;
    RequestOptions options;
    if (it->revision >= 0) {
        options.query.addQueryItem(
                QStringLiteral("sinceRevision"), QString::number(it->revision));
    }
    Reply* pReply = pClient->get(QStringLiteral("/api/smart-mix/playlists"), options);
    connect(pReply, &Reply::finished, this, [this, serverId](const JsonResult& result) {
        const auto server = m_servers.find(serverId);
        if (server == m_servers.end()) {
            return;
        }
        server->polling = false;
        if (!result.ok()) {
            kLogger.info() << "Cannot read the Smart Mix playlists of" << serverId << ":"
                           << result.error().message;
            return;
        }
        onPlaylists(serverId, result.value());
    });
}

void StemJobs::onPlaylists(const QString& serverId, const QJsonDocument& document) {
    const auto server = m_servers.find(serverId);
    if (server == m_servers.end()) {
        return;
    }
    const QJsonObject object = document.object();
    const bool firstRead = server->revision < 0;
    server->revision = std::max(
            server->revision, object.value(QStringLiteral("revision")).toInteger(0));
    const QJsonArray playlists = object.value(QStringLiteral("playlists")).toArray();
    for (const QJsonValue& value : playlists) {
        const QJsonObject playlist = value.toObject();
        const qint64 playlistId = playlist.value(QStringLiteral("playlistId")).toInteger(-1);
        if (playlistId < 0) {
            continue;
        }
        const bool enabled = playlist.value(QStringLiteral("enabled")).toBool();
        const auto known = server->playlists.constFind(playlistId);
        const bool wasEnabled = known != server->playlists.cend() && known->enabled;
        if (enabled) {
            // A change while on: tracks added or removed
            enablePlaylist(serverId, playlistId);
        } else if (wasEnabled || firstRead) {
            // Also one turned off before this run, whose pins may remain
            disablePlaylist(serverId, playlistId);
        }
    }
    emit playlistsChanged(serverId);
}

void StemJobs::enablePlaylist(const QString& serverId, qint64 playlistId) {
    Server& server = m_servers[serverId];
    server.playlists[playlistId].enabled = true;
    if (!m_options.readPlaylist) {
        return;
    }
    m_options.readPlaylist(serverId,
            playlistId,
            [this, serverId, playlistId](std::optional<QList<CrateTrackRef>> tracks) {
                const auto server = m_servers.find(serverId);
                if (server == m_servers.end() || !tracks) {
                    return;
                }
                const auto playlist = server->playlists.find(playlistId);
                if (playlist == server->playlists.end() || !playlist->enabled) {
                    return;
                }
                playlist->tracks = *tracks;
                const QString reason = pinReasonOf(playlistId);
                QList<CrateTrackRef> uncached;
                qint64 bytes = 0;
                for (const CrateTrackRef& track : std::as_const(*tracks)) {
                    if (m_options.pCache &&
                            m_options.pCache->cachedPath(track.serverId, track.entityUid)) {
                        m_options.pCache->pin(track.serverId, track.entityUid, reason);
                        enqueue(track, false);
                        continue;
                    }
                    uncached.append(track);
                    bytes += track.hints.byteLength.value_or(0);
                }
                emit playlistsChanged(serverId);
                if (uncached.isEmpty()) {
                    return;
                }
                const auto download = [this, serverId, playlistId, uncached, reason](
                                              bool accepted) {
                    const auto server = m_servers.find(serverId);
                    if (!accepted || server == m_servers.end() ||
                            !server->playlists.value(playlistId).enabled) {
                        return;
                    }
                    if (m_options.download) {
                        m_options.download(uncached, reason);
                    }
                    for (const CrateTrackRef& track : uncached) {
                        enqueue(track, false);
                    }
                };
                if (m_options.confirmDownload) {
                    m_options.confirmDownload(serverId,
                            playlistId,
                            static_cast<int>(uncached.size()),
                            bytes,
                            download);
                } else {
                    download(true);
                }
            });
}

void StemJobs::disablePlaylist(const QString& serverId, qint64 playlistId) {
    Server& server = m_servers[serverId];
    const Playlist playlist = server.playlists.value(playlistId);
    server.playlists[playlistId] = Playlist();
    if (m_options.pCache) {
        m_options.pCache->unpinAll(serverId, pinReasonOf(playlistId));
    }
    // Its tracks' stems no longer come first
    QSet<QString> keys;
    for (const CrateTrackRef& track : playlist.tracks) {
        keys.insert(keyOf(track.serverId, track.entityUid));
    }
    std::erase_if(m_queue, [this, &keys](const Job& job) {
        const QString key = keyOf(job.track.serverId, job.track.entityUid);
        if (!keys.contains(key)) {
            return false;
        }
        m_states.remove(key);
        return true;
    });
}

void StemJobs::requestStems(const CrateTrackRef& track) {
    enqueue(track, true);
}

void StemJobs::enqueue(const CrateTrackRef& track, bool front) {
    const QString key = keyOf(track.serverId, track.entityUid);
    const auto server = m_servers.constFind(track.serverId);
    if (server != m_servers.cend()) {
        const auto stem = server->stems.constFind(track.entityUid.toLower());
        if (stem != server->stems.cend() &&
                (track.hints.sourceRevision.isEmpty() ||
                        stem.value() == track.hints.sourceRevision)) {
            return;
        }
    }
    if (m_running && keyOf(m_running->track.serverId, m_running->track.entityUid) == key) {
        return;
    }
    const auto queued = std::find_if(m_queue.begin(), m_queue.end(), [&key](const Job& job) {
        return keyOf(job.track.serverId, job.track.entityUid) == key;
    });
    if (queued != m_queue.end()) {
        if (!front) {
            return;
        }
        m_queue.erase(queued);
    }
    if (front) {
        m_queue.push_front(Job{track});
    } else {
        m_queue.push_back(Job{track});
    }
    setState(track, TrackState::Queued);
    QTimer::singleShot(0, this, &StemJobs::pump);
}

void StemJobs::setState(const CrateTrackRef& track, TrackState state) {
    const QString key = keyOf(track.serverId, track.entityUid);
    if (m_states.value(key, TrackState::None) == state) {
        return;
    }
    m_states.insert(key, state);
    emit trackStateChanged(track.serverId, track.entityUid);
}

StemJobs::TrackState StemJobs::trackState(
        const QString& serverId, const QString& entityUid) const {
    return m_states.value(keyOf(serverId, entityUid), TrackState::None);
}

std::optional<StemJobs::PlaylistState> StemJobs::playlistState(
        const QString& serverId, qint64 playlistId) const {
    const auto server = m_servers.constFind(serverId);
    if (server == m_servers.cend()) {
        return std::nullopt;
    }
    const auto playlist = server->playlists.constFind(playlistId);
    if (playlist == server->playlists.cend() || !playlist->enabled) {
        return std::nullopt;
    }
    PlaylistState state;
    state.enabled = true;
    state.tracks = static_cast<int>(playlist->tracks.size());
    for (const CrateTrackRef& track : playlist->tracks) {
        if (trackState(track.serverId, track.entityUid) == TrackState::Ready) {
            ++state.ready;
        }
    }
    return state;
}

bool StemJobs::mustPause() const {
    if (m_options.separateWhilePlaying && m_options.separateWhilePlaying()) {
        return false;
    }
    return m_options.anyDeckPlays ? m_options.anyDeckPlays() : decksPlay();
}

void StemJobs::checkPause() {
    if (m_separating && mustPause()) {
        // A deck started: the separation stops and starts again later
        kLogger.info() << "A deck plays: separation pauses";
        if (m_options.pSeparator) {
            m_options.pSeparator->cancel();
        }
        m_separating = false;
        const Job job = *m_running;
        m_running.reset();
        ++m_generation;
        m_queue.push_front(job);
        setState(job.track, TrackState::Queued);
    }
    if (!m_running) {
        pump();
    }
    if (!m_running && !m_paused) {
        m_pauseTimer.stop();
    }
}

void StemJobs::pump() {
    if (m_running || m_queue.empty()) {
        return;
    }
    m_paused = mustPause();
    if (!m_pauseTimer.isActive()) {
        m_pauseTimer.start();
    }
    if (m_paused) {
        return;
    }
    m_running = m_queue.front();
    m_queue.pop_front();
    runJob();
}

void StemJobs::runJob() {
    if (!m_running || !m_options.pCache || !m_options.pSync) {
        finishJob(TrackState::Failed);
        return;
    }
    const CrateTrackRef track = m_running->track;
    const quint64 generation = ++m_generation;
    setState(track, TrackState::Working);
    const std::optional<QString> path =
            m_options.pCache->cachedPath(track.serverId, track.entityUid);
    if (!path) {
        // Its file first, as a prefetch would
        cache::CacheTicket* pTicket = m_options.pCache->ensure(
                track.serverId, track.entityUid, cache::Priority::Prefetch, track.hints);
        m_pTicket = pTicket;
        connect(pTicket, &cache::CacheTicket::ready, this, [this, generation] {
            if (generation == m_generation) {
                m_pTicket = nullptr;
                runJob();
            }
        });
        connect(pTicket,
                &cache::CacheTicket::failed,
                this,
                [this, generation](const crate::Error& error) {
                    if (generation == m_generation) {
                        kLogger.warning() << "No file to separate:" << error.message;
                        m_pTicket = nullptr;
                        finishJob(TrackState::Failed);
                    }
                });
        return;
    }
    const QString sourceRevision = track.hints.sourceRevision;
    m_options.pSync->fetch(track.serverId,
            track.entityUid,
            *path,
            sourceRevision,
            [this, generation, cachedPath = *path, sourceRevision](
                    const StemSync::Outcome& outcome) {
                if (generation != m_generation || !m_running) {
                    return;
                }
                switch (outcome.result) {
                case StemSync::Outcome::Result::ServerStems:
                    recordStems(m_running->track, outcome.sidecarPath, false);
                    finishJob(TrackState::Ready);
                    return;
                case StemSync::Outcome::Result::NoServerStems:
                    if (m_options.pSeparator && m_options.pSeparator->isAvailable()) {
                        separateLocally(cachedPath, sourceRevision);
                    } else {
                        finishJob(TrackState::Unavailable);
                    }
                    return;
                case StemSync::Outcome::Result::Discarded:
                    finishJob(TrackState::None);
                    return;
                case StemSync::Outcome::Result::Failed:
                    finishJob(TrackState::Failed);
                    return;
                }
            });
}

void StemJobs::separateLocally(const QString& cachedPath, const QString& sourceRevision) {
    // Created and let go of on the main thread
    const TrackPointer pTrack = Track::newTemporary(cachedPath);
    StemSidecarWriter::Options options;
    options.pTrack = pTrack;
    options.sourceRevision = sourceRevision;
    auto pSink = std::make_shared<TeeSink>(options);
    const quint64 generation = m_generation;
    m_separating = true;
    if (!m_pauseTimer.isActive()) {
        m_pauseTimer.start();
    }
    m_options.pSeparator->separate(pTrack,
            pSink,
            [this, generation, pSink, pTrack, sourceRevision](bool done, const QString& error) {
                if (generation != m_generation || !m_running) {
                    return;
                }
                m_separating = false;
                if (!done) {
                    kLogger.warning() << "The separation failed:" << error;
                    finishJob(TrackState::Failed);
                    return;
                }
                auto* pWatcher = new QFutureWatcher<QString>(this);
                connect(pWatcher,
                        &QFutureWatcherBase::finished,
                        this,
                        [this, pWatcher, generation, pSink, pTrack, sourceRevision] {
                            pWatcher->deleteLater();
                            if (generation != m_generation || !m_running) {
                                QFile::remove(pSink->writer.sidecarPath());
                                return;
                            }
                            const QString error = pWatcher->result();
                            if (!error.isEmpty()) {
                                kLogger.warning() << "Cannot write the stems:" << error;
                                finishJob(TrackState::Failed);
                                return;
                            }
                            const CrateTrackRef track = m_running->track;
                            if (!m_options.pSync->isCurrent(
                                        track.serverId, track.entityUid, sourceRevision)) {
                                QFile::remove(pSink->writer.sidecarPath());
                                finishJob(TrackState::None);
                                return;
                            }
                            const StemAnalysis analysis =
                                    pSink->analyzer.finish(sourceRevision);
                            const QString sidecarPath = pSink->writer.sidecarPath();
                            recordStems(track, sidecarPath, false);
                            finishJob(TrackState::Ready);
                            m_options.pSync->upload(track.serverId,
                                    track.entityUid,
                                    analysis,
                                    [this, track, sidecarPath](StemSync::UploadResult result) {
                                        if (result == StemSync::UploadResult::Stored ||
                                                result == StemSync::UploadResult::KeptStronger) {
                                            recordStems(track, sidecarPath, true);
                                        }
                                    });
                        });
                pWatcher->setFuture(QtConcurrent::run(&m_pool, [pSink]() {
                    QString error;
                    return pSink->writer.finish(&error) ? QString() : error;
                }));
            });
}

void StemJobs::finishJob(TrackState state) {
    if (m_running) {
        setState(m_running->track, state);
    }
    m_running.reset();
    m_pTicket = nullptr;
    m_separating = false;
    ++m_generation;
    QTimer::singleShot(0, this, &StemJobs::pump);
}

void StemJobs::recordStems(const CrateTrackRef& track, const QString& sidecarPath, bool uploaded) {
    const auto server = m_servers.find(track.serverId);
    if (server != m_servers.end()) {
        server->stems.insert(track.entityUid.toLower(), track.hints.sourceRevision);
    }
    if (m_options.pCache) {
        m_options.pCache->refreshCompanions(track.serverId, track.entityUid);
    }
    emit playlistsChanged(track.serverId);
    if (!m_options.pDbConnectionPool) {
        return;
    }
    QString separator = StemAnalysis::kSeparatorVersion;
    if (const auto info = StemSidecar::readInfo(sidecarPath)) {
        separator = info->separator;
    }
    const QString relativePath = m_options.pCache
            ? QDir(m_options.pCache->directory()).relativeFilePath(sidecarPath)
            : sidecarPath;
    m_pool.start([pPool = m_options.pDbConnectionPool,
                         track,
                         relativePath,
                         separator,
                         uploaded,
                         now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate)] {
        const mixxx::DbConnectionPooler pooler(pPool);
        const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
        for (int attempt = 0; attempt < kDbAttempts; ++attempt) {
            if (attempt > 0) {
                QThread::msleep(kDbRetryMillis * static_cast<unsigned long>(attempt));
            }
            QSqlQuery query(database);
            query.prepare(QStringLiteral(
                    "INSERT INTO crateserver_stems (server_id, entity_uid, path, "
                    "separator_model, source_revision, created_at, uploaded_at) "
                    "VALUES (:server, :uid, :path, :separator, :revision, :now, :uploaded) "
                    "ON CONFLICT (server_id, entity_uid) DO UPDATE SET path = excluded.path, "
                    "separator_model = excluded.separator_model, "
                    "source_revision = excluded.source_revision, "
                    "created_at = CASE WHEN excluded.uploaded_at IS NULL "
                    "THEN excluded.created_at ELSE crateserver_stems.created_at END, "
                    "uploaded_at = excluded.uploaded_at"));
            query.bindValue(QStringLiteral(":server"), track.serverId);
            query.bindValue(QStringLiteral(":uid"), track.entityUid.toLower());
            query.bindValue(QStringLiteral(":path"), relativePath);
            query.bindValue(QStringLiteral(":separator"), separator);
            query.bindValue(QStringLiteral(":revision"), track.hints.sourceRevision);
            query.bindValue(QStringLiteral(":now"), now);
            query.bindValue(QStringLiteral(":uploaded"), uploaded ? QVariant(now) : QVariant());
            if (query.exec()) {
                return;
            }
        }
        kLogger.warning() << "Cannot record the stems of" << track.entityUid;
    });
}

} // namespace crate

#include "moc_stemjobs.cpp"
