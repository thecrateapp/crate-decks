#include "library/crateserver/crateserverfeature.h"

#include <QFutureWatcher>
#include <QIconEngine>
#include <QJsonDocument>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QScopeGuard>
#include <QSet>
#include <QtConcurrentRun>

#include "crate/analysis/analysisimporter.h"
#include "crate/analysis/profilestore.h"
#include "crate/cache/audiocache.h"
#include "crate/client/cratecapabilities.h"
#include "crate/client/crateclient.h"
#include "crate/cratepreferences.h"
#include "crate/crateservers.h"
#include "crate/library/crateeventsubscriber.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarysync.h"
#include "crate/library/crateprefetcher.h"
#include "crate/library/cratesearch.h"
#include "crate/library/cratesessionpins.h"
#include "crate/library/cratetrackpreparer.h"
#include "crate/ui/cachestatusview.h"
#include "library/crateserver/crateservertrackmodel.h"
#include "library/library.h"
#include "library/library_prefs.h"
#include "library/librarytablemodel.h"
#include "library/sidebarmodel.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "mixer/playerinfo.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::CrateServerFeature");

const QString kIconName = QStringLiteral("crateserver");

/// The sections of a server, in the sidebar's order (GU1 mockups).
const QList<Section> kSections = {
        Section::Playlists,
        Section::Crates,
        Section::Genres,
        Section::Moods,
        Section::Recent,
        Section::Likes,
};

/// An icon that names the Solar icon the CrateDecks sidebar paints
/// (SidebarDelegate reads QIcon::name()) and paints Mixxx's own in other
/// skins: Mixxx's SidebarModel forwards a child item's QIcon but not a
/// custom role.
class NamedIconEngine : public QIconEngine {
  public:
    NamedIconEngine(QString name, QString fallbackPath)
            : m_name(std::move(name)),
              m_fallbackPath(std::move(fallbackPath)),
              m_fallback(m_fallbackPath) {
    }

    void paint(QPainter* pPainter, const QRect& rect, QIcon::Mode mode, QIcon::State state)
            override {
        m_fallback.paint(pPainter, rect, Qt::AlignCenter, mode, state);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return m_fallback.pixmap(size, mode, state);
    }
    QIconEngine* clone() const override {
        return new NamedIconEngine(m_name, m_fallbackPath);
    }
    QString key() const override {
        return QStringLiteral("CrateNamedIconEngine");
    }
    QString iconName() override {
        return m_name;
    }

  private:
    const QString m_name;
    const QString m_fallbackPath;
    const QIcon m_fallback;
};

QIcon namedIcon(const QString& name, const QString& mixxxIcon) {
    return QIcon(new NamedIconEngine(name,
            QStringLiteral(":/images/library/ic_library_%1.svg").arg(mixxxIcon)));
}

QIcon sectionIcon(Section section) {
    switch (section) {
    case Section::Playlists:
        return namedIcon(QStringLiteral("crate-playlists"), QStringLiteral("playlist"));
    case Section::Crates:
        return namedIcon(QStringLiteral("crate-crates"), QStringLiteral("crates"));
    case Section::Genres:
        return namedIcon(QStringLiteral("crate-genres"), QStringLiteral("crates"));
    case Section::Moods:
        return namedIcon(QStringLiteral("crate-moods"), QStringLiteral("crates"));
    case Section::Recent:
        return namedIcon(QStringLiteral("crate-recent"), QStringLiteral("history"));
    case Section::Likes:
        return namedIcon(QStringLiteral("crate-likes"), QStringLiteral("tracks"));
    }
    return QIcon();
}

QString sectionName(Section section) {
    switch (section) {
    case Section::Playlists:
        return CrateServerFeature::tr("Playlists");
    case Section::Crates:
        return CrateServerFeature::tr("Crates");
    case Section::Genres:
        return CrateServerFeature::tr("Genres");
    case Section::Moods:
        return CrateServerFeature::tr("Moods");
    case Section::Recent:
        return CrateServerFeature::tr("Recent");
    case Section::Likes:
        return CrateServerFeature::tr("Likes");
    }
    return QString();
}

/// "Name (12)", which the CrateDecks sidebar splits into a label and a
/// count on the right, as Mixxx's own features write their counts.
QString withCount(const QString& label, std::optional<qint64> count) {
    if (!count || *count <= 0) {
        return label;
    }
    return QStringLiteral("%1 (%2)").arg(label).arg(*count);
}

/// What a container's sidebar item counts: its synced tracks, else the
/// server's count when it has one.
std::optional<qint64> countOf(const StoredContainer& container) {
    if (const auto synced = container.syncedTrackCount()) {
        return synced;
    }
    return container.trackCount;
}

// Item data: what each sidebar item stands for
QString libraryData(const QString& serverId) {
    return QStringLiteral("library\n") + serverId;
}
QString serverData(const QString& serverId) {
    return QStringLiteral("server\n") + serverId;
}
QString sectionData(const QString& serverId, Section section) {
    return QStringLiteral("section\n%1\n%2").arg(serverId).arg(static_cast<int>(section));
}
QString containerData(const QString& serverId, qint64 containerId) {
    return QStringLiteral("container\n%1\n%2").arg(serverId).arg(containerId);
}
QString cacheData() {
    return QStringLiteral("cache\n");
}

QIcon cacheIcon() {
    return namedIcon(QStringLiteral("crate-cache"), QStringLiteral("tracks"));
}

/// Likes and Recent are one container each: their item is the container.
bool isSingleContainer(Section section, const QList<StoredContainer>& containers) {
    return (section == Section::Likes || section == Section::Recent) && containers.size() == 1;
}

QStringList childData(const TreeItem* pItem) {
    QStringList data;
    for (const TreeItem* pChild : pItem->children()) {
        data.append(pChild->getData().toString());
    }
    return data;
}

} // namespace

const QString CrateServerFeature::kCacheViewName = QStringLiteral("CRATE_CACHE");

struct CrateServerFeature::Server {
    QString id;
    std::unique_ptr<LibrarySync> pSync;
    std::unique_ptr<EventSubscriber> pEvents;
    std::unique_ptr<Search> pSearch;
    enum class Readiness {
        Unknown,
        Checking,
        Ready,
        /// The server does not serve the library (no library_cursor).
        Unsupported,
    };
    Readiness readiness = Readiness::Unknown;
    QList<std::function<void()>> waiting;
    /// The containers asked for in this run, so a clean one is revalidated
    /// once (syncIfNeeded()).
    QSet<qint64> revalidated;
};

CrateServerFeature::CrateServerFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : CrateServerFeature(pLibrary,
                  pConfig,
                  Dependencies{pLibrary->trackCollectionManager(),
                          pLibrary->dbConnectionPool(),
                          nullptr,
                          // CoreServices creates it before the library
                          &PlayerInfo::instance()}) {
}

CrateServerFeature::CrateServerFeature(Library* pLibrary,
        UserSettingsPointer pConfig,
        const Dependencies& dependencies)
        : LibraryFeature(pLibrary, pConfig, kIconName),
          m_dependencies(dependencies),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pContainerModel(make_parented<CrateServerTrackModel>(
                  CrateServerTrackModel::Source::Container,
                  dependencies.pTrackCollectionManager->internalCollection()->database(),
                  dependencies.pDbConnectionPool,
                  this)),
          m_pLibraryModel(make_parented<CrateServerTrackModel>(
                  CrateServerTrackModel::Source::Library,
                  dependencies.pTrackCollectionManager->internalCollection()->database(),
                  dependencies.pDbConnectionPool,
                  this)),
          m_pSearchModel(make_parented<CrateServerTrackModel>(
                  CrateServerTrackModel::Source::Container,
                  dependencies.pTrackCollectionManager->internalCollection()->database(),
                  dependencies.pDbConnectionPool,
                  this)),
          m_pCacheStatus(make_parented<CacheStatusModel>(
                  CacheStatusModel::Options{nullptr,
                          nullptr,
                          [this](const QList<cache::TrackKey>& tracks,
                                  const CacheStatusModel::TitlesReady& ready) {
                              lookUpTitles(tracks, ready);
                          },
                          [this](const QString& serverId, const QString& reason) {
                              return offlineSetName(serverId, reason);
                          }},
                  this)) {
    m_pool.setMaxThreadCount(1);
    for (CrateServerTrackModel* pModel :
            {m_pContainerModel.get(), m_pLibraryModel.get(), m_pSearchModel.get()}) {
        pModel->setSearchHandler([this, pModel](const QString& text) {
            return onSearch(pModel, text);
        });
    }
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
    if (m_dependencies.pServers) {
        watchServers(m_dependencies.pServers);
    }
}

CrateServerFeature::~CrateServerFeature() {
    stopLoading();
    m_servers.clear();
    m_pool.waitForDone();
}

CrateServers* CrateServerFeature::servers() {
    if (!m_pServers && !m_dependencies.pServers && CrateServers::instance()) {
        watchServers(CrateServers::instance());
    }
    return m_pServers;
}

void CrateServerFeature::watchServers(CrateServers* pServers) {
    m_pServers = pServers;
    connect(pServers, &CrateServers::serversChanged, this, &CrateServerFeature::updateServers);
    // CoreServices destroys the servers before the library: their Clients go
    // first, and the syncs with them
    connect(pServers, &QObject::destroyed, this, [this] {
        // Half destroyed: nothing of it is used any more
        m_pServers = nullptr;
        stopLoading();
        m_servers.clear();
        m_pool.waitForDone();
        updateServers();
    });
    updateServers();
}

void CrateServerFeature::updateServers() {
    QStringList ids;
    if (m_pServers) {
        const QList<Session*> sessions = m_pServers->sessions();
        for (const Session* pSession : sessions) {
            const QString& id = pSession->profile().id;
            if (m_pServers->isSaved(id)) {
                ids.append(id);
            }
        }
    }
    if (ids == m_serverIds && m_pSidebarModel->getRootItem()->hasChildren() == !ids.isEmpty()) {
        return;
    }
    m_serverIds = ids;

    // The servers that went, and the ones that came
    for (auto it = m_servers.begin(); it != m_servers.end();) {
        it = ids.contains(it->first) ? std::next(it) : m_servers.erase(it);
    }
    for (const QString& id : std::as_const(ids)) {
        if (m_servers.find(id) != m_servers.end()) {
            continue;
        }
        auto pServer = std::make_unique<Server>();
        pServer->id = id;
        pServer->pSync = std::make_unique<LibrarySync>(
                id, m_pServers->client(id), m_dependencies.pDbConnectionPool);
        LibrarySync* pSync = pServer->pSync.get();
        if (Client* pClient = m_pServers->client(id)) {
            pServer->pSearch = std::make_unique<Search>(
                    id, pClient, m_dependencies.pDbConnectionPool);
            connect(pServer->pSearch.get(),
                    &Search::resultsReady,
                    this,
                    [this, id](qint64 containerId, const QString& query) {
                        onSearchResults(id, containerId, query);
                    });
        }
        connect(pSync, &LibrarySync::containersUpdated, this, [this, id] {
            loadSidebar(id);
        });
        connect(pSync, &LibrarySync::containerUpdated, this, [this, id](qint64 containerId) {
            if (m_pContainerModel->containerId() == containerId) {
                m_pContainerModel->select();
            }
            if (m_pLibraryModel->serverId() == id) {
                m_pLibraryModel->select();
            }
            loadSidebar(id);
        });
        connect(pSync,
                &LibrarySync::containersDirty,
                this,
                [this, id](const QList<qint64>& containerIds) {
                    // The one on screen is fetched now; the rest when shown
                    if (containerIds.contains(m_pContainerModel->containerId())) {
                        syncIfNeeded(id, m_pContainerModel->containerId());
                    }
                });
        connect(pSync, &LibrarySync::syncFailed, this, [this, id](qint64 containerId) {
            // Tried again the next time it is shown
            if (Server* pFailed = server(id)) {
                pFailed->revalidated.remove(containerId);
            }
        });
        m_servers.emplace(id, std::move(pServer));
    }

    // The tree: one server's sections right under the feature, or one item
    // per server
    std::unique_ptr<TreeItem> pRoot = TreeItem::newRoot(this);
    if (ids.size() == 1) {
        appendDefaultSections(pRoot.get(), ids.first());
    } else {
        for (const QString& id : std::as_const(ids)) {
            TreeItem* pServerItem = pRoot->appendChild(titleOf(id), serverData(id));
            pServerItem->setIcon(namedIcon(
                    QStringLiteral("crate-server"), QStringLiteral("tracks")));
            appendDefaultSections(pServerItem, id);
        }
    }
    if (!ids.isEmpty()) {
        // Last, as in the GU1 mockups: one cache for every server
        pRoot->appendChild(cacheLabel(), cacheData())->setIcon(cacheIcon());
    }
    m_pSidebarModel->setRootItem(std::move(pRoot));
    // The title changed with the servers
    emit featureIsLoading(this, false);
    expandOwnItems();
    for (const QString& id : std::as_const(ids)) {
        loadSidebar(id);
    }
    if (!ids.isEmpty()) {
        startLoading();
    }
}

void CrateServerFeature::startLoading() {
    TrackCollectionManager* const pCollection = m_dependencies.pTrackCollectionManager;
    if (m_pLoadCoordinator || !m_pServers || !pCollection) {
        return;
    }
    CrateLoadCoordinator::Options options;
    options.pCache = m_pServers->audioCache();
    options.pConfig = m_pConfig;
    options.trackOfFile = [pCollection](const QString& path) {
        return pCollection->getOrAddTrack(TrackRef::fromFilePath(path));
    };
    options.trackIdsOfFiles = CrateLoadCoordinator::trackIdsOfFilesIn(pCollection);
    options.trackInLibrary = [pCollection](const QString& path) {
        return pCollection->getTrackByRef(TrackRef::fromFilePath(path));
    };
    // On a worker: the settings are read under their lock
    options.prepareTrack = [pConfig = m_pConfig](const QString& path) {
        return prepareTrackForLibrary(path, SyncTrackMetadataParams::readFromUserSettings(*pConfig));
    };
    options.trackById = [pCollection](TrackId trackId) {
        return pCollection->getTrackById(trackId);
    };
    options.pPlayerInfo = m_dependencies.pPlayerInfo;
    options.applyAnalysis =
            [this](const TrackPointer& pTrack, const QString& serverId, const QString& entityUid) {
                applyAnalysis(pTrack, serverId, entityUid);
            };
    m_pLoadCoordinator = std::make_unique<CrateLoadCoordinator>(std::move(options));
    connect(m_pLoadCoordinator.get(),
            &CrateLoadCoordinator::loadTrackToPlayer,
            this,
            [this](const TrackPointer& pTrack, const QString& group, bool play) {
                emit loadTrackToPlayer(pTrack,
                        group,
#ifdef __STEM__
                        mixxx::StemChannelSelection(),
#endif
                        play);
            });
    connect(m_pLoadCoordinator.get(),
            &CrateLoadCoordinator::loadTrackIntoNextAvailableDeck,
            this,
            &LibraryFeature::loadTrack);
    for (CrateServerTrackModel* pModel :
            {m_pContainerModel.get(), m_pLibraryModel.get(), m_pSearchModel.get()}) {
        pModel->setLoadCoordinator(m_pLoadCoordinator.get());
    }

    CratePreferences* const pPreferences = m_pServers->preferences();
    CratePrefetcher::Options prefetch;
    prefetch.pCache = m_pServers->audioCache();
    prefetch.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    prefetch.pPlaylistDao = &pCollection->internalCollection()->getPlaylistDAO();
    prefetch.enabled = pPreferences->prefetchAutoDj();
    m_pPrefetcher = std::make_unique<CratePrefetcher>(std::move(prefetch));
    // The LRU never evicts what is loaded or queued (design §9)
    CrateSessionPins::Options pins;
    pins.pCache = m_pServers->audioCache();
    pins.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    pins.pPlaylistDao = &pCollection->internalCollection()->getPlaylistDAO();
    pins.pPlayerInfo = m_dependencies.pPlayerInfo;
    m_pSessionPins = std::make_unique<CrateSessionPins>(std::move(pins));
    connect(pPreferences,
            &CratePreferences::prefetchAutoDjChanged,
            this,
            &CrateServerFeature::onPrefetchAutoDjChanged,
            Qt::UniqueConnection);

    cache::AudioCache* const pCache = m_pServers->audioCache();
    m_pCacheStatus->setSources(pCache, pPreferences);
    connect(pCache,
            &cache::AudioCache::usageChanged,
            this,
            &CrateServerFeature::updateCacheItem,
            Qt::UniqueConnection);
    connect(pCache,
            &cache::AudioCache::loaded,
            this,
            &CrateServerFeature::updateCacheItem,
            Qt::UniqueConnection);
    updateCacheItem();
}

void CrateServerFeature::onPrefetchAutoDjChanged(bool enabled) {
    if (m_pPrefetcher) {
        m_pPrefetcher->setEnabled(enabled);
    }
}

void CrateServerFeature::stopLoading() {
    for (CrateServerTrackModel* pModel :
            {m_pContainerModel.get(), m_pLibraryModel.get(), m_pSearchModel.get()}) {
        pModel->setLoadCoordinator(nullptr);
    }
    m_pSessionPins.reset();
    m_pPrefetcher.reset();
    m_pLoadCoordinator.reset();
    m_pCacheStatus->setSources(nullptr, nullptr);
}

QString CrateServerFeature::cacheLabel() const {
    const cache::AudioCache* pCache = m_pServers ? m_pServers->existingAudioCache() : nullptr;
    const qint64 bytes = pCache ? pCache->usage().readyBytes : 0;
    if (bytes <= 0) {
        return tr("Cache");
    }
    // The use as the item's count (GU1 mockups: "Cache  18.4 GB")
    return QStringLiteral("%1 (%2)").arg(tr("Cache"), formatCacheBytes(bytes));
}

void CrateServerFeature::updateCacheItem() {
    TreeItem* pRoot = m_pSidebarModel->getRootItem();
    for (TreeItem* pChild : pRoot->children()) {
        if (pChild->getData().toString() == cacheData()) {
            const QString label = cacheLabel();
            if (pChild->getLabel() != label) {
                pChild->setLabel(label);
                m_pSidebarModel->triggerRepaint(indexOf(pChild));
            }
            return;
        }
    }
}

void CrateServerFeature::bindLibraryWidget(
        WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) {
    Q_UNUSED(pKeyboard);
    auto* pView = new CacheStatusView(m_pCacheStatus, pLibraryWidget);
    pLibraryWidget->registerView(kCacheViewName, pView);
}

void CrateServerFeature::lookUpTitles(const QList<cache::TrackKey>& tracks,
        std::function<void(const QHash<QString, CacheTrackTitle>&)> ready) {
    using Titles = QHash<QString, CacheTrackTitle>;
    auto* pWatcher = new QFutureWatcher<Titles>(this);
    connect(pWatcher,
            &QFutureWatcherBase::finished,
            this,
            [pWatcher, ready = std::move(ready)] {
                pWatcher->deleteLater();
                ready(pWatcher->result());
            });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_dependencies.pDbConnectionPool, tracks]() {
                const mixxx::DbConnectionPooler pooler(pPool);
                const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
                Titles titles;
                for (const cache::TrackKey& track : tracks) {
                    for (int attempt = 0; attempt < 4; ++attempt) {
                        if (attempt > 0) {
                            QThread::msleep(25);
                        }
                        QSqlQuery query(database);
                        query.prepare(QStringLiteral(
                                "SELECT title, artist FROM crateserver_tracks "
                                "WHERE server_id = :server AND entity_uid = :uid"));
                        query.bindValue(QStringLiteral(":server"), track.serverId);
                        query.bindValue(QStringLiteral(":uid"), track.entityUid);
                        if (!query.exec()) {
                            continue;
                        }
                        if (query.next()) {
                            titles.insert(track.serverId + QLatin1Char('/') + track.entityUid,
                                    CacheTrackTitle{query.value(0).toString(),
                                            query.value(1).toString()});
                        }
                        break;
                    }
                }
                return titles;
            }));
}

QString CrateServerFeature::offlineSetName(const QString& serverId, const QString& reason) const {
    // "offline-set:<kind>:<remote id>", or "offline-set:<container id>"
    // (downloadOfflineSet())
    const QStringList parts = reason.mid(CacheStatusModel::kOfflineSetPrefix.size())
                                      .split(QLatin1Char(':'));
    for (const StoredContainer& container : m_containers) {
        if (container.serverId != serverId) {
            continue;
        }
        if ((parts.size() == 2 && container.kind == parts[0] && container.remoteId == parts[1]) ||
                (parts.size() == 1 && QString::number(container.id) == parts[0])) {
            return container.name;
        }
    }
    return QString();
}

void CrateServerFeature::applyAnalysis(
        const TrackPointer& pTrack, const QString& serverId, const QString& entityUid) {
    analysis::ProfileStore* const pStore =
            m_pServers ? m_pServers->profileStore(serverId) : nullptr;
    if (!pStore || !pTrack) {
        return;
    }
    const QString uid = entityUid.toLower();
    // What the Crate preferences say when the track is taken
    const analysis::Policy policy = m_pServers->preferences()->analysisPolicy();
    analysis::ProfileFetch* pFetch = pStore->get({uid});
    connect(pFetch,
            &analysis::ProfileFetch::finished,
            this,
            [pTrack, pStore = QPointer<analysis::ProfileStore>(pStore), uid, policy](
                    const analysis::ProfileResults& results) {
                const auto it = results.profiles.constFind(uid);
                if (it == results.profiles.cend()) {
                    return;
                }
                const analysis::ImportOutcome outcome = analysis::AnalysisImporter::apply(
                        pTrack, it->profile, policy, it->appliedCues);
                if (outcome.appliedCues && pStore) {
                    pStore->recordAppliedCues(uid, *outcome.appliedCues);
                }
            });
}

void CrateServerFeature::readOfflineSet(const QString& serverId,
        qint64 containerId,
        std::function<void(const OfflineSet&)> ready) {
    auto* pWatcher = new QFutureWatcher<QList<CrateTrackRef>>(this);
    connect(pWatcher,
            &QFutureWatcherBase::finished,
            this,
            [this, pWatcher, ready = std::move(ready)] {
                pWatcher->deleteLater();
                OfflineSet set;
                QSet<QString> seen;
                const QList<CrateTrackRef> tracks = pWatcher->result();
                for (const CrateTrackRef& track : tracks) {
                    if (seen.contains(track.entityUid) ||
                            (m_pLoadCoordinator &&
                                    m_pLoadCoordinator->isCached(
                                            track.serverId, track.entityUid))) {
                        continue;
                    }
                    seen.insert(track.entityUid);
                    set.tracks.append(track);
                    if (track.hints.byteLength) {
                        set.knownBytes += *track.hints.byteLength;
                    } else {
                        ++set.unknownSizes;
                    }
                }
                ready(set);
            });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_dependencies.pDbConnectionPool, serverId, containerId]() {
                const mixxx::DbConnectionPooler pooler(pPool);
                const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
                QList<CrateTrackRef> tracks;
                for (int attempt = 0; attempt < 4; ++attempt) {
                    if (attempt > 0) {
                        QThread::msleep(25);
                    }
                    QSqlQuery query(database);
                    query.prepare(QStringLiteral(
                            "SELECT t.entity_uid, t.format, t.byte_length, t.source_revision "
                            "FROM crateserver_container_tracks ct "
                            "JOIN crateserver_tracks t ON t.server_id = :server "
                            "AND t.entity_uid = ct.entity_uid "
                            "WHERE ct.container_id = :container ORDER BY ct.position"));
                    query.bindValue(QStringLiteral(":server"), serverId);
                    query.bindValue(QStringLiteral(":container"), containerId);
                    if (!query.exec()) {
                        continue;
                    }
                    while (query.next()) {
                        CrateTrackRef track;
                        track.serverId = serverId;
                        track.entityUid = query.value(0).toString();
                        track.hints.format = query.value(1).toString();
                        if (!query.value(2).isNull()) {
                            track.hints.byteLength = query.value(2).toLongLong();
                        }
                        track.hints.sourceRevision = query.value(3).toString();
                        tracks.append(track);
                    }
                    return tracks;
                }
                kLogger.warning() << "Cannot read the tracks of the container" << containerId;
                return tracks;
            }));
}

void CrateServerFeature::downloadOfflineSet(qint64 containerId, const OfflineSet& set) {
    if (!m_pLoadCoordinator) {
        return;
    }
    const auto it = m_containers.constFind(containerId);
    // Pinned by the container's identity on the server, which survives a
    // new sync
    const QString reason = it != m_containers.cend()
            ? QStringLiteral("offline-set:%1:%2").arg(it->kind, it->remoteId)
            : QStringLiteral("offline-set:%1").arg(containerId);
    m_pLoadCoordinator->download(set.tracks, cache::Priority::OfflineSet, reason);
}

CrateServerFeature::Server* CrateServerFeature::server(const QString& serverId) const {
    const auto it = m_servers.find(serverId);
    return it == m_servers.end() ? nullptr : it->second.get();
}

QString CrateServerFeature::titleOf(const QString& serverId) const {
    const Session* pSession = m_pServers ? m_pServers->session(serverId) : nullptr;
    if (!pSession) {
        return serverId;
    }
    const QUrl& url = pSession->profile().baseUrl;
    return url.port() > 0 ? QStringLiteral("%1:%2").arg(url.host()).arg(url.port()) : url.host();
}

QVariant CrateServerFeature::title() {
    servers();
    if (m_serverIds.size() == 1) {
        return QStringLiteral("Crate · %1").arg(titleOf(m_serverIds.first()));
    }
    return QStringLiteral("Crate");
}

TreeItemModel* CrateServerFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void CrateServerFeature::appendDefaultSections(TreeItem* pParent, const QString& serverId) const {
    // Before the tree is read: the sections every server has
    TreeItem* pLibrary = pParent->appendChild(tr("Library"), libraryData(serverId));
    pLibrary->setIcon(namedIcon(QStringLiteral("crate-library"), QStringLiteral("tracks")));
    for (const Section section : {Section::Playlists, Section::Crates}) {
        TreeItem* pSection = pParent->appendChild(sectionName(section),
                sectionData(serverId, section));
        pSection->setIcon(sectionIcon(section));
    }
}

TreeItem* CrateServerFeature::serverItem(const QString& serverId) const {
    TreeItem* pRoot = m_pSidebarModel->getRootItem();
    if (m_serverIds.size() == 1) {
        return m_serverIds.first() == serverId ? pRoot : nullptr;
    }
    for (TreeItem* pChild : pRoot->children()) {
        if (pChild->getData().toString() == serverData(serverId)) {
            return pChild;
        }
    }
    return nullptr;
}

void CrateServerFeature::loadSidebar(const QString& serverId) {
    auto* pWatcher = new QFutureWatcher<SidebarData>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher, serverId] {
        pWatcher->deleteLater();
        applySidebar(serverId, pWatcher->result());
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_dependencies.pDbConnectionPool, serverId]() {
                const mixxx::DbConnectionPooler pooler(pPool);
                const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
                SidebarData data;
                for (int attempt = 0; attempt < 4 && !data.ok; ++attempt) {
                    if (attempt > 0) {
                        QThread::msleep(25);
                    }
                    QSqlQuery count(database);
                    count.prepare(QStringLiteral(
                            "SELECT COUNT(*) FROM crateserver_tracks WHERE server_id = :server"));
                    count.bindValue(QStringLiteral(":server"), serverId);
                    if (!count.exec() || !count.next()) {
                        continue;
                    }
                    data.syncedTracks = count.value(0).toLongLong();
                    count.finish();
                    data.libraryTrackCount = loadLibraryTrackCount(database, serverId);
                    data.containers = loadContainers(database, serverId);
                    data.ok = true;
                }
                return data;
            }));
}

void CrateServerFeature::applySidebar(const QString& serverId, const SidebarData& data) {
    TreeItem* pParent = serverItem(serverId);
    if (!data.ok || !pParent) {
        return;
    }
    const QPointer<CrateServerFeature> self(this);
    const auto updated = qScopeGuard([self, serverId] {
        if (self) {
            emit self->sidebarUpdated(serverId);
        }
    });
    for (auto it = m_containers.begin(); it != m_containers.end();) {
        it = it->serverId == serverId ? m_containers.erase(it) : std::next(it);
    }
    QHash<int, QList<StoredContainer>> containersOf;
    for (const StoredContainer& container : data.containers) {
        m_containers.insert(container.id, container);
        if (const auto section = sectionOf(container.kind)) {
            containersOf[static_cast<int>(*section)].append(container);
        }
    }

    // What the server's items should be
    struct Item {
        QString label;
        QString data;
        QIcon icon;
        QList<std::pair<QString, QString>> children; // label, data
    };
    QList<Item> items;
    // The whole library as the server counts it (GU1 mockups); from a server
    // that does not, the tracks synced so far
    items.append({withCount(tr("Library"), data.libraryTrackCount.value_or(data.syncedTracks)),
            libraryData(serverId),
            namedIcon(QStringLiteral("crate-library"), QStringLiteral("tracks")),
            {}});
    for (const Section section : kSections) {
        const QList<StoredContainer> containers = containersOf.value(static_cast<int>(section));
        const bool always = section == Section::Playlists || section == Section::Crates;
        if (containers.isEmpty() && !always) {
            // Not served by this server (yet): no empty placeholder
            continue;
        }
        // As the mockups: counts on Playlists, Crates and Likes only
        if (isSingleContainer(section, containers)) {
            items.append({section == Section::Likes
                            ? withCount(sectionName(section), countOf(containers.first()))
                            : sectionName(section),
                    containerData(serverId, containers.first().id),
                    sectionIcon(section),
                    {}});
            continue;
        }
        const bool counted = section == Section::Playlists || section == Section::Crates;
        Item item{counted ? withCount(sectionName(section), containers.size())
                          : sectionName(section),
                sectionData(serverId, section),
                sectionIcon(section),
                {}};
        for (const StoredContainer& container : containers) {
            item.children.append({withCount(container.name, countOf(container)),
                    containerData(serverId, container.id)});
        }
        items.append(item);
    }

    if (m_serverIds.size() == 1) {
        // The server's items are the feature's: the cache's stays last
        items.append({cacheLabel(), cacheData(), cacheIcon(), {}});
    }

    // Replace what changed: the selection and the expanded items stay
    QStringList wanted;
    for (const Item& item : std::as_const(items)) {
        wanted.append(item.data);
    }
    const QModelIndex parentIndex = indexOf(pParent);
    if (childData(pParent) != wanted) {
        if (pParent->hasChildren()) {
            m_pSidebarModel->removeRows(0, pParent->childRows(), parentIndex);
        }
        std::vector<std::unique_ptr<TreeItem>> children;
        for (const Item& item : std::as_const(items)) {
            auto pItem = std::make_unique<TreeItem>(item.label, item.data);
            pItem->setIcon(item.icon);
            for (const auto& [label, childData] : item.children) {
                pItem->appendChild(label, childData);
            }
            children.push_back(std::move(pItem));
        }
        m_pSidebarModel->insertTreeItemRows(std::move(children), 0, parentIndex);
        expandOwnItems();
        return;
    }
    QStringList wantedChildren;
    for (int row = 0; row < items.size(); ++row) {
        TreeItem* pItem = pParent->child(row);
        const Item& item = items[row];
        if (pItem->getLabel() != item.label) {
            pItem->setLabel(item.label);
            m_pSidebarModel->triggerRepaint(indexOf(pItem));
        }
        wantedChildren.clear();
        for (const auto& child : item.children) {
            wantedChildren.append(child.second);
        }
        if (childData(pItem) != wantedChildren) {
            const QModelIndex itemIndex = indexOf(pItem);
            if (pItem->hasChildren()) {
                m_pSidebarModel->removeRows(0, pItem->childRows(), itemIndex);
            }
            std::vector<std::unique_ptr<TreeItem>> children;
            for (const auto& [label, childData] : item.children) {
                children.push_back(std::make_unique<TreeItem>(label, childData));
            }
            m_pSidebarModel->insertTreeItemRows(std::move(children), 0, itemIndex);
            continue;
        }
        for (int childRow = 0; childRow < item.children.size(); ++childRow) {
            TreeItem* pChild = pItem->child(childRow);
            if (pChild->getLabel() != item.children[childRow].first) {
                pChild->setLabel(item.children[childRow].first);
                m_pSidebarModel->triggerRepaint(indexOf(pChild));
            }
        }
    }
}

QModelIndex CrateServerFeature::indexOf(const TreeItem* pItem) const {
    if (!pItem || !pItem->hasParent()) {
        return QModelIndex();
    }
    return m_pSidebarModel->index(pItem->parentRow(), 0, indexOf(pItem->parent()));
}

CrateServerFeature::Node CrateServerFeature::nodeOf(const QModelIndex& index) const {
    Node node;
    // The sidebar's own indexes and the feature model's both point at the
    // TreeItem
    const auto* pItem = static_cast<const TreeItem*>(index.internalPointer());
    if (!index.isValid() || !pItem) {
        return node;
    }
    const QStringList parts = pItem->getData().toString().split(QLatin1Char('\n'));
    if (parts.size() < 2) {
        return node;
    }
    node.serverId = parts[1];
    if (parts[0] == QLatin1String("server")) {
        node.kind = Node::Kind::Server;
    } else if (parts[0] == QLatin1String("library")) {
        node.kind = Node::Kind::Library;
    } else if (parts[0] == QLatin1String("section") && parts.size() == 3) {
        node.kind = Node::Kind::Section;
        node.section = parts[2].toInt();
    } else if (parts[0] == QLatin1String("container") && parts.size() == 3) {
        node.kind = Node::Kind::Container;
        node.containerId = parts[2].toLongLong();
    } else if (parts[0] == QLatin1String("cache")) {
        node.kind = Node::Kind::Cache;
    }
    return node;
}

void CrateServerFeature::whenReady(const QString& serverId, std::function<void()> ready) {
    Server* pServer = server(serverId);
    if (!pServer) {
        return;
    }
    switch (pServer->readiness) {
    case Server::Readiness::Ready:
        if (ready) {
            ready();
        }
        return;
    case Server::Readiness::Unsupported:
        return;
    case Server::Readiness::Checking:
        if (ready) {
            pServer->waiting.append(std::move(ready));
        }
        return;
    case Server::Readiness::Unknown:
        break;
    }
    pServer->readiness = Server::Readiness::Checking;
    if (ready) {
        pServer->waiting.append(std::move(ready));
    }
    Client* pClient = m_pServers ? m_pServers->client(serverId) : nullptr;
    if (!pClient) {
        onCapabilities(serverId, false, QByteArray());
        return;
    }
    RequestOptions anonymous;
    anonymous.authenticated = false;
    Reply* pReply = pClient->get(QStringLiteral("/api/capabilities"), anonymous);
    connect(pReply, &Reply::finished, this, [this, serverId](const JsonResult& result) {
        onCapabilities(serverId,
                result.ok(),
                result.ok() ? result.value().toJson(QJsonDocument::Compact) : QByteArray());
    });
}

void CrateServerFeature::onCapabilities(
        const QString& serverId, bool ok, const QByteArray& json) {
    Server* pServer = server(serverId);
    if (!pServer || !m_pServers) {
        return;
    }
    QByteArray capabilitiesJson = json;
    if (ok) {
        m_pServers->rememberCapabilities(serverId, json);
    } else {
        // Offline: what the server said last time
        capabilitiesJson = m_pServers->lastCapabilities(serverId);
    }
    const Result<Capabilities> capabilities =
            Capabilities::fromJson(QJsonDocument::fromJson(capabilitiesJson));
    if (!capabilities.ok() || !capabilities.value().has(DecksFeature::LibraryCursor)) {
        kLogger.info() << "The Crate server" << serverId
                       << "does not serve the library to Decks (library_cursor)";
        pServer->readiness = Server::Readiness::Unsupported;
        pServer->waiting.clear();
        return;
    }
    pServer->readiness = Server::Readiness::Ready;
    if (!pServer->pEvents && m_pServers->client(serverId)) {
        pServer->pEvents = std::make_unique<EventSubscriber>(
                m_pServers->client(serverId), pServer->pSync.get());
        pServer->pEvents->start();
    }
    // Once per run: the tree, then whatever was waiting
    pServer->pSync->syncContainers();
    const QList<std::function<void()>> waiting = std::exchange(pServer->waiting, {});
    for (const auto& ready : waiting) {
        ready();
    }
}

void CrateServerFeature::activate() {
    servers();
    if (m_serverIds.isEmpty()) {
        // No Crate server: Mixxx's own tracks
        if (m_pLibrary && m_pLibrary->trackTableModel()) {
            emit showTrackModel(m_pLibrary->trackTableModel());
        }
        return;
    }
    const QString& serverId = m_serverIds.first();
    TreeItem* pParent = serverItem(serverId);
    if (pParent && pParent->hasChildren()) {
        // Not scrolled to: the section header above it stays in view
        emit featureSelect(this, indexOf(pParent->child(0)), false);
    }
    showLibrary(serverId);
}

void CrateServerFeature::activateChild(const QModelIndex& index) {
    const Node node = nodeOf(index);
    switch (node.kind) {
    case Node::Kind::Library:
        showLibrary(node.serverId);
        return;
    case Node::Kind::Container:
        showContainer(node.serverId, node.containerId);
        return;
    case Node::Kind::Server:
    case Node::Kind::Section:
        whenReady(node.serverId, nullptr);
        return;
    case Node::Kind::Cache:
        endSearch();
        m_pCacheStatus->refresh();
        emit switchToView(kCacheViewName);
        emit disableSearch();
        emit enableCoverArtDisplay(false);
        return;
    case Node::Kind::None:
        return;
    }
}

void CrateServerFeature::onLazyChildExpandation(const QModelIndex& index) {
    const Node node = nodeOf(index);
    if (!node.serverId.isEmpty()) {
        whenReady(node.serverId, nullptr);
    }
}

void CrateServerFeature::showLibrary(const QString& serverId) {
    endSearch();
    m_pLibraryModel->setBpmPrecision(m_pConfig->getValue(
            mixxx::library::prefs::kBpmColumnPrecisionConfigKey,
            BaseTrackTableModel::kBpmColumnPrecisionDefault));
    m_pLibraryModel->showLibrary(serverId);
    emit showTrackModel(m_pLibraryModel);
    emit enableCoverArtDisplay(false);
    whenReady(serverId, nullptr);
}

void CrateServerFeature::showContainer(const QString& serverId, qint64 containerId) {
    endSearch();
    m_containerServerId = serverId;
    m_pContainerModel->setBpmPrecision(m_pConfig->getValue(
            mixxx::library::prefs::kBpmColumnPrecisionConfigKey,
            BaseTrackTableModel::kBpmColumnPrecisionDefault));
    // The stored tracks at once; the server's if they changed
    m_pContainerModel->showContainer(containerId);
    emit showTrackModel(m_pContainerModel);
    emit enableCoverArtDisplay(false);
    whenReady(serverId, [this, serverId, containerId] {
        syncIfNeeded(serverId, containerId);
    });
}

bool CrateServerFeature::onSearch(CrateServerTrackModel* pModel, const QString& text) {
    const bool fromResults = pModel == m_pSearchModel.get();
    QString serverId;
    if (fromResults) {
        serverId = m_searchServerId;
    } else if (pModel == m_pLibraryModel.get()) {
        serverId = m_pLibraryModel->serverId();
    } else {
        serverId = m_containerServerId;
    }
    Server* pServer = server(serverId);
    if (!pServer || !pServer->pSearch) {
        // Without the server's search, the words filter what is on screen
        return false;
    }
    if (text.trimmed().size() < 2) {
        if (!m_searching) {
            return false;
        }
        // Cleared: what was on screen before the search
        CrateServerTrackModel* pSearched = m_pSearchedModel;
        const bool resultsShown = m_resultsShown;
        endSearch();
        if (resultsShown && pSearched) {
            emit showTrackModel(pSearched);
        }
        return true;
    }
    if (fromResults && text == m_pSearchModel->currentSearch() && !pServer->pSearch->isBusy()) {
        // The search box showing the results' own query again
        return true;
    }
    if (!fromResults) {
        m_pSearchedModel = pModel;
    }
    m_searching = true;
    m_searchServerId = serverId;
    pServer->pSearch->search(text);
    return true;
}

void CrateServerFeature::onSearchResults(
        const QString& serverId, qint64 containerId, const QString& query) {
    if (!m_searching || serverId != m_searchServerId) {
        return;
    }
    m_pSearchModel->setBpmPrecision(m_pConfig->getValue(
            mixxx::library::prefs::kBpmColumnPrecisionConfigKey,
            BaseTrackTableModel::kBpmColumnPrecisionDefault));
    m_pSearchModel->setSearchText(query);
    m_pSearchModel->showContainer(containerId);
    if (!m_resultsShown) {
        m_resultsShown = true;
        emit showTrackModel(m_pSearchModel);
        emit enableCoverArtDisplay(false);
    }
}

void CrateServerFeature::endSearch() {
    if (Server* pServer = server(m_searchServerId); pServer && pServer->pSearch) {
        pServer->pSearch->cancel();
    }
    m_searching = false;
    m_resultsShown = false;
    m_pSearchedModel = nullptr;
    m_pSearchModel->setSearchText(QString());
}

void CrateServerFeature::syncIfNeeded(const QString& serverId, qint64 containerId) {
    Server* pServer = server(serverId);
    if (!pServer || pServer->readiness != Server::Readiness::Ready) {
        return;
    }
    // The tree read last; a container the tree has not shown yet is synced.
    // A clean one is still asked once per run: its tree ETag only follows
    // membership and order, and a mood's never changes, while its tracks'
    // metadata, profiles and availability change only its pages' ETags (S3
    // contract). Each page goes with If-None-Match, so an unchanged
    // container costs a 304 per page and no parsing.
    const auto it = m_containers.constFind(containerId);
    const bool firstThisRun = !pServer->revalidated.contains(containerId);
    if (it == m_containers.constEnd() || it->needsSync() || firstThisRun) {
        pServer->revalidated.insert(containerId);
        pServer->pSync->syncContainerTracks(containerId);
    }
}

void CrateServerFeature::onRightClickChild(const QPoint& globalPos, const QModelIndex& index) {
    // TEST_GAP: a context menu needs a running event loop with a menu open
    const Node node = nodeOf(index);
    Server* pServer = server(node.serverId);
    if (!pServer) {
        return;
    }
    QMenu menu;
    QAction* pRefresh = menu.addAction(tr("Refresh"));
    QAction* pOffline = nullptr;
    if (node.kind == Node::Kind::Container && m_pLoadCoordinator) {
        //: Downloads every track of a playlist or crate for playing without network
        pOffline = menu.addAction(tr("Download for offline use"));
    }
    QAction* const pChosen = menu.exec(globalPos);
    if (pChosen && pChosen == pOffline) {
        const qint64 containerId = node.containerId;
        readOfflineSet(node.serverId, containerId, [this, containerId](const OfflineSet& set) {
            if (set.tracks.isEmpty()) {
                QMessageBox::information(nullptr,
                        tr("Download for offline use"),
                        tr("Every track of this list is already on this computer."));
                return;
            }
            QString size = QLocale().formattedDataSize(set.knownBytes);
            if (set.unknownSizes > 0) {
                //: The size of an offline set, when some tracks' sizes are unknown
                size = tr("%1 or more").arg(size);
            }
            const auto answer = QMessageBox::question(nullptr,
                    tr("Download for offline use"),
                    tr("Download %n track(s), %1, and keep them on this computer?",
                            nullptr,
                            static_cast<int>(set.tracks.size()))
                            .arg(size));
            if (answer == QMessageBox::Yes) {
                downloadOfflineSet(containerId, set);
            }
        });
        return;
    }
    if (pChosen != pRefresh) {
        return;
    }
    if (node.kind == Node::Kind::Container) {
        const auto it = m_containers.constFind(node.containerId);
        if (it != m_containers.constEnd()) {
            pServer->pSync->markDirty(
                    {Invalidation::Scope::Container, it->kind, it->remoteId});
        }
        whenReady(node.serverId, [pSync = pServer->pSync.get(), containerId = node.containerId] {
            pSync->syncContainerTracks(containerId);
        });
        return;
    }
    whenReady(node.serverId, [pSync = pServer->pSync.get()] {
        pSync->syncContainers();
    });
}

void CrateServerFeature::bindSidebarWidget(WLibrarySidebar* pSidebarWidget) {
    m_pSidebarWidget = pSidebarWidget;
    servers();
    if (auto* pModel = qobject_cast<SidebarModel*>(pSidebarWidget->model())) {
        // A reset of the sidebar folds every item
        connect(pModel, &QAbstractItemModel::modelReset, this, [this] {
            expandOwnItems();
        });
    }
    expandOwnItems();
}

void CrateServerFeature::expandOwnItems() {
    // The section is always open, as in the mockups: its root is a header
    auto* pModel = m_pSidebarWidget ? qobject_cast<SidebarModel*>(m_pSidebarWidget->model())
                                    : nullptr;
    if (!pModel) {
        return;
    }
    const QModelIndex root = pModel->getFeatureRootIndex(this);
    if (!root.isValid()) {
        return;
    }
    m_pSidebarWidget->expand(root);
    if (m_serverIds.size() > 1) {
        for (int row = 0; row < pModel->rowCount(root); ++row) {
            m_pSidebarWidget->expand(pModel->index(row, 0, root));
        }
    }
}

} // namespace crate::library

#include "moc_crateserverfeature.cpp"
