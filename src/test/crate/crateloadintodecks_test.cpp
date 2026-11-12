// Loading a Crate track that is not cached into the real decks (plan
// F4.C-PR3), through the track table and menu hooks, as a DJ does: the
// application's Library, PlayerManager and engine, with the network faked.
// It mirrors upstream's playermanagertest.cpp fixture.

#include <attachedpictureframe.h>
#include <gtest/gtest.h>
#include <id3v2tag.h>
#include <mpegfile.h>

#include <QAction>
#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QSignalSpy>
#include <QSqlError>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <memory>

#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
#include "crate/cache/audiocache.h"
#include "crate/cratepreferences.h"
#include "crate/crateservers.h"
#include "crate/db/crateserverprofiles.h"
#include "crate/db/crateserverschema.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarystore.h"
#include "database/mixxxdb.h"
#include "effects/effectsmanager.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"
#include "library/coverartcache.h"
#include "library/crateserver/crateserverfeature.h"
#include "library/crateserver/crateservertrackmodel.h"
#include "library/dao/playlistdao.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "mixer/deck.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "sources/soundsourceproxy.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/taglib/trackmetadata_file.h"
#include "track/track.h"
#include "widget/wtrackmenu.h"
#include "widget/wtracktableview.h"
#ifdef __RUBBERBAND__
#include "engine/bufferscalers/rubberbandworkerpool.h"
#endif

namespace {

using crate::CrateServers;
using crate::ServerProfile;
using crate::library::CrateServerFeature;
using crate::library::CrateServerTrackModel;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeSecretStore;

const QString kServer = QStringLiteral("srv-1");
const QString kTrackFile = QStringLiteral("id3-test-data/cover-test-øé~ł€˚-png.mp3");
const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");
constexpr int kWaitMs = 10000;

const QByteArray kCapabilities = QByteArrayLiteral(
        R"({"decks": {"available": true, "min_client_version": "0.1.0",
            "contract_version": 1, "features": ["library_cursor"]}})");
const QByteArray kTree = QByteArrayLiteral(R"({"library": {"track_count": 2},
    "containers": [{"id": "7", "kind": "playlist", "name": "Warm-up", "parent_id": null,
     "track_count": 2, "etag": "p7"}]})");

void deleteTrack(Track* pTrack) {
    delete pTrack;
}

class CrateLoadIntoDecksTest : public MixxxDbTest, SoundSourceProviderRegistration {
  public:
    CrateLoadIntoDecksTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(m_cacheDir.isValid());
        m_audio = testAudio();
        ASSERT_FALSE(m_audio.isEmpty());

        // As coreservices.cpp and upstream's PlayerManagerTest build them
        auto pChannelHandleFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager = std::make_shared<EffectsManager>(m_pConfig, pChannelHandleFactory);
        m_pEngine = std::make_shared<EngineMixer>(
                m_pConfig, "[Master]", m_pEffectsManager.get(), pChannelHandleFactory, true);
        m_pSoundManager = std::make_shared<SoundManager>(m_pConfig, m_pEngine.get());
        m_pControlIndicatorTimer = std::make_shared<mixxx::ControlIndicatorTimer>(nullptr);
        m_pEngine->registerNonEngineChannelSoundIO(gsl::make_not_null(m_pSoundManager.get()));
        CoverArtCache::createInstance();
        m_pPlayerManager = std::make_shared<PlayerManager>(
                m_pConfig, m_pSoundManager.get(), m_pEffectsManager.get(), m_pEngine.get());
        m_pPlayerManager->addConfiguredDecks();
        PlayerInfo::create();
        m_pEffectsManager->setup();
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection()));
        m_pTrackCollectionManager = std::make_unique<TrackCollectionManager>(
                nullptr, m_pConfig, dbConnectionPooler(), deleteTrack);
        m_pRecordingManager = std::make_shared<RecordingManager>(m_pConfig, m_pEngine.get());

        // The Crate server, its tree and one playlist of two tracks
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ServerProfile profile;
        profile.id = kServer;
        profile.baseUrl = QUrl(QStringLiteral("https://dev.crate.local"));
        ASSERT_TRUE(crate::db::saveServerProfile(dbConnection(), profile));
        ASSERT_TRUE(crate::db::saveLastCapabilities(dbConnection(), kServer, kCapabilities));
        const auto tree = crate::library::parseContainerTree(kTree);
        ASSERT_TRUE(tree.ok());
        ASSERT_EQ(crate::library::WriteStatus::Ok,
                crate::library::storeContainerTree(dbConnection(), kServer, tree.value()));
        QList<crate::library::RemoteTrack> tracks;
        for (const QString& uid : {QStringLiteral("e1"), QStringLiteral("e2")}) {
            crate::library::RemoteTrack track;
            track.entityUid = uid;
            track.title = QStringLiteral("Track ") + uid;
            track.artist = QStringLiteral("Ondine Varga");
            track.format = QStringLiteral("mp3");
            track.byteLength = m_audio.size();
            track.playable = true;
            tracks.append(track);
        }
        ASSERT_EQ(crate::library::WriteStatus::Ok,
                crate::library::storeTracks(
                        dbConnection(), kServer, tracks, QStringLiteral("2026-11-11T10:00:00Z")));
        const auto stored = crate::library::loadContainer(dbConnection(), playlistId());
        ASSERT_TRUE(stored.has_value());
        ASSERT_EQ(crate::library::WriteStatus::Ok,
                crate::library::replaceContainerTracks(dbConnection(),
                        playlistId(),
                        {QStringLiteral("e1"), QStringLiteral("e2")},
                        {{{QString(), QStringLiteral("\"t\""), 2}},
                                stored->etag,
                                QStringLiteral("2026-11-11T10:00:00Z")}));

        m_store.secrets.insert(kServer + QStringLiteral(":refresh"), QStringLiteral("refresh-1"));
        CrateServers::Dependencies dependencies;
        dependencies.pConfig = m_pConfig;
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.pSecretStore = &m_store;
        dependencies.audioCacheDirectory = m_cacheDir.path();
        m_pServers = std::make_unique<CrateServers>(dependencies);
        m_pServers->start();
        ASSERT_FALSE(m_network.sent.isEmpty());
        m_network.last().pReply->respond(200,
                QByteArrayLiteral(R"({"id": 7, "email": "dj@crate.test", "role": "user",
                    "token": "access-1", "refresh_token": "refresh-2"})"));

        // The application's Library without its own Crate section: this
        // test adds one that uses the fake network
        m_pConfig->setValue(ConfigKey(QStringLiteral("[Library]"),
                                    QStringLiteral("ShowCrateServer")),
                false);
        m_pLibrary = std::make_shared<Library>(nullptr,
                m_pConfig,
                dbConnectionPooler(),
                m_pTrackCollectionManager.get(),
                m_pPlayerManager.get(),
                m_pRecordingManager.get());
        m_pPlayerManager->bindToLibrary(m_pLibrary.get());
#ifdef __RUBBERBAND__
        RubberBandWorkerPool::createInstance();
#endif
        CrateServerFeature::Dependencies featureDependencies;
        featureDependencies.pTrackCollectionManager = m_pTrackCollectionManager.get();
        featureDependencies.pDbConnectionPool = dbConnectionPooler();
        featureDependencies.pServers = m_pServers.get();
        featureDependencies.pPlayerInfo = &PlayerInfo::instance();
        // Owned by the Library, which wires its loads to the PlayerManager
        m_pFeature = new CrateServerFeature(m_pLibrary.get(), m_pConfig, featureDependencies);
        m_pLibrary->addFeature(m_pFeature);
        ASSERT_NE(nullptr, m_pFeature->loadCoordinator());
    }

    void TearDown() override {
        m_pView.reset();
        CoverArtCache::destroy();
#ifdef __RUBBERBAND__
        RubberBandWorkerPool::destroy();
#endif
    }

    ~CrateLoadIntoDecksTest() override {
        m_pSoundManager.reset();
        m_pPlayerManager.reset();
        PlayerInfo::destroy();
        m_pLibrary.reset();
        m_pServers.reset();
        m_pRecordingManager.reset();
        m_pEngine.reset();
        m_pEffectsManager.reset();
        m_pTrackCollectionManager.reset();
        m_pControlIndicatorTimer.reset();
    }

  protected:
    /// The file the server sends for both tracks.
    virtual QByteArray testAudio() const {
        QFile audio(getTestDir().filePath(kTrackFile));
        return audio.open(QIODevice::ReadOnly) ? audio.readAll() : QByteArray();
    }

    qint64 playlistId() const {
        QSqlQuery query(dbConnection());
        query.prepare(
                QStringLiteral("SELECT id FROM crateserver_containers WHERE "
                               "kind = 'playlist' AND remote_id = '7'"));
        return query.exec() && query.next() ? query.value(0).toLongLong() : 0;
    }

    /// The Warm-up playlist in a track table, as the library shows it.
    CrateServerTrackModel* showPlaylist() {
        QSignalSpy shown(m_pFeature, &LibraryFeature::showTrackModel);
        TreeItem* pPlaylists = nullptr;
        for (TreeItem* pChild : m_pFeature->sidebarModel()->getRootItem()->children()) {
            if (pChild->getLabel().startsWith(QStringLiteral("Playlists"))) {
                pPlaylists = pChild;
            }
        }
        EXPECT_NE(nullptr, pPlaylists);
        // The sidebar is read off the main thread
        EXPECT_TRUE(QTest::qWaitFor([&] { return pPlaylists && pPlaylists->hasChildren(); },
                kWaitMs));
        if (!pPlaylists || !pPlaylists->hasChildren()) {
            return nullptr;
        }
        TreeItemModel* pSidebar = m_pFeature->sidebarModel();
        const QModelIndex playlists = pSidebar->index(pPlaylists->parentRow(), 0);
        m_pFeature->activateChild(pSidebar->index(0, 0, playlists));
        if (shown.isEmpty()) {
            return nullptr;
        }
        auto* pModel = dynamic_cast<CrateServerTrackModel*>(
                shown.first().first().value<QAbstractItemModel*>());
        EXPECT_TRUE(pModel && QTest::qWaitFor([&] { return pModel->rowCount() == 2; }, kWaitMs));
        m_pView = std::make_unique<WTrackTableView>(nullptr, m_pConfig, m_pLibrary.get(), 1.0);
        m_pView->loadTrackModel(pModel);
        return pModel;
    }

    /// Answers the download of `uid` with the test file.
    bool serveDownload(const QString& uid) {
        const QString path = QStringLiteral("/api/tracks/by-entity/%1/stream").arg(uid);
        crate::test::FakeReply* pReply = nullptr;
        const bool sent = QTest::qWaitFor(
                [&] {
                    for (const auto& request : std::as_const(m_network.sent)) {
                        if (request.request.url().path() == path && request.pReply &&
                                !request.pReply->isFinished()) {
                            pReply = request.pReply.data();
                        }
                    }
                    return pReply != nullptr;
                },
                kWaitMs);
        if (!sent) {
            return false;
        }
        pReply->respond(200,
                m_audio,
                {{QByteArrayLiteral("Content-Length"), QByteArray::number(m_audio.size())},
                        {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/mpeg")}});
        return true;
    }

    /// Answers the request for the mix profile of `uid` with a full one, in
    /// 8A (A minor).
    bool serveProfile(const QString& uid) {
        QFile file(getTestDir().filePath(
                QStringLiteral("crate/data/mix-profiles/full-constant-124.json")));
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        QJsonObject profile = QJsonDocument::fromJson(file.readAll()).object();
        profile.insert(QStringLiteral("trackEntityUid"), uid);
        const QString path = QStringLiteral("/api/tracks/by-entity/%1/mix-profile").arg(uid);
        crate::test::FakeReply* pReply = nullptr;
        const bool sent = QTest::qWaitFor(
                [&] {
                    for (const auto& request : std::as_const(m_network.sent)) {
                        if (request.request.url().path() == path && request.pReply &&
                                !request.pReply->isFinished()) {
                            pReply = request.pReply.data();
                        }
                    }
                    return pReply != nullptr;
                },
                kWaitMs);
        if (!sent) {
            return false;
        }
        pReply->respond(200, QJsonDocument(profile).toJson());
        return true;
    }

    /// Lists `uid` in the library with the revision of the profile
    /// serveProfile() answers.
    void listProfileOf(const QString& uid) {
        QFile file(getTestDir().filePath(
                QStringLiteral("crate/data/mix-profiles/full-constant-124.json")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QJsonObject profile = QJsonDocument::fromJson(file.readAll()).object();
        QSqlQuery listed(dbConnection());
        listed.prepare(QStringLiteral(
                "UPDATE crateserver_tracks SET profile_revision = :revision "
                "WHERE server_id = :server AND entity_uid = :uid"));
        listed.bindValue(QStringLiteral(":revision"),
                profile.value(QStringLiteral("profileRevision")).toString());
        listed.bindValue(QStringLiteral(":server"), kServer);
        listed.bindValue(QStringLiteral(":uid"), uid);
        // Again while the library's workers hold the database
        ASSERT_TRUE(QTest::qWaitFor([&] { return listed.exec(); }, kWaitMs))
                << listed.lastError().text().toStdString();
    }

    QString cachedPathOf(const QString& uid) const {
        return m_cacheDir.filePath(kServer + QLatin1Char('/') + uid + QLatin1Char('/') + uid +
                QStringLiteral(".mp3"));
    }

    /// Waits until `group` plays the cached file of `uid` in the engine.
    bool deckHasLoaded(const QString& group, const QString& uid) {
        Deck* pDeck = m_pPlayerManager->getDeck(group == kDeck1 ? 0 : 1);
        return QTest::qWaitFor(
                [&] {
                    const TrackPointer pTrack = pDeck->getLoadedTrack();
                    m_pEngine->process(1024);
                    return pTrack && pTrack->getLocation() == cachedPathOf(uid) &&
                            pDeck->getEngineDeck()->getEngineBuffer()->isTrackLoaded();
                },
                kWaitMs);
    }

    QTemporaryDir m_cacheDir;
    QByteArray m_audio;
    /// What the main window's timer drives; the track table reads it.
    ControlObject m_guiTick{ConfigKey(
            QStringLiteral("[App]"), QStringLiteral("gui_tick_50ms_period_s"))};
    FakeNetworkAccessManager m_network;
    FakeSecretStore m_store;
    std::shared_ptr<EffectsManager> m_pEffectsManager;
    std::shared_ptr<mixxx::ControlIndicatorTimer> m_pControlIndicatorTimer;
    std::shared_ptr<EngineMixer> m_pEngine;
    std::shared_ptr<SoundManager> m_pSoundManager;
    std::shared_ptr<PlayerManager> m_pPlayerManager;
    std::unique_ptr<TrackCollectionManager> m_pTrackCollectionManager;
    std::shared_ptr<RecordingManager> m_pRecordingManager;
    std::unique_ptr<CrateServers> m_pServers;
    std::shared_ptr<Library> m_pLibrary;
    CrateServerFeature* m_pFeature = nullptr;
    std::unique_ptr<WTrackTableView> m_pView;
};

TEST_F(CrateLoadIntoDecksTest, LoadSelectedTrackDownloadsThenLoadsTheDeck) {
    CrateServerTrackModel* pModel = showPlaylist();
    ASSERT_NE(nullptr, pModel);
    Deck* pDeck1 = m_pPlayerManager->getDeck(0);
    ASSERT_EQ(nullptr, pDeck1->getLoadedTrack());

    // [Channel1],LoadSelectedTrack (keyboard or controller) on the first row
    m_pView->selectionModel()->select(pModel->index(0, 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_pView->loadSelectedTrackToGroup(kDeck1,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);
    // Nothing in the deck while the file downloads
    EXPECT_EQ(nullptr, pDeck1->getLoadedTrack());

    ASSERT_TRUE(serveDownload(QStringLiteral("e1")));
    ASSERT_TRUE(deckHasLoaded(kDeck1, QStringLiteral("e1")));
    EXPECT_TRUE(pDeck1->getLoadedTrack()->getId().isValid());
}

TEST_F(CrateLoadIntoDecksTest, TrackMenuLoadsAnUncachedTrackIntoDeck2) {
    CrateServerTrackModel* pModel = showPlaylist();
    ASSERT_NE(nullptr, pModel);

    // The menu of a track that has no Track yet still offers the decks
    WTrackMenu menu(nullptr, m_pConfig, m_pLibrary.get(), WTrackMenu::Feature::LoadTo, pModel);
    menu.loadTrackModelIndices({pModel->index(1, 0)});
    menu.updateMenus();
    QAction* pDeck2 = nullptr;
    const QList<QMenu*> menus = menu.findChildren<QMenu*>();
    for (const QMenu* pMenu : menus) {
        const QList<QAction*> actions = pMenu->actions();
        for (QAction* pAction : actions) {
            if (pAction->text() == QStringLiteral("Deck 2")) {
                pDeck2 = pAction;
            }
        }
    }
    ASSERT_NE(nullptr, pDeck2);
    ASSERT_TRUE(pDeck2->isEnabled());
    // And the model's own entries
    bool download = false;
    const QList<QAction*> actions = menu.actions();
    for (const QAction* pAction : actions) {
        download = download || (pAction->text() == QStringLiteral("Download"));
    }
    EXPECT_TRUE(download);

    pDeck2->trigger();
    ASSERT_TRUE(serveDownload(QStringLiteral("e2")));
    ASSERT_TRUE(deckHasLoaded(kDeck2, QStringLiteral("e2")));
}

// The Crate preferences choose what of Crate's analysis the loaded track
// takes (plan F4.C-PR4): here the key and the gain from loudness.
TEST_F(CrateLoadIntoDecksTest, LoadedTrackTakesTheAnalysisThePreferencesChoose) {
    m_pServers->preferences()->setKeyFromCrate(true);
    m_pServers->preferences()->setGainFromLufs(true);
    QFile file(getTestDir().filePath(
            QStringLiteral("crate/data/mix-profiles/full-constant-124.json")));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject profile = QJsonDocument::fromJson(file.readAll()).object();
    profile.insert(QStringLiteral("trackEntityUid"), QStringLiteral("e1"));
    // The library lists the track with a profile
    QSqlQuery listed(dbConnection());
    listed.prepare(QStringLiteral(
            "UPDATE crateserver_tracks SET profile_revision = :revision "
            "WHERE server_id = :server AND entity_uid = 'e1'"));
    listed.bindValue(QStringLiteral(":revision"),
            profile.value(QStringLiteral("profileRevision")).toString());
    listed.bindValue(QStringLiteral(":server"), kServer);
    // Again while the library's workers hold the database
    ASSERT_TRUE(QTest::qWaitFor([&] { return listed.exec(); }, kWaitMs))
            << listed.lastError().text().toStdString();
    CrateServerTrackModel* pModel = showPlaylist();
    ASSERT_NE(nullptr, pModel);
    m_pView->selectionModel()->select(pModel->index(0, 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_pView->loadSelectedTrackToGroup(kDeck1,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);
    ASSERT_TRUE(serveDownload(QStringLiteral("e1")));
    ASSERT_TRUE(deckHasLoaded(kDeck1, QStringLiteral("e1")));

    // Its mix profile (a full one, in 8A at -8.6 LUFS), once asked for
    const QString path = QStringLiteral("/api/tracks/by-entity/e1/mix-profile");
    crate::test::FakeReply* pReply = nullptr;
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                for (const auto& request : std::as_const(m_network.sent)) {
                    if (request.request.url().path() == path && request.pReply &&
                            !request.pReply->isFinished()) {
                        pReply = request.pReply.data();
                    }
                }
                return pReply != nullptr;
            },
            kWaitMs));
    pReply->respond(200, QJsonDocument(profile).toJson());

    const TrackPointer pTrack = m_pPlayerManager->getDeck(0)->getLoadedTrack();
    ASSERT_NE(nullptr, pTrack);
    EXPECT_TRUE(QTest::qWaitFor(
            [&] {
                return pTrack->getKey() == mixxx::track::io::key::A_MINOR &&
                        pTrack->getReplayGain().hasRatio();
            },
            kWaitMs));
}

// "Add to Auto DJ Queue (bottom)" on two uncached Crate tracks, then Auto DJ
// (F4.C-PR3 decision 1, Diego, 2026-11-11): the queue shows them with Crate's
// metadata, the first one downloads when its turn comes, the real deck 1
// plays it, and it takes Crate's analysis once loaded, as a cached track does.
TEST_F(CrateLoadIntoDecksTest, AutoDjPlaysUncachedTracksAddedFromTheTrackMenu) {
    m_pServers->preferences()->setKeyFromCrate(true);
    listProfileOf(QStringLiteral("e1"));
    CrateServerTrackModel* pModel = showPlaylist();
    ASSERT_NE(nullptr, pModel);

    WTrackMenu menu(nullptr, m_pConfig, m_pLibrary.get(), WTrackMenu::Feature::AutoDJ, pModel);
    menu.loadTrackModelIndices({pModel->index(0, 0), pModel->index(1, 0)});
    menu.updateMenus();
    QAction* pBottom = nullptr;
    const QList<QAction*> actions = menu.actions();
    for (QAction* pAction : actions) {
        if (pAction->text() == QStringLiteral("Add to Auto DJ Queue (bottom)")) {
            pBottom = pAction;
        }
    }
    ASSERT_NE(nullptr, pBottom);
    ASSERT_TRUE(pBottom->isEnabled());
    pBottom->trigger();

    PlaylistDAO& playlists =
            m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
    const QList<TrackId> queue = playlists.getAutoDJTrackIds();
    ASSERT_EQ(2, queue.size());
    {
        const TrackPointer pFirst = m_pTrackCollectionManager->getTrackById(queue[0]);
        ASSERT_NE(nullptr, pFirst);
        EXPECT_EQ(cachedPathOf(QStringLiteral("e1")), pFirst->getLocation());
        EXPECT_EQ(QStringLiteral("Track e1"), pFirst->getTitle());
        EXPECT_EQ(QStringLiteral("Ondine Varga"), pFirst->getArtist());
    }
    EXPECT_FALSE(QFile::exists(cachedPathOf(QStringLiteral("e1"))));

    // Auto DJ on: the first track is on its way, then deck 1 plays it
    ControlObject::set(ConfigKey(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled")), 1.0);
    EXPECT_EQ(1.0,
            ControlObject::get(ConfigKey(
                    QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"))));
    ASSERT_TRUE(serveDownload(QStringLiteral("e1")));
    ASSERT_TRUE(deckHasLoaded(kDeck1, QStringLiteral("e1")));
    const TrackPointer pTrack = m_pPlayerManager->getDeck(0)->getLoadedTrack();
    ASSERT_NE(nullptr, pTrack);
    EXPECT_EQ(queue[0], pTrack->getId());

    // Crate's analysis, once loaded: here its key
    ASSERT_TRUE(serveProfile(QStringLiteral("e1")));
    EXPECT_TRUE(QTest::qWaitFor(
            [&] { return pTrack->getKey() == mixxx::track::io::key::A_MINOR; }, kWaitMs));
    ControlObject::set(ConfigKey(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled")), 0.0);
}

// Design §9: the LRU never evicts what is loaded (session pins, decided by
// Diego on 2026-11-11). A track playing in the real deck 1 keeps its file when
// the limit drops to nothing and the cache is cleared; once ejected, it goes.
TEST_F(CrateLoadIntoDecksTest, TrackInADeckSurvivesClearCacheAndALowerLimit) {
    CrateServerTrackModel* pModel = showPlaylist();
    ASSERT_NE(nullptr, pModel);
    m_pView->selectionModel()->select(pModel->index(0, 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_pView->loadSelectedTrackToGroup(kDeck1,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);
    ASSERT_TRUE(serveDownload(QStringLiteral("e1")));
    ASSERT_TRUE(deckHasLoaded(kDeck1, QStringLiteral("e1")));
    crate::cache::AudioCache* pCache = m_pServers->audioCache();
    ASSERT_TRUE(pCache->cachedPath(kServer, QStringLiteral("e1")).has_value());

    // Preferences: the lowest limit, then "Clear cache"
    pCache->setLimitBytes(0);
    EXPECT_EQ(0, pCache->clear());
    EXPECT_TRUE(pCache->cachedPath(kServer, QStringLiteral("e1")).has_value());
    EXPECT_TRUE(QFile::exists(cachedPathOf(QStringLiteral("e1"))));
    EXPECT_EQ(m_audio.size(), pCache->usage().inUseBytes);

    // Ejected (later than Mixxx's double press, which would reload the
    // last ejected track instead): the limit takes it. The engine keeps
    // running while time passes, as with a sound card: it takes in the chunks
    // read for the loaded track. Left unread, they reach the reader after the
    // eject, while it unloads, and trip its debug assertion
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 600) {
        m_pEngine->process(1024);
        QTest::qWait(10);
    }
    ControlObject::set(ConfigKey(kDeck1, QStringLiteral("eject")), 1.0);
    ControlObject::set(ConfigKey(kDeck1, QStringLiteral("eject")), 0.0);
    Deck* pDeck = m_pPlayerManager->getDeck(0);
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                m_pEngine->process(1024);
                return pDeck->getLoadedTrack() == nullptr;
            },
            kWaitMs));
    EXPECT_FALSE(pCache->cachedPath(kServer, QStringLiteral("e1")).has_value());
    EXPECT_TRUE(QTest::qWaitFor(
            [&] { return !QFile::exists(cachedPathOf(QStringLiteral("e1"))); }, kWaitMs));
}

/// The main thread may block for 16 ms at most (design §9).
constexpr double kStallBudgetMs = 16.0;
/// The side of the embedded cover of the large-cover file, in pixels.
constexpr int kLargeCoverSide = 4000;

/// The test file with an embedded PNG cover of `side` × `side` pixels in
/// place of its own. Adding a file to the library decodes, scales and hashes
/// its embedded cover: the larger, the longer.
QByteArray withLargeCover(const QString& sourcePath, int side) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("large-cover.mp3"));
    if (!dir.isValid() || !QFile::copy(sourcePath, path)) {
        return QByteArray();
    }
    QImage image(side, side, QImage::Format_RGB32);
    for (int y = 0; y < side; ++y) {
        auto* pLine = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < side; ++x) {
            pLine[x] = qRgb(x % 256, y % 256, (x + y) % 256);
        }
    }
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        return QByteArray();
    }
    {
        TagLib::MPEG::File file(TAGLIB_FILENAME_FROM_QSTRING(path));
        TagLib::ID3v2::Tag* pTag = file.ID3v2Tag(true);
        if (!pTag) {
            return QByteArray();
        }
        pTag->removeFrames("APIC");
        auto* pFrame = new TagLib::ID3v2::AttachedPictureFrame();
        pFrame->setMimeType("image/png");
        pFrame->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
        pFrame->setPicture(TagLib::ByteVector(
                png.constData(), static_cast<unsigned int>(png.size())));
        // Owned by the tag
        pTag->addFrame(pFrame);
        if (!file.save()) {
            return QByteArray();
        }
    }
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// How long it takes to read the tags and the cover of `audio` as Mixxx
/// does when it adds a file to its library, on this machine.
double importCostMs(const QByteArray& audio) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("import-cost.mp3"));
    QFile file(path);
    if (!dir.isValid() || !file.open(QIODevice::WriteOnly) || file.write(audio) != audio.size()) {
        return -1.0;
    }
    file.close();
    const TrackPointer pTrack = Track::newTemporary(mixxx::FileAccess(mixxx::FileInfo(path)));
    QElapsedTimer timer;
    timer.start();
    SoundSourceProxy(pTrack).updateTrackFromSource(
            SoundSourceProxy::UpdateTrackFromSourceMode::Once, SyncTrackMetadataParams());
    const double ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    return pTrack->getCoverInfo().type == CoverInfo::METADATA ? ms : -1.0;
}

/// Runs the event loop with a 1 ms timer on the main thread: `action` on the
/// first tick, then `done` on each one until it holds. Returns the longest
/// time between two ticks, `action` included and leaving out the time `done`
/// takes: how long the main thread was blocked, at most. Negative if `done`
/// never held.
double longestStallUntil(
        const std::function<void()>& action, const std::function<bool()>& done) {
    struct State {
        QEventLoop loop;
        QElapsedTimer clock;
        qint64 lastTickNs = 0;
        double longestMs = 0.0;
        bool acted = false;
        bool held = false;
    } state;
    State* const pState = &state;
    const std::function<void()>* const pAction = &action;
    const std::function<bool()>* const pDone = &done;
    QTimer tick;
    tick.setTimerType(Qt::PreciseTimer);
    tick.setInterval(1);
    QObject::connect(&tick, &QTimer::timeout, &state.loop, [pState, pAction, pDone] {
        const qint64 nowNs = pState->clock.nsecsElapsed();
        pState->longestMs = std::max(pState->longestMs,
                static_cast<double>(nowNs - pState->lastTickNs) / 1e6);
        if (!pState->acted) {
            // What the action takes counts: the next tick measures from here
            pState->acted = true;
            pState->lastTickNs = nowNs;
            (*pAction)();
            return;
        }
        if ((*pDone)()) {
            pState->held = true;
            pState->loop.quit();
            return;
        } else if (pState->clock.elapsed() > kWaitMs) {
            pState->loop.quit();
            return;
        }
        pState->lastTickNs = pState->clock.nsecsElapsed();
    });
    state.clock.start();
    state.lastTickNs = state.clock.nsecsElapsed();
    tick.start();
    state.loop.exec();
    return state.held ? state.longestMs : -1.0;
}

/// The decks with a file whose embedded cover is large, so that adding it
/// to the library takes far longer than the main thread may block.
class CrateLoadIntoDecksLargeCoverTest : public CrateLoadIntoDecksTest {
  protected:
    QByteArray testAudio() const override {
        return withLargeCover(getTestDir().filePath(kTrackFile), kLargeCoverSide);
    }

    /// Whether `group` has the cached file of `uid` in the engine; drives
    /// the engine once, without waiting.
    bool deckIsLoaded(const QString& group, const QString& uid) {
        Deck* pDeck = m_pPlayerManager->getDeck(group == kDeck1 ? 0 : 1);
        const TrackPointer pTrack = pDeck->getLoadedTrack();
        m_pEngine->process(1024);
        return pTrack && pTrack->getLocation() == cachedPathOf(uid) &&
                pDeck->getEngineDeck()->getEngineBuffer()->isTrackLoaded();
    }

    /// The playlist in a track table wired to the decks as
    /// Library::bindLibraryWidget() wires the application's.
    CrateServerTrackModel* showPlaylistOnTheDecks() {
        CrateServerTrackModel* pModel = showPlaylist();
        if (pModel) {
            QObject::connect(m_pView.get(),
                    &WTrackTableView::loadTrackToPlayer,
                    m_pLibrary.get(),
                    &Library::slotLoadTrackToPlayer);
        }
        return pModel;
    }

    void loadFirstRowInto(CrateServerTrackModel* pModel, const QString& group) {
        m_pView->selectionModel()->select(pModel->index(0, 0),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_pView->loadSelectedTrackToGroup(group,
#ifdef __STEM__
                mixxx::StemChannelSelection(),
#endif
                false);
    }
};

// The first load of a Crate track adds its file to the library, which reads
// its tags and decodes, scales and hashes its embedded cover: 40-90 ms with
// the dev stack's files (G3, 2026-11-11). That happens off the main thread;
// the main thread only writes the library row.
TEST_F(CrateLoadIntoDecksLargeCoverTest, FirstLoadReadsTheFileOffTheMainThread) {
    const double importMs = importCostMs(m_audio);
    ASSERT_GT(importMs, 2.0 * kStallBudgetMs) << "The cover is too small to tell: " << importMs;
    CrateServerTrackModel* pModel = showPlaylistOnTheDecks();
    ASSERT_NE(nullptr, pModel);
    loadFirstRowInto(pModel, kDeck1);
    ASSERT_TRUE(serveDownload(QStringLiteral("e1")));

    const double stallMs = longestStallUntil([] {},
            [this] { return deckIsLoaded(kDeck1, QStringLiteral("e1")); });
    ASSERT_GE(stallMs, 0.0) << "Deck 1 never loaded the track";
    EXPECT_LT(stallMs, importMs / 2.0)
            << "The main thread blocked " << stallMs << " ms; the import takes " << importMs
            << " ms";
    const TrackPointer pTrack = m_pPlayerManager->getDeck(0)->getLoadedTrack();
    ASSERT_NE(nullptr, pTrack);
    EXPECT_TRUE(pTrack->getId().isValid());
    // The cover was read, off the main thread
    EXPECT_EQ(CoverInfo::METADATA, pTrack->getCoverInfo().type);
}

// A track cached before (a download, an offline set) whose file the library
// does not have yet: selecting its row and loading it never read the file on
// the main thread either.
TEST_F(CrateLoadIntoDecksLargeCoverTest, CachedTrackFirstLoadReadsTheFileOffTheMainThread) {
    const double importMs = importCostMs(m_audio);
    ASSERT_GT(importMs, 2.0 * kStallBudgetMs) << "The cover is too small to tell: " << importMs;
    CrateServerTrackModel* pModel = showPlaylistOnTheDecks();
    ASSERT_NE(nullptr, pModel);
    crate::library::CrateTrackRef track;
    track.serverId = kServer;
    track.entityUid = QStringLiteral("e1");
    track.hints.format = QStringLiteral("mp3");
    track.hints.byteLength = m_audio.size();
    m_pFeature->loadCoordinator()->download({track}, crate::cache::Priority::Prefetch);
    ASSERT_TRUE(serveDownload(QStringLiteral("e1")));
    crate::cache::AudioCache* pCache = m_pServers->audioCache();
    // The download ends and the row shows it
    const double downloadStallMs = longestStallUntil([] {},
            [pCache] { return pCache->cachedPath(kServer, QStringLiteral("e1")).has_value(); });
    ASSERT_GE(downloadStallMs, 0.0) << "The download never ended";
    EXPECT_LT(downloadStallMs, importMs / 2.0)
            << "The main thread blocked " << downloadStallMs
            << " ms as the download ended; the import takes " << importMs << " ms";
    // What the table and the cache do next
    QElapsedTimer settling;
    settling.start();
    const double settleStallMs =
            longestStallUntil([] {}, [&settling] { return settling.elapsed() > 300; });
    EXPECT_LT(settleStallMs, importMs / 2.0)
            << "The main thread blocked " << settleStallMs
            << " ms after the download; the import takes " << importMs << " ms";

    const double stallMs = longestStallUntil(
            [this, pModel] {
                // As the track table does once a row stays selected
                Q_UNUSED(pModel->getTrack(pModel->index(0, 0)));
                loadFirstRowInto(pModel, kDeck1);
            },
            [this] { return deckIsLoaded(kDeck1, QStringLiteral("e1")); });
    ASSERT_GE(stallMs, 0.0) << "Deck 1 never loaded the track";
    EXPECT_LT(stallMs, importMs / 2.0)
            << "The main thread blocked " << stallMs << " ms; the import takes " << importMs
            << " ms";
    const TrackPointer pTrack = m_pPlayerManager->getDeck(0)->getLoadedTrack();
    ASSERT_NE(nullptr, pTrack);
    EXPECT_EQ(CoverInfo::METADATA, pTrack->getCoverInfo().type);
}

} // namespace
