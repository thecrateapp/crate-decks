#include "library/crateserver/crateserverfeature.h"

#include <QFutureWatcher>
#include <QIconEngine>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QScopeGuard>
#include <QSet>
#include <QTableView>
#include <QTimer>
#include <QtConcurrentRun>
#include <chrono>
#include <limits>

#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "controllers/keyboard/keyboardeventfilter.h"
#include "crate/analysis/analysisimporter.h"
#include "crate/analysis/profilestore.h"
#include "crate/cache/audiocache.h"
#include "crate/client/cratecapabilities.h"
#include "crate/client/crateclient.h"
#include "crate/cratepreferences.h"
#include "crate/crateservers.h"
#include "crate/inference/modelstore.h"
#include "crate/library/crateeventsubscriber.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarysync.h"
#include "crate/library/crateprefetcher.h"
#include "crate/library/cratesearch.h"
#include "crate/library/cratesessionpins.h"
#include "crate/library/cratetrackpreparer.h"
#include "crate/recs/compatiblesmodel.h"
#include "crate/recs/compatiblesview.h"
#include "crate/recs/cratepathsource.h"
#include "crate/recs/crateradiosource.h"
#include "crate/recs/recommendationqueue.h"
#include "crate/smartmix/assistpanel.h"
#include "crate/smartmix/autodjsmartmixadapter.h"
#include "crate/smartmix/eqmap.h"
#include "crate/smartmix/planclient.h"
#include "crate/smartmix/sethistory.h"
#include "crate/smartmix/smartmixassist.h"
#include "crate/stems/stemjobs.h"
#ifdef __CRATE_INFERENCE__
#include "crate/stems/appstemseparator.h"
#endif
#include "crate/stems/stemsync.h"
#include "crate/ui/cachestatusview.h"
#include "crate/ui/trackmenuhook.h"
#include "crate/ui/widgets/camelotwheelmodel.h"
#include "crate/ui/widgets/instrumentpainter.h"
#include "crate/writeback/cratecorrections.h"
#include "crate/writeback/cratecuesync.h"
#include "crate/writeback/cratelikes.h"
#include "crate/writeback/crateplaylistedit.h"
#include "crate/writeback/crateratings.h"
#include "crate/writeback/historyreporter.h"
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
#include "mixer/playermanager.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wtracktableview.h"

namespace crate::library {

namespace {

/// From the start until the stems of Smart Mix playlists are read.
constexpr std::chrono::seconds kStemsStartDelay{20};

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
QString smartMixData() {
    return QStringLiteral("smartmix\n");
}
QString compatiblesData() {
    return QStringLiteral("compatibles\n");
}
QString radioData() {
    return QStringLiteral("radio\n");
}

QIcon radioIcon() {
    return namedIcon(QStringLiteral("crate-radio"), QStringLiteral("tracks"));
}

QIcon compatiblesIcon() {
    return namedIcon(QStringLiteral("crate-compatibles"), QStringLiteral("tracks"));
}

QIcon smartMixIcon() {
    return namedIcon(QStringLiteral("crate-smartmix"), QStringLiteral("tracks"));
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
const QString CrateServerFeature::kSmartMixViewName = QStringLiteral("CRATE_SMART_MIX");
const QString CrateServerFeature::kCompatiblesViewName = QStringLiteral("CRATE_COMPATIBLES");

const ConfigKey& CrateServerFeature::showSmartMixKey() {
    static const ConfigKey kKey(QStringLiteral("[CrateDecks]"), QStringLiteral("show_smart_mix"));
    return kKey;
}

const ConfigKey& CrateServerFeature::likeOnAirKey() {
    static const ConfigKey kKey(QStringLiteral("[CrateDecks]"), QStringLiteral("like_on_air"));
    return kKey;
}

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
          m_pCompatiblesModel(make_parented<recs::CompatiblesModel>(
                  dependencies.pTrackCollectionManager->internalCollection()->database(),
                  dependencies.pDbConnectionPool,
                  this)),
          m_pRadioModel(make_parented<CrateServerTrackModel>(CrateServerTrackModel::Source::Memory,
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
    // Edits of the servers' playlists (plan F7.C-PR3)
    writeback::CratePlaylistEdit::Options edit;
    edit.clientFor = [this](const QString& serverId) -> Client* {
        return m_pServers ? m_pServers->client(serverId) : nullptr;
    };
    edit.readStored = [this](const QString& serverId,
                              qint64 playlistId,
                              const std::function<void(std::optional<QStringList>)>& done) {
        const qint64 containerId = playlistContainer(serverId, playlistId);
        if (containerId <= 0) {
            done(std::nullopt);
            return;
        }
        auto* pWatcher = new QFutureWatcher<QStringList>(this);
        connect(pWatcher, &QFutureWatcherBase::finished, this, [pWatcher, done] {
            pWatcher->deleteLater();
            done(pWatcher->result());
        });
        pWatcher->setFuture(QtConcurrent::run(&m_pool,
                [pPool = m_dependencies.pDbConnectionPool, containerId] {
                    const mixxx::DbConnectionPooler pooler(pPool);
                    return loadContainerTrackUids(mixxx::DbConnectionPooled(pPool),
                            containerId,
                            0,
                            std::numeric_limits<int>::max());
                }));
    };
    edit.resync = [this](const QString& serverId,
                          qint64 playlistId,
                          const std::function<void(bool)>& done) {
        Server* pServer = server(serverId);
        if (!pServer) {
            done(false);
            return;
        }
        LibrarySync* pSync = pServer->pSync.get();
        const qint64 containerId = playlistId > 0 ? playlistContainer(serverId, playlistId) : 0;
        if (containerId <= 0) {
            // A new playlist, or one the tree does not have yet
            pSync->syncContainers();
            done(playlistId <= 0);
            return;
        }
        // Answered by the first sync that starts after the edit: one already
        // running may have read the playlist before the server changed it
        pSync->syncContainerTracks(containerId, done);
    };
    m_pPlaylistEdit = std::make_unique<writeback::CratePlaylistEdit>(std::move(edit));
    connect(m_pPlaylistEdit.get(),
            &writeback::CratePlaylistEdit::warning,
            this,
            &CrateServerFeature::showWarning);

    // A mapping's way to like the track on air; the first feature owns it
    if (!ControlObject::exists(likeOnAirKey())) {
        m_pLikeOnAir = std::make_unique<ControlPushButton>(likeOnAirKey());
        connect(m_pLikeOnAir.get(), &ControlObject::valueChanged, this, [this](double value) {
            if (value > 0) {
                toggleLikeOnAir();
            }
        });
    }
    for (CrateServerTrackModel* pModel :
            {m_pContainerModel.get(), m_pLibraryModel.get(), m_pSearchModel.get()}) {
        pModel->setSearchHandler([this, pModel](const QString& text) {
            return onSearch(pModel, text);
        });
    }
    const QList<CrateServerTrackModel*> models = trackModels();
    for (CrateServerTrackModel* pModel : models) {
        pModel->setMenuActions([this](const QList<CrateTrackRef>& tracks,
                                       const QStringList& labels,
                                       QObject* pParent) {
            return trackActions(tracks, labels, pParent);
        });
    }
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
    if (m_dependencies.pServers) {
        watchServers(m_dependencies.pServers);
    }

    // The Compatibles view follows the master deck from the start, so that
    // it is ready when the DJ opens it
    recs::Compatibles::Options compatibles;
    compatibles.clientFor = [this](const QString& serverId) -> Client* {
        return m_pServers ? m_pServers->client(serverId) : nullptr;
    };
    compatibles.trackOfFile =
            [this](const QString& location) -> std::optional<cache::TrackKey> {
        const cache::AudioCache* pCache = m_pServers ? m_pServers->existingAudioCache() : nullptr;
        return pCache ? pCache->trackOfFile(location) : std::nullopt;
    };
    compatibles.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    compatibles.prewarmEnabled = [this] {
        return m_pServers && m_pServers->preferences()->prewarmCompatibles();
    };
    compatibles.prewarm = [this](const QList<CrateTrackRef>& tracks) {
        if (m_pLoadCoordinator) {
            m_pLoadCoordinator->download(tracks, cache::Priority::Prefetch);
        }
    };
    m_pCompatibles = std::make_unique<recs::Compatibles>(
            m_pCompatiblesModel.get(), std::move(compatibles));
    if (PlayerInfo* pPlayerInfo = m_dependencies.pPlayerInfo) {
        connect(pPlayerInfo,
                &PlayerInfo::currentPlayingTrackChanged,
                m_pCompatibles.get(),
                &recs::Compatibles::setMasterTrack);
        m_pCompatibles->setMasterTrack(pPlayerInfo->getCurrentPlayingTrack());
        // The view names the deck it follows and loads into the other one
        const auto setMasterDeck = [pCompatibles = m_pCompatibles.get()](int deck) {
            pCompatibles->setMasterDeck(deck >= 0 ? PlayerManager::groupForDeck(deck) : QString());
        };
        connect(pPlayerInfo,
                &PlayerInfo::currentPlayingDeckChanged,
                m_pCompatibles.get(),
                setMasterDeck);
        setMasterDeck(pPlayerInfo->getCurrentPlayingDeck());
    }
    if (m_pLibrary) {
        connect(m_pLibrary,
                &Library::trackSelected,
                m_pCompatibles.get(),
                &recs::Compatibles::setSelectedTrack);
    }
}

CrateServerFeature::~CrateServerFeature() {
    // Its entries leave upstream's track menus with it
    crate::ui::setTrackMenuProvider(nullptr);
    disconnect(m_selectionConnection);
    m_pCompatibles.reset();
    stopLoading();
    m_servers.clear();
    m_pool.waitForDone();
}

PlanClient* CrateServerFeature::planClient(const QString& serverId) {
    Client* pClient = m_pServers ? m_pServers->client(serverId) : nullptr;
    if (!pClient) {
        return nullptr;
    }
    std::unique_ptr<PlanClient>& pPlans = m_planClients[serverId];
    if (!pPlans) {
        pPlans = std::make_unique<PlanClient>(pClient);
    }
    return pPlans.get();
}

void CrateServerFeature::correctionChanged(const QString& serverId, const QString& entityUid) {
    // Decks asks plans without profile revisions: the cache cannot tell
    // the plans made before the correction from those made after
    const auto it = m_planClients.find(serverId);
    if (it != m_planClients.end() && it->second) {
        it->second->invalidateTrack(entityUid);
    }
    if (m_pSmartMix) {
        m_pSmartMix->trackPlansChanged(serverId, entityUid);
    }
    if (m_pAssist) {
        m_pAssist->trackPlansChanged(serverId, entityUid);
    }
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
    // The server's history hears of the tracks on air (plan F7.C-PR1)
    if (PlayerInfo* pPlayerInfo = m_dependencies.pPlayerInfo) {
        writeback::HistoryReporter* pHistory = pServers->historyReporter();
        connect(pPlayerInfo,
                &PlayerInfo::currentPlayingDeckChanged,
                pHistory,
                &writeback::HistoryReporter::setPlayingDeck);
        connect(pPlayerInfo,
                &PlayerInfo::currentPlayingTrackChanged,
                pHistory,
                &writeback::HistoryReporter::setPlayingTrack);
    }
    // A Crate track's new rating goes to its server (plan F7.C-PR2)
    if (TrackCollectionManager* pCollection = m_dependencies.pTrackCollectionManager) {
        connect(pCollection->internalCollection(),
                &TrackCollection::tracksChanged,
                pServers->ratings(),
                &writeback::CrateRatings::onTracksChanged);
    }
    // Cues and corrections that another computer changed after the edits
    // here replace them (plan F8): the user hears of it
    connect(pServers->cueSync(),
            &writeback::CrateCueSync::localEditsReplaced,
            this,
            [this](const QString&, const QString&, const QString& title) {
                showWarning(tr(
                        "The cues of \u201c%1\u201d were changed on another computer. "
                        "Decks now uses those in place of the ones set here.")
                                .arg(title));
            });
    connect(pServers->corrections(),
            &writeback::CrateCorrections::localEditsReplaced,
            this,
            [this](const QString&, const QString&, const QString& title) {
                showWarning(tr(
                        "The correction of \u201c%1\u201d was changed on another "
                        "computer after you saved yours. Decks now uses that one.")
                                .arg(title));
            });
    // The first sync of a track with hot cues on both computers merges them
    connect(pServers->cueSync(),
            &writeback::CrateCueSync::cuesMerged,
            this,
            [this](const QString&,
                    const QString&,
                    const QString& title,
                    int moved,
                    int dropped) {
                QString message =
                        tr("The cues of \u201c%1\u201d on Crate and on this computer were "
                           "put together.")
                                .arg(title);
                if (moved > 0) {
                    message += QLatin1Char(' ') +
                            tr("%n cue(s) set here moved to a free hot cue.", nullptr, moved);
                }
                if (dropped > 0) {
                    message += QLatin1Char(' ') +
                            tr("%n cue(s) set here were left out: no hot cue was free.",
                                    nullptr,
                                    dropped);
                }
                showWarning(message);
            });
    // The server plans with the user's correction (plan F8.SM): the plans
    // of a track whose correction changed are asked for again
    connect(pServers->corrections(),
            &writeback::CrateCorrections::correctionChanged,
            this,
            &CrateServerFeature::correctionChanged);
    // Clearing a correction puts Crate's profile back (Diego, 2026-11-16)
    pServers->corrections()->setProfileRestorer(
            [this](const TrackPointer& pTrack, const QString& serverId, const QString& uid) {
                applyProfile(pTrack, serverId, uid);
            });
    // The correction entries, in every track menu of a Crate track (plan
    // F8.C-PR2, the WTrackMenu hook)
    crate::ui::setTrackMenuProvider([this](const TrackPointer& pTrack, QObject* pParent) {
        return correctionActions(pTrack, pParent);
    });
    // The Liked column
    writeback::CrateLikes* pLikes = pServers->likes();
    const QList<CrateServerTrackModel*> models = trackModels();
    for (CrateServerTrackModel* pModel : models) {
        pModel->setLikedOf([pLikes](const QString& serverId, const QString& entityUid) {
            return pLikes->isLiked(serverId, entityUid);
        });
        connect(pLikes,
                &writeback::CrateLikes::likesChanged,
                pModel,
                &CrateServerTrackModel::likesChanged);
    }
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
        if (ids.contains(it->first)) {
            ++it;
            continue;
        }
        if (m_pStemJobs) {
            m_pStemJobs->stopServer(it->first);
        }
        it = m_servers.erase(it);
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
        // Last, as in the GU1 mockups: the compatible tracks, the Smart Mix
        // assistant and one cache for every server
        pRoot->appendChild(tr("Compatibles"), compatiblesData())->setIcon(compatiblesIcon());
        pRoot->appendChild(tr("Radio"), radioData())->setIcon(radioIcon());
        pRoot->appendChild(tr("Smart Mix"), smartMixData())->setIcon(smartMixIcon());
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
    // The stems of Smart Mix playlists start a while after the library is
    // painted, so that starting asks the servers nothing (design §9)
    QTimer::singleShot(kStemsStartDelay, this, [this] {
        const QStringList servers = m_serverIds;
        for (const QString& id : servers) {
            whenReady(id, [this, id] {
                startStems(id);
            });
        }
    });
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
        return libraryTrackOfFile(pCollection, path);
    };
    // On a worker: the settings are read under their lock
    options.prepareTrack = [pConfig = m_pConfig](const QString& path) {
        return prepareTrackForLibrary(
                path, SyncTrackMetadataParams::readFromUserSettings(*pConfig));
    };
    options.readTrack = [pConfig = m_pConfig](const TrackPointer& pTrack) {
        readLibraryTrackFromFile(pTrack, SyncTrackMetadataParams::readFromUserSettings(*pConfig));
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
    const QList<CrateServerTrackModel*> models = trackModels();
    for (CrateServerTrackModel* pModel : models) {
        pModel->setLoadCoordinator(m_pLoadCoordinator.get());
    }

    CratePreferences* const pPreferences = m_pServers->preferences();
    CratePrefetcher::Options prefetch;
    prefetch.pCache = m_pServers->audioCache();
    prefetch.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    prefetch.pPlaylistDao = &pCollection->internalCollection()->getPlaylistDAO();
    prefetch.enabled = pPreferences->prefetchAutoDj();
    m_pPrefetcher = std::make_unique<CratePrefetcher>(std::move(prefetch));
    // Smart Mix in the decks: Auto DJ's Crate Smart Mix (plan F6.C-PR3) and
    // the assist mode (F6.C-PR4) share the plan client of each server
    const auto trackOfFile = [pCache = m_pServers->audioCache()](const QString& location) {
        return pCache->trackOfFile(location);
    };
    const auto planClientFor = [this](const QString& serverId) {
        return planClient(serverId);
    };
    const auto capabilitiesOf =
            [pServers = m_pServers](const QString& serverId) -> std::optional<Capabilities> {
        if (!pServers) {
            return std::nullopt;
        }
        Result<Capabilities> capabilities = Capabilities::fromJson(
                QJsonDocument::fromJson(pServers->lastCapabilities(serverId)));
        if (!capabilities.ok()) {
            return std::nullopt;
        }
        return capabilities.value();
    };
    const auto defaultEqOn = [pServers = m_pServers](const QStringList& deckGroups) {
        const EffectsManager* pEffects = pServers ? pServers->effectsManager() : nullptr;
        return pEffects && !deckGroups.isEmpty() &&
                std::all_of(deckGroups.cbegin(),
                        deckGroups.cend(),
                        [pEffects](const QString& group) {
                            return eqmap::isDefaultEqOn(*pEffects, group);
                        });
    };
    // The old ones first, after a stopLoading(): the adapter is Auto DJ's
    // director while it lives, and the assistant owns its controls
    m_pAssist.reset();
    m_pSmartMix.reset();
    if (!m_pSetHistory) {
        // The set goes on across reloads; its numbers come from the
        // tracks' mix profiles (plan F6b.PR1)
        SetHistory::Options history;
        history.profilesOf = [pServers = m_pServers](const QString& serverId,
                                     const QStringList& entityUids,
                                     const SetHistory::Options::ProfilesDone& done) {
            analysis::ProfileStore* pStore =
                    pServers ? pServers->profileStore(serverId) : nullptr;
            if (!pStore) {
                done({});
                return;
            }
            QObject::connect(pStore->get(entityUids),
                    &analysis::ProfileFetch::finished,
                    [done](const analysis::ProfileResults& results) {
                        QList<analysis::MixProfile> profiles;
                        for (const analysis::StoredProfile& stored : results.profiles) {
                            profiles.append(stored.profile);
                        }
                        done(profiles);
                    });
        };
        m_pSetHistory = std::make_unique<SetHistory>(std::move(history));
    }
    // Both play the decks: without their tracks there is no Smart Mix
    if (PlayerInfo* const pPlayerInfo = m_dependencies.pPlayerInfo) {
        AutoDJSmartMixAdapter::Options smartMix;
        smartMix.pConfig = m_pConfig;
        smartMix.pPlayerInfo = pPlayerInfo;
        smartMix.pPlaylistDao = &pCollection->internalCollection()->getPlaylistDAO();
        smartMix.pDbConnectionPool = m_dependencies.pDbConnectionPool;
        smartMix.pPreferences = pPreferences;
        smartMix.trackOfFile = trackOfFile;
        smartMix.planClientFor = planClientFor;
        smartMix.capabilitiesOf = capabilitiesOf;
        smartMix.defaultEqOn = defaultEqOn;
        smartMix.pSetHistory = m_pSetHistory.get();
        m_pSmartMix = std::make_unique<AutoDJSmartMixAdapter>(std::move(smartMix));
        SmartMixAssist::Options assist;
        assist.pPlayerInfo = pPlayerInfo;
        assist.pPreferences = pPreferences;
        assist.trackOfFile = trackOfFile;
        assist.planClientFor = planClientFor;
        assist.capabilitiesOf = capabilitiesOf;
        assist.defaultEqOn = defaultEqOn;
        assist.pSetHistory = m_pSetHistory.get();
        m_pAssist = std::make_unique<SmartMixAssist>(std::move(assist));
    }
    if (m_pAssistPanel) {
        m_pAssistPanel->setAssist(m_pAssist.get());
    }
    // The LRU never evicts what is loaded or queued (design §9)
    CrateSessionPins::Options pins;
    pins.pCache = m_pServers->audioCache();
    pins.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    pins.pPlaylistDao = &pCollection->internalCollection()->getPlaylistDAO();
    pins.pPlayerInfo = m_dependencies.pPlayerInfo;
    m_pSessionPins = std::make_unique<CrateSessionPins>(std::move(pins));

    // Radio and Music Paths put their tracks in Auto DJ's queue, uncached
    // ones too, as "Add to Auto DJ" does (plan F5.C-PR2, F5.C-PR3)
    recs::AutoDjRecommendationQueue::Options recommendations;
    recommendations.pPlaylistDao = &pCollection->internalCollection()->getPlaylistDAO();
    recommendations.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    recommendations.trackIds = [pCoordinator = m_pLoadCoordinator.get()](
                                       const QList<CrateQueuedTrack>& tracks) {
        return pCoordinator->trackIdsForAutoDj(tracks);
    };
    recommendations.trackOfFile = trackOfFile;
    m_pPaths.reset();
    m_pRadio.reset();
    m_pRecommendations =
            std::make_unique<recs::AutoDjRecommendationQueue>(std::move(recommendations));
    m_pRadio = std::make_unique<recs::CrateRadioSource>(m_pRecommendations.get());
    m_pPaths = std::make_unique<recs::CratePathSource>(m_pRecommendations.get());
    connect(m_pPaths.get(), &recs::CratePathSource::failed, this, [](const Error& error) {
        // TEST_GAP: a message box needs a running event loop
        QMessageBox::warning(nullptr,
                tr("Music Path"),
                error.kind == Error::Kind::Http && error.httpStatus == 422
                        ? tr("Crate could not find a path between these tracks. They may "
                             "not be analyzed yet.")
                        : tr("Crate could not make the path: %1").arg(error.message));
    });
    connect(m_pRadio.get(),
            &recs::CrateRadioSource::changed,
            this,
            &CrateServerFeature::updateRadioRows);
    connect(m_pRadio.get(),
            &recs::CrateRadioSource::stopped,
            this,
            [this](recs::CrateRadioSource::StopReason reason) {
                // TEST_GAP: a message box needs a running event loop
                if (reason == recs::CrateRadioSource::StopReason::Failed && m_pRadio &&
                        m_pRadio->tracks().isEmpty()) {
                    QMessageBox::warning(nullptr,
                            tr("Radio"),
                            tr("Crate could not start a radio from this. Its tracks may not "
                               "be analyzed yet."));
                }
            });

    // The stems of Smart Mix playlists (plan ST5.PR2)
    StemSync::Options stemSync;
    stemSync.clientFor = [pServers = m_pServers](const QString& serverId) {
        return pServers ? pServers->client(serverId) : nullptr;
    };
    stemSync.mayUpload = [this](const QString& serverId) {
        return m_stemUploads.contains(serverId);
    };
    // The file the stems are made from: the one in the cache
    stemSync.currentRevision = [pCache = m_pServers->audioCache()](
                                       const QString& serverId, const QString& entityUid) {
        return pCache->cachedSourceRevision(serverId, entityUid).value_or(QString());
    };
    m_pStemSync = std::make_unique<StemSync>(std::move(stemSync));
    StemJobs::Options stems;
    stems.pCache = m_pServers->audioCache();
    stems.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    stems.pSync = m_pStemSync.get();
    stems.clientFor = [pServers = m_pServers](const QString& serverId) {
        return pServers ? pServers->client(serverId) : nullptr;
    };
    stems.readPlaylist =
            [this](const QString& serverId,
                    qint64 playlistId,
                    const std::function<void(
                            std::optional<QList<CrateTrackRef>>)>& done) {
                readSmartMixPlaylist(serverId, playlistId, done);
            };
    stems.confirmDownload = [this](const QString& serverId,
                                    qint64 playlistId,
                                    int tracks,
                                    qint64 bytes,
                                    std::function<void(bool)> answer) {
        confirmSmartMixDownload(serverId, playlistId, tracks, bytes, std::move(answer));
    };
    stems.download = [this](const QList<CrateTrackRef>& tracks, const QString& reason) {
        if (m_pLoadCoordinator) {
            m_pLoadCoordinator->download(tracks, cache::Priority::OfflineSet, reason);
        }
    };
#ifdef __CRATE_INFERENCE__
    // Local separation: the htdemucs model downloads on first use, after a
    // question with its size
    ModelStore::Dependencies models;
    models.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    m_pModelNetwork = std::make_unique<QNetworkAccessManager>();
    models.pNetworkAccessManager = m_pModelNetwork.get();
    models.dataDirectory = m_pConfig->getSettingsPath();
    m_pModels = std::make_unique<ModelStore>(ModelStore::builtInManifest(), models);
    AppStemSeparator::Options local;
    local.pModels = m_pModels.get();
    local.useGpu = [pPreferences] {
        return pPreferences->stemsOnGpu();
    };
    local.confirmDownload = [this](qint64 bytes, std::function<void(bool)> answer) {
        confirmModelDownload(bytes, std::move(answer));
    };
    m_pLocalSeparator = std::make_unique<AppStemSeparator>(std::move(local));
    stems.pSeparator = m_pLocalSeparator.get();
#else
    // Without ONNX Runtime, only the server's stems
    stems.pSeparator = nullptr;
#endif
    stems.separateWhilePlaying = [pPreferences] {
        return pPreferences->separateWhilePlaying();
    };
    m_pStemJobs = std::make_unique<StemJobs>(std::move(stems));
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
    m_pPaths.reset();
    m_pRadio.reset();
    m_pRecommendations.reset();
    const QList<CrateServerTrackModel*> models = trackModels();
    for (CrateServerTrackModel* pModel : models) {
        pModel->setLoadCoordinator(nullptr);
    }
    m_pStemJobs.reset();
    m_pStemSync.reset();
    m_pLocalSeparator.reset();
    m_pModels.reset();
    m_pModelNetwork.reset();
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
    auto* pView = new CacheStatusView(m_pCacheStatus, pLibraryWidget);
    pLibraryWidget->registerView(kCacheViewName, pView);
    // The Smart Mix assistant (plan F6.C-PR4), which the toolbar's Smart Mix
    // button shows and hides
    auto* pPanel = new AssistPanel(pLibraryWidget);
    pPanel->setAssist(m_pAssist.get());
    pLibraryWidget->registerView(kSmartMixViewName, pPanel);
    m_pAssistPanel = pPanel;
    // The Compatibles view (plan F5.C-PR1): upstream's track table, as Auto
    // DJ's view has, over the compatibles model
    QWidget* pTable = nullptr;
    if (m_pLibrary) {
        auto* pTrackTable = new WTrackTableView(pLibraryWidget,
                m_pConfig,
                m_pLibrary,
                pLibraryWidget->getTrackTableBackgroundColorOpacity());
        if (pKeyboard) {
            pTrackTable->installEventFilter(pKeyboard);
        }
        connect(pTrackTable, &WTrackTableView::loadTrack, this, &LibraryFeature::loadTrack);
        connect(pTrackTable,
                &WTrackTableView::loadTrackToPlayer,
                this,
                &LibraryFeature::loadTrackToPlayer);
        connect(m_pLibrary,
                &Library::setTrackTableFont,
                pTrackTable,
                &WTrackTableView::setTrackTableFont);
        connect(m_pLibrary,
                &Library::setTrackTableRowHeight,
                pTrackTable,
                &WTrackTableView::setTrackTableRowHeight);
        connect(m_pLibrary,
                &Library::setSelectedClick,
                pTrackTable,
                &WTrackTableView::setSelectedClick);
        pTrackTable->loadTrackModel(m_pCompatiblesModel.get());
        pTable = pTrackTable;
    } else {
        // Without the library (tests): a plain table
        auto* pPlainTable = new QTableView(pLibraryWidget);
        pPlainTable->setModel(m_pCompatiblesModel.get());
        pTable = pPlainTable;
    }
    // The wheel names the keys in the notation of [Library],key_notation,
    // which the library creates
    if (!m_pWheelModel && ControlObject::exists(mixxx::library::prefs::kKeyNotationConfigKey)) {
        m_pWheelModel = std::make_unique<widgets::CamelotWheelModel>(
                widgets::InstrumentStyle::installed().opacityContext);
    }
    auto* pCompatibles = new recs::CompatiblesView(
            m_pCompatibles.get(), pTable, m_pWheelModel.get(), pLibraryWidget);
    pLibraryWidget->registerView(kCompatiblesViewName, pCompatibles);
    m_pCompatiblesView = pCompatibles;
    // "Plan transition": the assistant, for the edge from the master deck to
    // the track the view loaded, as the toolbar's Smart Mix button opens it
    connect(pCompatibles, &recs::CompatiblesView::planTransitionRequested, this, [this] {
        showSmartMix(true);
    });
    m_pLibraryWidget = pLibraryWidget;
    connect(pLibraryWidget,
            &QStackedWidget::currentChanged,
            this,
            &CrateServerFeature::onLibraryViewChanged);
    // A skin reload binds a new library widget: the old control goes
    delete m_pShowSmartMix;
    m_pShowSmartMix = nullptr;
    if (ControlObject::exists(showSmartMixKey())) {
        m_pShowSmartMix = new ControlProxy(showSmartMixKey(), this);
        m_pShowSmartMix->connectValueChanged(this, &CrateServerFeature::onShowSmartMix);
    }
}

void CrateServerFeature::showCompatibles() {
    endSearch();
    m_pCompatiblesModel->setBpmPrecision(m_pConfig->getValue(
            mixxx::library::prefs::kBpmColumnPrecisionConfigKey,
            BaseTrackTableModel::kBpmColumnPrecisionDefault));
    emit switchToView(kCompatiblesViewName);
    emit disableSearch();
    emit enableCoverArtDisplay(false);
}

QList<CrateServerTrackModel*> CrateServerFeature::trackModels() const {
    return {m_pContainerModel.get(),
            m_pLibraryModel.get(),
            m_pSearchModel.get(),
            m_pCompatiblesModel.get(),
            m_pRadioModel.get()};
}

QList<QAction*> CrateServerFeature::trackActions(
        const QList<CrateTrackRef>& tracks, const QStringList& labels, QObject* pParent) {
    QList<QAction*> actions;
    if (tracks.isEmpty()) {
        return actions;
    }
    // Likes on Crate (plan F7.C-PR2)
    if (m_pServers) {
        QList<cache::TrackKey> keys;
        for (const CrateTrackRef& track : tracks) {
            keys.append(cache::TrackKey{track.serverId, track.entityUid});
        }
        writeback::CrateLikes* pLikes = m_pServers->likes();
        const bool liked = pLikes->allLiked(keys);
        auto* pLike = new QAction(liked
                        //: Track menu: stops liking the selected tracks on Crate
                        ? tr("Unlike")
                        //: Track menu: likes the selected tracks on Crate
                        : tr("Like"),
                pParent);
        connect(pLike, &QAction::triggered, this, [pLikes = QPointer(pLikes), keys, liked] {
            if (pLikes) {
                pLikes->setLiked(keys, !liked);
            }
        });
        actions.append(pLike);
    }
    if (!m_pRadio) {
        return actions;
    }
    const CrateTrackRef& first = tracks.first();
    //: Starts Crate's Radio, which fills Auto DJ's queue, from the selected track
    auto* pRadio = new QAction(tr("Start radio from this track"), pParent);
    pRadio->setEnabled(tracks.size() == 1);
    connect(pRadio, &QAction::triggered, this, [this, first, label = labels.value(0)] {
        startRadio(first.serverId, recs::RadioSeed::track(first.entityUid, label));
    });
    actions.append(pRadio);

    // A Music Path between two tracks of a server: the two selected, or the
    // one on air and the one selected
    std::optional<std::pair<CrateTrackRef, CrateTrackRef>> ends;
    QStringList endLabels;
    if (tracks.size() == 2 && tracks[0].serverId == tracks[1].serverId &&
            tracks[0].entityUid.compare(tracks[1].entityUid, Qt::CaseInsensitive) != 0) {
        ends = std::pair(tracks[0], tracks[1]);
        endLabels = labels;
    } else if (tracks.size() == 1) {
        if (const auto onAir = crateTrackOnAir(); onAir && onAir->serverId == first.serverId &&
                onAir->entityUid.compare(first.entityUid, Qt::CaseInsensitive) != 0) {
            CrateTrackRef from;
            from.serverId = onAir->serverId;
            from.entityUid = onAir->entityUid;
            ends = std::pair(from, first);
            //: The track of the deck on air, as one end of a Music Path
            endLabels = QStringList{tr("the track on air"), labels.value(0)};
        }
    }
    //: Asks Crate for a path of tracks between two tracks, into Auto DJ or a playlist
    auto* pPath = new QAction(tr("Music Path..."), pParent);
    pPath->setEnabled(ends.has_value() && m_pPaths);
    if (ends) {
        connect(pPath, &QAction::triggered, this, [this, ends, endLabels] {
            askForPath(ends->first, endLabels.value(0), ends->second, endLabels.value(1));
        });
    }
    actions.append(pPath);

    // The DJ's say on the radio's own tracks
    const bool radioTracks = std::all_of(tracks.cbegin(), tracks.cend(), [this](const auto& track) {
        return m_pRadio->hasTrack(track.serverId, track.entityUid);
    });
    if (radioTracks) {
        for (const auto feedback :
                {recs::CrateRadioSource::Feedback::Like,
                        recs::CrateRadioSource::Feedback::Dislike}) {
            auto* pFeedback = new QAction(feedback == recs::CrateRadioSource::Feedback::Like
                            //: Radio feedback: steers the radio towards the selected track
                            ? tr("More like this")
                            //: Radio feedback: steers the radio away from the selected track
                            : tr("Less like this"),
                    pParent);
            connect(pFeedback, &QAction::triggered, this, [this, tracks, feedback] {
                if (!m_pRadio) {
                    return;
                }
                for (const CrateTrackRef& track : tracks) {
                    m_pRadio->sendFeedback(track.entityUid, feedback);
                }
            });
            actions.append(pFeedback);
        }
    }
    return actions;
}

QList<QAction*> CrateServerFeature::correctionActions(
        const TrackPointer& pTrack, QObject* pParent) {
    // BPM, grid and key corrections (plan F8.C-PR2): in the track's menu,
    // wherever it opens, so that a correction made during a set never
    // interrupts it
    QList<QAction*> actions;
    cache::AudioCache* const pCache = m_pServers ? m_pServers->existingAudioCache() : nullptr;
    const std::optional<cache::TrackKey> track =
            pCache && pTrack ? pCache->trackOfFile(pTrack->getLocation()) : std::nullopt;
    if (!track) {
        return actions;
    }
    writeback::CrateCorrections* const pCorrections = m_pServers->corrections();
    if (pCorrections->offer(track->serverId, track->entityUid)) {
        //: Track menu: sends the BPM, beat grid or key the user corrected in
        //: Decks to Crate, so that their other computers use it too
        auto* pSave = new QAction(tr("Save correction to Crate"), pParent);
        connect(pSave,
                &QAction::triggered,
                this,
                [pCorrections = QPointer(pCorrections), track = *track] {
                    if (pCorrections) {
                        pCorrections->save(track.serverId, track.entityUid);
                    }
                });
        actions.append(pSave);
    }
    if (pCorrections->hasCorrection(track->serverId, track->entityUid)) {
        //: Track menu: removes the user's BPM, grid and key correction of the
        //: track from Crate; Crate's analysis applies again on their computers
        auto* pClear = new QAction(tr("Clear correction on Crate"), pParent);
        connect(pClear,
                &QAction::triggered,
                this,
                [pCorrections = QPointer(pCorrections), track = *track] {
                    if (pCorrections) {
                        pCorrections->clear(track.serverId, track.entityUid);
                    }
                });
        actions.append(pClear);
    }
    return actions;
}

void CrateServerFeature::startRadio(const QString& serverId, const recs::RadioSeed& seed) {
    Client* pClient = m_pServers ? m_pServers->client(serverId) : nullptr;
    if (!m_pRadio || !pClient) {
        return;
    }
    m_pRadio->start(pClient, serverId, seed);
}

qint64 CrateServerFeature::playlistContainer(const QString& serverId, qint64 playlistId) const {
    const QString remoteId = QString::number(playlistId);
    for (auto it = m_containers.cbegin(); it != m_containers.cend(); ++it) {
        if (it->serverId == serverId && it->kind == QLatin1String("playlist") &&
                it->remoteId == remoteId) {
            return it.key();
        }
    }
    return 0;
}

qint64 CrateServerFeature::playlistIdOf(qint64 containerId) const {
    const auto it = m_containers.constFind(containerId);
    if (it == m_containers.cend() || it->kind != QLatin1String("playlist")) {
        return 0;
    }
    bool ok = false;
    const qint64 playlistId = it->remoteId.toLongLong(&ok);
    return ok ? playlistId : 0;
}

void CrateServerFeature::editPlaylistIn(const QString& serverId, qint64 containerId) {
    const qint64 playlistId = playlistIdOf(containerId);
    if (playlistId <= 0) {
        m_pContainerModel->setPlaylistEditor(std::nullopt);
        return;
    }
    CrateServerTrackModel::PlaylistEditor editor;
    editor.remove = [this, serverId, playlistId](const QList<std::pair<int, QString>>& rows) {
        QList<writeback::CratePlaylistEdit::Row> edits;
        for (const auto& [index, entityUid] : rows) {
            edits.append({index, entityUid});
        }
        m_pPlaylistEdit->remove(serverId, playlistId, edits);
    };
    editor.reorder = [this, serverId, playlistId](const QStringList& entityUids) {
        m_pPlaylistEdit->reorder(serverId, playlistId, entityUids);
    };
    editor.add = [this, serverId, playlistId](const QStringList& locations) {
        addToPlaylist(serverId, playlistId, locations);
    };
    m_pContainerModel->setPlaylistEditor(std::move(editor));
}

void CrateServerFeature::addToPlaylist(
        const QString& serverId, qint64 playlistId, const QStringList& locations) {
    const cache::AudioCache* pCache = m_pServers ? m_pServers->existingAudioCache() : nullptr;
    const auto tracks = writeback::CratePlaylistEdit::crateTracksOf(serverId,
            locations,
            [pCache](const QString& location) -> std::optional<cache::TrackKey> {
                return pCache ? pCache->trackOfFile(location) : std::nullopt;
            });
    const int rejected = tracks.rejected;
    m_pPlaylistEdit->add(serverId,
            playlistId,
            tracks.entityUids,
            [this, rejected](const writeback::CratePlaylistEdit::Outcome& outcome) {
                if (outcome.ok && rejected > 0) {
                    showWarning(tr(
                            "%n track(s) left out: only Crate tracks of this server "
                            "can go into its playlists.",
                            nullptr,
                            rejected));
                }
            });
}

void CrateServerFeature::exportPlaylistToCrate(const QString& serverId, int playlistId) {
    TrackCollectionManager* pCollection = m_dependencies.pTrackCollectionManager;
    if (!pCollection) {
        return;
    }
    // A few queries of upstream's tables, which live on this thread
    PlaylistDAO& playlists = pCollection->internalCollection()->getPlaylistDAO();
    const TrackDAO& tracks = pCollection->internalCollection()->getTrackDAO();
    QStringList locations;
    const QList<TrackId> trackIds = playlists.getTrackIdsInPlaylistOrder(playlistId);
    for (const TrackId& trackId : trackIds) {
        locations.append(tracks.getTrackLocation(trackId));
    }
    const cache::AudioCache* pCache = m_pServers ? m_pServers->existingAudioCache() : nullptr;
    const auto crateTracks = writeback::CratePlaylistEdit::crateTracksOf(serverId,
            locations,
            [pCache](const QString& location) -> std::optional<cache::TrackKey> {
                return pCache ? pCache->trackOfFile(location) : std::nullopt;
            });
    m_pPlaylistEdit->exportTracks(serverId,
            playlists.getPlaylistName(playlistId),
            crateTracks.entityUids,
            crateTracks.rejected,
            [this](const writeback::CratePlaylistEdit::Outcome& outcome) {
                if (outcome.ok && outcome.rejected > 0) {
                    showWarning(tr(
                            "The playlist is on Crate without %n track(s) that are "
                            "not Crate tracks of this server.",
                            nullptr,
                            outcome.rejected));
                }
            });
}

void CrateServerFeature::showWarning(const QString& message) {
    auto* pBox = new QMessageBox(QMessageBox::Warning, tr("Crate"), message, QMessageBox::Ok);
    pBox->setAttribute(Qt::WA_DeleteOnClose);
    pBox->open();
}

bool CrateServerFeature::dropAcceptChild(
        const QModelIndex& index, const QList<QUrl>& urls, QObject* pSource) {
    Q_UNUSED(pSource);
    const Node node = nodeOf(index);
    const qint64 playlistId =
            node.kind == Node::Kind::Container ? playlistIdOf(node.containerId) : 0;
    if (playlistId <= 0) {
        return false;
    }
    QStringList locations;
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            locations.append(url.toLocalFile());
        }
    }
    addToPlaylist(node.serverId, playlistId, locations);
    return true;
}

bool CrateServerFeature::dragMoveAcceptChild(const QModelIndex& index, const QList<QUrl>& urls) {
    const Node node = nodeOf(index);
    if (node.kind != Node::Kind::Container || playlistIdOf(node.containerId) <= 0) {
        return false;
    }
    const cache::AudioCache* pCache = m_pServers ? m_pServers->existingAudioCache() : nullptr;
    return pCache && std::any_of(urls.cbegin(), urls.cend(), [&](const QUrl& url) {
        const auto track = pCache->trackOfFile(url.toLocalFile());
        return track && track->serverId == node.serverId;
    });
}

bool CrateServerFeature::toggleLikeOnAir() {
    const std::optional<cache::TrackKey> onAir = crateTrackOnAir();
    if (!onAir || !m_pServers) {
        return false;
    }
    m_pServers->likes()->toggle({*onAir});
    return true;
}

std::optional<cache::TrackKey> CrateServerFeature::crateTrackOnAir() const {
    PlayerInfo* pPlayerInfo = m_dependencies.pPlayerInfo;
    const cache::AudioCache* pCache = m_pServers ? m_pServers->existingAudioCache() : nullptr;
    if (!pPlayerInfo || !pCache) {
        return std::nullopt;
    }
    const TrackPointer pTrack = pPlayerInfo->getCurrentPlayingTrack();
    return pTrack ? pCache->trackOfFile(pTrack->getLocation()) : std::nullopt;
}

void CrateServerFeature::askForPath(const CrateTrackRef& from,
        const QString& fromLabel,
        const CrateTrackRef& to,
        const QString& toLabel) {
    // TEST_GAP: the dialog needs a running event loop; CratePathSourceTest
    // covers what it asks for
    Client* pClient = m_pServers ? m_pServers->client(from.serverId) : nullptr;
    if (!m_pPaths || !pClient) {
        return;
    }
    recs::CratePathDialog dialog(fromLabel, toLabel);
    if (dialog.exec() != QDialog::Accepted || !m_pPaths) {
        return;
    }
    recs::PathRequest request;
    request.serverId = from.serverId;
    request.fromEntityUid = from.entityUid;
    request.toEntityUid = to.entityUid;
    request.steps = dialog.steps();
    request.destination = dialog.destination();
    m_pPaths->request(pClient, request);
}

void CrateServerFeature::showRadio() {
    endSearch();
    m_pRadioModel->setBpmPrecision(m_pConfig->getValue(
            mixxx::library::prefs::kBpmColumnPrecisionConfigKey,
            BaseTrackTableModel::kBpmColumnPrecisionDefault));
    updateRadioRows();
    emit showTrackModel(m_pRadioModel);
    emit enableCoverArtDisplay(false);
}

void CrateServerFeature::updateRadioRows() {
    if (!m_pRadio) {
        m_pRadioModel->setRows(QString(), QString(), {});
        return;
    }
    QVector<CrateServerTrackModel::Row> rows;
    const QList<recs::RecommendedTrack>& tracks = m_pRadio->tracks();
    rows.reserve(tracks.size());
    for (const recs::RecommendedTrack& track : tracks) {
        CrateServerTrackModel::Row row;
        row.position = static_cast<int>(rows.size()) + 1;
        row.serverId = m_pRadio->serverId();
        row.entityUid = track.entityUid;
        row.title = track.title;
        row.artist = track.artist;
        row.album = track.album;
        row.durationMs = track.durationMs;
        row.format = track.format;
        rows.append(row);
    }
    m_pRadioModel->setRows(m_pRadio->serverId(), m_pRadio->label(), rows);
}

void CrateServerFeature::watchSelection() {
    // TEST_GAP: the library's own track table only exists in the
    // application; the tests feed Compatibles::setSelectedCrateTrack()
    if (!m_pLibraryWidget || !m_pCompatibles) {
        return;
    }
    auto* pView = dynamic_cast<QAbstractItemView*>(m_pLibraryWidget->getActiveView());
    auto* pModel = pView ? dynamic_cast<CrateServerTrackModel*>(pView->model()) : nullptr;
    if (!pModel || !pView->selectionModel() ||
            pModel == static_cast<CrateServerTrackModel*>(m_pCompatiblesModel.get())) {
        return;
    }
    disconnect(m_selectionConnection);
    m_selectionConnection = connect(pView->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this, pModel = QPointer<CrateServerTrackModel>(pModel)](const QModelIndex& current) {
                if (!pModel || !m_pCompatibles || !current.isValid()) {
                    return;
                }
                const CrateTrackRef track = pModel->trackRef(current.row());
                if (!track.entityUid.isEmpty()) {
                    m_pCompatibles->setSelectedCrateTrack(
                            cache::TrackKey{track.serverId, track.entityUid});
                }
            });
}

TreeItem* CrateServerFeature::smartMixItem() const {
    for (TreeItem* pChild : m_pSidebarModel->getRootItem()->children()) {
        if (pChild->getData().toString() == smartMixData()) {
            return pChild;
        }
    }
    return nullptr;
}

void CrateServerFeature::showSmartMix(bool fromToolbar) {
    const QModelIndex current =
            m_pSidebarWidget ? m_pSidebarWidget->currentIndex() : QModelIndex();
    // The sidebar's index of a feature's root points at the sidebar model,
    // the others at their TreeItem
    const auto* pCurrentItem = current.isValid() && current.internalPointer() != current.model()
            ? static_cast<const TreeItem*>(current.internalPointer())
            : nullptr;
    const bool onSmartMix = pCurrentItem && pCurrentItem->feature() == this &&
            pCurrentItem->getData().toString() == smartMixData();
    if (!fromToolbar) {
        m_beforeSmartMix = QPersistentModelIndex();
    } else if (!onSmartMix) {
        m_beforeSmartMix = QPersistentModelIndex(current);
    }
    if (fromToolbar && !onSmartMix) {
        if (TreeItem* pItem = smartMixItem()) {
            emit featureSelect(this, indexOf(pItem), false);
        }
    }
    endSearch();
    emit switchToView(kSmartMixViewName);
    emit disableSearch();
    emit enableCoverArtDisplay(false);
}

void CrateServerFeature::onShowSmartMix(double value) {
    if (!m_pAssistPanel || !m_pLibraryWidget) {
        return;
    }
    const bool shown = m_pLibraryWidget->currentWidget() == m_pAssistPanel;
    if (value > 0.0 && !shown) {
        showSmartMix(true);
    } else if (value <= 0.0 && shown) {
        // Back where the DJ was before the toolbar's button
        auto* pModel = m_pSidebarWidget ? qobject_cast<SidebarModel*>(m_pSidebarWidget->model())
                                        : nullptr;
        const QModelIndex before = m_beforeSmartMix;
        m_beforeSmartMix = QPersistentModelIndex();
        if (pModel && before.isValid()) {
            m_pSidebarWidget->selectIndex(before, false);
            pModel->clicked(before);
        } else {
            activate();
        }
    }
}

void CrateServerFeature::onLibraryViewChanged() {
    if (!m_pShowSmartMix || !m_pLibraryWidget) {
        return;
    }
    const bool shown = m_pAssistPanel && m_pLibraryWidget->currentWidget() == m_pAssistPanel;
    if ((m_pShowSmartMix->get() > 0.0) != shown) {
        m_pShowSmartMix->set(shown ? 1.0 : 0.0);
    }
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
    // The user's own cues and corrections, read with the other tracks taken
    // now, go over the profile's (plan F8): the synced cue set rules every
    // cue type, and the profile never replaces a grid locked by a correction
    // or the user's key
    writeback::CrateCueSync* const pCueSync = m_pServers->cueSync();
    writeback::CrateCorrections* const pCorrections = m_pServers->corrections();
    pCueSync->attach(pTrack, serverId, uid);
    pCorrections->attach(pTrack, serverId, uid);
    applyProfile(pTrack, serverId, uid);
}

void CrateServerFeature::applyProfile(
        const TrackPointer& pTrack, const QString& serverId, const QString& entityUid) {
    analysis::ProfileStore* const pStore =
            m_pServers ? m_pServers->profileStore(serverId) : nullptr;
    if (!pStore || !pTrack) {
        return;
    }
    const QString uid = entityUid.toLower();
    writeback::CrateCueSync* const pCueSync = m_pServers->cueSync();
    writeback::CrateCorrections* const pCorrections = m_pServers->corrections();
    // What the Crate preferences say when the track is taken
    const analysis::Policy policy = m_pServers->preferences()->analysisPolicy();
    analysis::ProfileFetch* pFetch = pStore->get({uid});
    connect(pFetch,
            &analysis::ProfileFetch::finished,
            this,
            [pTrack,
                    pStore = QPointer<analysis::ProfileStore>(pStore),
                    pCueSync = QPointer<writeback::CrateCueSync>(pCueSync),
                    pCorrections = QPointer<writeback::CrateCorrections>(pCorrections),
                    uid,
                    policy](const analysis::ProfileResults& results) {
                const auto it = results.profiles.constFind(uid);
                if (it == results.profiles.cend()) {
                    return;
                }
                const analysis::ImportOutcome outcome = analysis::AnalysisImporter::apply(
                        pTrack, it->profile, policy, it->appliedCues);
                if (outcome.appliedCues && pStore) {
                    pStore->recordAppliedCues(uid, *outcome.appliedCues);
                }
                // What the profile changed is not the user's edit
                if (pCueSync) {
                    pCueSync->absorbAutomaticChanges(pTrack);
                }
                if (pCorrections) {
                    pCorrections->absorbAutomaticChanges(pTrack);
                }
            });
}

void CrateServerFeature::readOfflineSet(const QString& serverId,
        qint64 containerId,
        std::function<void(const OfflineSet&)> ready) {
    readContainerTracks(serverId,
            containerId,
            [this, ready = std::move(ready)](const QList<CrateTrackRef>& tracks) {
                OfflineSet set;
                QSet<QString> seen;
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
}

void CrateServerFeature::readContainerTracks(const QString& serverId,
        qint64 containerId,
        std::function<void(const QList<CrateTrackRef>&)> ready) {
    auto* pWatcher = new QFutureWatcher<QList<CrateTrackRef>>(this);
    connect(pWatcher,
            &QFutureWatcherBase::finished,
            this,
            [pWatcher, ready = std::move(ready)] {
                pWatcher->deleteLater();
                ready(pWatcher->result());
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

void CrateServerFeature::readSmartMixPlaylist(const QString& serverId,
        qint64 playlistId,
        const std::function<void(std::optional<QList<CrateTrackRef>>)>& done) {
    // A Smart Mix playlist is a playlist of the server's tree (STEM-D2)
    const QString remoteId = QString::number(playlistId);
    const StoredContainer* pContainer = nullptr;
    for (const StoredContainer& container : std::as_const(m_containers)) {
        if (container.serverId == serverId && container.kind == QLatin1String("playlist") &&
                container.remoteId == remoteId) {
            pContainer = &container;
            break;
        }
    }
    Server* pServer = server(serverId);
    if (!pContainer || !pServer) {
        done(std::nullopt);
        return;
    }
    const qint64 containerId = pContainer->id;
    if (!pContainer->needsSync()) {
        readContainerTracks(serverId, containerId, [done](const QList<CrateTrackRef>& tracks) {
            done(tracks);
        });
        return;
    }
    // Synced first; read once the sync ends
    LibrarySync* pSync = pServer->pSync.get();
    auto pConnections = std::make_shared<QList<QMetaObject::Connection>>();
    const auto disconnectAll = [pConnections] {
        for (const QMetaObject::Connection& connection : std::as_const(*pConnections)) {
            QObject::disconnect(connection);
        }
        pConnections->clear();
    };
    pConnections->append(connect(pSync,
            &LibrarySync::containerUpdated,
            this,
            [this, serverId, containerId, done, disconnectAll](qint64 updated) {
                if (updated != containerId) {
                    return;
                }
                disconnectAll();
                readContainerTracks(serverId,
                        containerId,
                        [done](const QList<CrateTrackRef>& tracks) {
                            done(tracks);
                        });
            }));
    pConnections->append(connect(pSync,
            &LibrarySync::syncFailed,
            this,
            [containerId, done, disconnectAll](qint64 failed) {
                if (failed != containerId) {
                    return;
                }
                disconnectAll();
                done(std::nullopt);
            }));
    whenReady(serverId, [pSync, containerId] {
        pSync->syncContainerTracks(containerId);
    });
}

void CrateServerFeature::confirmSmartMixDownload(const QString& serverId,
        qint64 playlistId,
        int tracks,
        qint64 bytes,
        std::function<void(bool)> answer) {
    QString name;
    for (const StoredContainer& container : std::as_const(m_containers)) {
        if (container.serverId == serverId && container.kind == QLatin1String("playlist") &&
                container.remoteId == QString::number(playlistId)) {
            name = container.name;
        }
    }
    // Not modal: it may come up at any time, also during a set
    auto* pBox = new QMessageBox(QMessageBox::Question,
            tr("Smart Mix playlist"),
            //: A playlist marked for Smart Mix on the server is downloaded to
            //: this computer, as an offline set, before its stems are made
            tr("Download %n track(s) of the Smart Mix playlist \u201c%1\u201d, %2, and keep "
               "them on this computer?",
                    nullptr,
                    tracks)
                    .arg(name, QLocale().formattedDataSize(bytes)),
            QMessageBox::Yes | QMessageBox::No);
    pBox->setAttribute(Qt::WA_DeleteOnClose);
    connect(pBox, &QMessageBox::finished, this, [answer = std::move(answer)](int result) {
        answer(result == QMessageBox::Yes);
    });
    pBox->open();
}

void CrateServerFeature::confirmModelDownload(qint64 bytes, std::function<void(bool)> answer) {
    auto* pBox = new QMessageBox(QMessageBox::Question,
            tr("Stems"),
            //: Asked the first time Decks separates a track into stems
            //: on this computer
            tr("Separating stems on this computer needs the separation model, %1. "
               "Download it now?")
                    .arg(QLocale().formattedDataSize(bytes)),
            QMessageBox::Yes | QMessageBox::No);
    pBox->setAttribute(Qt::WA_DeleteOnClose);
    connect(pBox, &QMessageBox::finished, this, [answer = std::move(answer)](int result) {
        answer(result == QMessageBox::Yes);
    });
    pBox->open();
}

void CrateServerFeature::startStems(const QString& serverId) {
    Client* pClient = m_pServers ? m_pServers->client(serverId) : nullptr;
    if (!pClient || !m_pStemJobs) {
        return;
    }
    // Signed in: stem_upload depends on this account's permissions
    Reply* pReply = pClient->get(QStringLiteral("/api/capabilities"));
    connect(pReply, &Reply::finished, this, [this, serverId](const JsonResult& result) {
        if (!result.ok() || !m_pStemJobs) {
            return;
        }
        const Result<Capabilities> capabilities = Capabilities::fromJson(result.value());
        if (!capabilities.ok() || !capabilities.value().has(DecksFeature::Stems)) {
            return;
        }
        if (capabilities.value().has(DecksFeature::StemUpload)) {
            m_stemUploads.insert(serverId);
        } else {
            m_stemUploads.remove(serverId);
        }
        m_pStemJobs->startServer(serverId);
    });
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
    // The Likes container may have changed with the rest
    if (m_pServers) {
        m_pServers->likes()->reload(serverId);
    }
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
        // The server's items are the feature's: the compatibles', the
        // assistant's and the cache's stay last
        items.append({tr("Compatibles"), compatiblesData(), compatiblesIcon(), {}});
        items.append({tr("Radio"), radioData(), radioIcon(), {}});
        items.append({tr("Smart Mix"), smartMixData(), smartMixIcon(), {}});
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
    } else if (parts[0] == QLatin1String("smartmix")) {
        node.kind = Node::Kind::SmartMix;
    } else if (parts[0] == QLatin1String("compatibles")) {
        node.kind = Node::Kind::Compatibles;
    } else if (parts[0] == QLatin1String("radio")) {
        node.kind = Node::Kind::Radio;
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
    case Node::Kind::SmartMix:
        showSmartMix(false);
        return;
    case Node::Kind::Compatibles:
        showCompatibles();
        return;
    case Node::Kind::Radio:
        showRadio();
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
    QTimer::singleShot(0, this, &CrateServerFeature::watchSelection);
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
    editPlaylistIn(serverId, containerId);
    emit showTrackModel(m_pContainerModel);
    QTimer::singleShot(0, this, &CrateServerFeature::watchSelection);
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
        QTimer::singleShot(0, this, &CrateServerFeature::watchSelection);
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
    if (node.kind == Node::Kind::Radio) {
        QMenu menu;
        //: Stops Crate's Radio: no more tracks join Auto DJ's queue
        QAction* pStop = menu.addAction(tr("Stop radio"));
        pStop->setEnabled(m_pRadio && m_pRadio->isRunning());
        if (menu.exec(globalPos) == pStop && m_pRadio) {
            m_pRadio->stop();
        }
        return;
    }
    Server* pServer = server(node.serverId);
    if (!pServer) {
        return;
    }
    QMenu menu;
    QAction* pRefresh = menu.addAction(tr("Refresh"));
    QAction* pExport = nullptr;
    if (node.kind == Node::Kind::Section && node.section == static_cast<int>(Section::Playlists)) {
        //: Creates a playlist on Crate from a playlist of Decks' own library
        pExport = menu.addAction(tr("Export a Decks playlist to Crate..."));
    }
    QAction* pOffline = nullptr;
    QAction* pRadio = nullptr;
    std::optional<recs::RadioSeed> radioSeed;
    if (node.kind == Node::Kind::Container && m_pLoadCoordinator) {
        //: Downloads every track of a playlist or crate for playing without network
        pOffline = menu.addAction(tr("Download for offline use"));
        const auto container = m_containers.constFind(node.containerId);
        if (container != m_containers.constEnd() && m_pRadio) {
            radioSeed = recs::RadioSeed::container(
                    container->kind, container->remoteId, container->name);
        }
        if (radioSeed) {
            //: Starts Crate's Radio, which fills Auto DJ's queue, from a playlist or crate
            pRadio = menu.addAction(tr("Start radio"));
        }
    }
    QAction* const pChosen = menu.exec(globalPos);
    if (pChosen && pChosen == pExport && m_dependencies.pTrackCollectionManager) {
        const QList<QPair<int, QString>> playlists =
                m_dependencies.pTrackCollectionManager->internalCollection()
                        ->getPlaylistDAO()
                        .getPlaylists(PlaylistDAO::PLHT_NOT_HIDDEN);
        QStringList names;
        for (const auto& playlist : playlists) {
            names.append(playlist.second);
        }
        bool ok = false;
        const QString name = QInputDialog::getItem(m_pSidebarWidget,
                tr("Export to Crate"),
                //: Only its Crate tracks of this server go to Crate
                tr("Playlist to export (only its Crate tracks go):"),
                names,
                0,
                false,
                &ok);
        const int chosen = static_cast<int>(names.indexOf(name));
        if (ok && chosen >= 0) {
            exportPlaylistToCrate(node.serverId, playlists[chosen].first);
        }
        return;
    }
    if (pChosen && pChosen == pRadio) {
        startRadio(node.serverId, *radioSeed);
        return;
    }
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
