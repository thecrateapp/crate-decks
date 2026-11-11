// Loading a Crate track that is not cached into the real decks (plan
// F4.C-PR3), through the track table and menu hooks, as a DJ does: the
// application's Library, PlayerManager and engine, with the network faked.
// It mirrors upstream's playermanagertest.cpp fixture.

#include <gtest/gtest.h>

#include <QAction>
#include <QFile>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QSignalSpy>
#include <QSqlError>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
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
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "mixer/deck.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"
#include "test/soundsourceproviderregistration.h"
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
        QFile audio(getTestDir().filePath(kTrackFile));
        ASSERT_TRUE(audio.open(QIODevice::ReadOnly));
        m_audio = audio.readAll();

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

} // namespace
