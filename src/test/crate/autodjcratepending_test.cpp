// Auto DJ's pending track hook (plan F4.C-PR3), on the fixture of upstream's
// autodjprocessor_test.cpp: Crate tracks whose file is still downloading stay
// in the queue for a while instead of being skipped as missing. Uncached
// Crate tracks added to the queue (F4.C-PR3 decision 1) download when their
// turn comes, through the real cache, prefetcher and load coordinator with
// the network faked.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QFile>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

#include "control/controllinpotmeter.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "crate/cache/audiocache.h"
#include "crate/client/crateclient.h"
#include "crate/library/crateloadcoordinator.h"
#include "crate/library/crateprefetcher.h"
#include "library/autodj/autodjprocessor.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/playlisttablemodel.h"
#include "library/trackcollection.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "test/crate/fakecratenetwork.h"
#include "test/librarytest.h"
#include "track/track.h"
#include "util/time.h"

// Its own namespace: upstream's test defines classes of the same names
namespace crate_autodj_pending {

using ::testing::Return;

const QString kTrackFile = QStringLiteral("id3-test-data/cover-test-øé~ł€˚-png.mp3");
const QString kAppGroup = QStringLiteral("[App]");
const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kServer = QStringLiteral("srv-1");
constexpr int kWaitMs = 5000;

class FakeStorageInfo : public crate::cache::StorageInfo {
  public:
    qint64 bytesAvailable(const QString&) const override {
        return 100 * crate::cache::AudioCache::kGiB;
    }
};

class FakeMixer {
  public:
    FakeMixer()
            : crossfader(ConfigKey("[Master]", "crossfader"), -1.0, 1.0),
              crossfaderReverse(ConfigKey("[Mixer Profile]", "xFaderReverse")) {
        crossfaderReverse.setButtonMode(mixxx::control::ButtonMode::Toggle);
    }

    ControlPotmeter crossfader;
    ControlPushButton crossfaderReverse;
};

class FakeDeck : public BaseTrackPlayer {
  public:
    FakeDeck(const QString& group, EngineChannel::ChannelOrientation orient)
            : BaseTrackPlayer(nullptr, group),
              trackSamples(ConfigKey(group, "track_samples")),
              samplerate(ConfigKey(group, "track_samplerate")),
              rateratio(ConfigKey(group, "rate_ratio"), true, false, false, 1.0),
              playposition(ConfigKey(group, "playposition"), 0.0, 1.0, 0, 0, true),
              play(ConfigKey(group, "play")),
              repeat(ConfigKey(group, "repeat")),
              introStartPos(ConfigKey(group, "intro_start_position")),
              introEndPos(ConfigKey(group, "intro_end_position")),
              outroStartPos(ConfigKey(group, "outro_start_position")),
              outroEndPos(ConfigKey(group, "outro_end_position")),
              orientation(ConfigKey(group, "orientation")) {
        play.setButtonMode(mixxx::control::ButtonMode::Toggle);
        repeat.setButtonMode(mixxx::control::ButtonMode::Toggle);
        outroStartPos.set(Cue::kNoPosition);
        outroEndPos.set(Cue::kNoPosition);
        orientation.set(orient);
    }

    TrackPointer getLoadedTrack() const override {
        return loadedTrack;
    }

    void setupEqControls() override {
    }

    void slotLoadTrack(TrackPointer pTrack,
#ifdef __STEM__
            mixxx::StemChannelSelection,
#endif
            bool bPlay) override {
        loadedTrack = pTrack;
        play.set(bPlay);
    }

    void slotEjectTrack(double val) override {
        if (val > 0) {
            loadedTrack = nullptr;
        }
    }

    MOCK_METHOD1(slotCloneFromGroup, void(const QString& group));
    MOCK_METHOD0(slotCloneDeck, void());

    TrackPointer loadedTrack;
    ControlObject trackSamples;
    ControlObject samplerate;
    ControlObject rateratio;
    ControlLinPotmeter playposition;
    ControlPushButton play;
    ControlPushButton repeat;
    ControlObject introStartPos;
    ControlObject introEndPos;
    ControlObject outroStartPos;
    ControlObject outroEndPos;
    ControlObject orientation;
};

class MockPlayerManager : public PlayerManagerInterface {
  public:
    MockPlayerManager()
            : numDecks(ConfigKey(kAppGroup, QStringLiteral("num_decks")), true),
              numSamplers(ConfigKey(kAppGroup, QStringLiteral("num_samplers")), true),
              numPreviewDecks(ConfigKey(kAppGroup, QStringLiteral("num_preview_decks")),
                      true) {
    }

    MOCK_CONST_METHOD1(getPlayer, BaseTrackPlayer*(const QString&));
    MOCK_CONST_METHOD1(getPlayer, BaseTrackPlayer*(const ChannelHandle&));
    MOCK_CONST_METHOD1(getDeckBase, BaseTrackPlayer*(int));
    MOCK_CONST_METHOD1(getPreviewDeck, PreviewDeck*(int));
    MOCK_CONST_METHOD1(getSampler, Sampler*(int));

    int numberOfDecks() const override {
        return static_cast<int>(numDecks.get());
    }
    int numberOfSamplers() const override {
        return static_cast<int>(numSamplers.get());
    }
    int numberOfPreviewDecks() const override {
        return static_cast<int>(numPreviewDecks.get());
    }

    ControlObject numDecks;
    ControlObject numSamplers;
    ControlObject numPreviewDecks;
};

struct Load {
    QString location;
    QString group;
    bool play = false;
};

/// Records the loads instead of sending them to the decks.
class RecordingAutoDJProcessor : public AutoDJProcessor {
  public:
    using AutoDJProcessor::AutoDJProcessor;

    QList<Load> loads;

  protected:
    void emitLoadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play) override {
        loads.append({pTrack ? pTrack->getLocation() : QString(), group, play});
    }
    void emitAutoDJStateChanged(AutoDJProcessor::AutoDJState) override {
    }
};

class AutoDJCratePendingTest : public LibraryTest {
  protected:
    AutoDJCratePendingTest()
            : deck1(kDeck1, EngineChannel::LEFT),
              deck2(QStringLiteral("[Channel2]"), EngineChannel::RIGHT),
              deck3(QStringLiteral("[Channel3]"), EngineChannel::LEFT),
              deck4(QStringLiteral("[Channel4]"), EngineChannel::RIGHT) {
        PlaylistDAO& playlistDao = internalCollection()->getPlaylistDAO();
        int autoDjPlaylistId = playlistDao.getPlaylistIdFromName(AUTODJ_TABLE);
        if (autoDjPlaylistId < 0) {
            autoDjPlaylistId = playlistDao.createPlaylist(AUTODJ_TABLE, PlaylistDAO::PLHT_AUTO_DJ);
        }
        pPlayerManager.reset(new MockPlayerManager());
        PlayerInfo::create();
        ON_CALL(*pPlayerManager, getDeckBase(0)).WillByDefault(Return(&deck1));
        ON_CALL(*pPlayerManager, getDeckBase(1)).WillByDefault(Return(&deck2));
        ON_CALL(*pPlayerManager, getDeckBase(2)).WillByDefault(Return(&deck3));
        ON_CALL(*pPlayerManager, getDeckBase(3)).WillByDefault(Return(&deck4));
        EXPECT_CALL(*pPlayerManager, getDeckBase(::testing::_)).Times(::testing::AnyNumber());
        pPlayerManager->numDecks.set(4);
        pProcessor.reset(new RecordingAutoDJProcessor(nullptr,
                config(),
                pPlayerManager.data(),
                trackCollectionManager(),
                autoDjPlaylistId));
    }

    ~AutoDJCratePendingTest() override {
        m_pPrefetcher.reset();
        m_pCoordinator.reset();
        m_pCache.reset();
        AutoDJProcessor::setPendingTrackHook(nullptr);
        mixxx::Time::setTestMode(false);
        pProcessor.reset();
        PlayerInfo::destroy();
    }

    /// The application's audio cache, Auto DJ prefetcher (which answers the
    /// pending track hook) and load coordinator, for the tracks of kServer.
    void makeCrate(bool prefetch) {
        ASSERT_TRUE(m_cacheDir.isValid());
        crate::ClientOptions clientOptions;
        clientOptions.deviceFingerprint = QStringLiteral("install-1");
        clientOptions.schedule = m_scheduler.function();
        clientOptions.random = [] { return 0.5; };
        m_pClient = std::make_unique<crate::Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, clientOptions);
        m_pClient->setTokenSource(&m_tokens);
        crate::cache::AudioCache::Options cacheOptions;
        cacheOptions.directory = m_cacheDir.path();
        cacheOptions.clientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pClient.get() : nullptr;
        };
        cacheOptions.pStorageInfo = std::make_shared<FakeStorageInfo>();
        m_pCache = std::make_unique<crate::cache::AudioCache>(std::move(cacheOptions));
        ASSERT_TRUE(QTest::qWaitFor([this] { return m_pCache->isLoaded(); }, kWaitMs));

        crate::library::CratePrefetcher::Options prefetchOptions;
        prefetchOptions.pCache = m_pCache.get();
        prefetchOptions.pDbConnectionPool = dbConnectionPooler();
        prefetchOptions.pPlaylistDao = &internalCollection()->getPlaylistDAO();
        prefetchOptions.settleTime = std::chrono::milliseconds(1);
        prefetchOptions.enabled = prefetch;
        m_pPrefetcher =
                std::make_unique<crate::library::CratePrefetcher>(std::move(prefetchOptions));

        crate::library::CrateLoadCoordinator::Options coordinatorOptions;
        coordinatorOptions.pCache = m_pCache.get();
        coordinatorOptions.pConfig = config();
        coordinatorOptions.trackOfFile = [this](const QString& path) {
            return trackCollectionManager()->getOrAddTrack(TrackRef::fromFilePath(path));
        };
        coordinatorOptions.trackIdsOfFiles =
                crate::library::CrateLoadCoordinator::trackIdsOfFilesIn(trackCollectionManager());
        coordinatorOptions.trackById = [this](TrackId trackId) {
            return trackCollectionManager()->getTrackById(trackId);
        };
        m_pCoordinator = std::make_unique<crate::library::CrateLoadCoordinator>(
                std::move(coordinatorOptions));
    }

    /// The file the cache downloads the Crate track `uid` to.
    QString cratePath(const QString& uid) const {
        return m_cacheDir.filePath(kServer + QLatin1Char('/') + uid.left(2) + QLatin1Char('/') +
                uid + QStringLiteral(".mp3"));
    }

    /// Adds the uncached Crate track `uid` to the end of the queue, as "Add
    /// to Auto DJ" does.
    TrackId queueUncached(const QString& uid) {
        crate::library::CrateQueuedTrack queued;
        queued.track.serverId = kServer;
        queued.track.entityUid = uid;
        queued.track.hints.format = QStringLiteral("mp3");
        queued.tags.title = QStringLiteral("Track ") + uid;
        queued.tags.artist = QStringLiteral("Ondine Varga");
        queued.tags.durationMs = 300000;
        const QList<TrackId> ids = m_pCoordinator->trackIdsForAutoDj({queued});
        EXPECT_EQ(1, ids.size());
        if (ids.isEmpty()) {
            return TrackId();
        }
        pProcessor->getTableModel()->appendTrack(ids.first());
        return ids.first();
    }

    crate::test::FakeReply* downloadOf(const QString& uid) const {
        const QString path = QStringLiteral("/api/tracks/by-entity/%1/stream").arg(uid);
        for (const auto& sent : m_network.sent) {
            if (sent.request.url().path() == path && sent.pReply && !sent.pReply->isFinished()) {
                return sent.pReply.data();
            }
        }
        return nullptr;
    }

    /// A library track whose file is in the temporary directory.
    TrackId addTrack(const QString& name) {
        const QString path = m_dir.filePath(name);
        EXPECT_TRUE(QFile::copy(getTestDir().filePath(kTrackFile), path));
        const TrackPointer pTrack = getOrAddTrackByLocation(path);
        return pTrack ? pTrack->getId() : TrackId();
    }

    /// A library track whose file is not there (yet): a Crate track whose
    /// file the cache evicted or still downloads.
    TrackId addMissingTrack(const QString& name) {
        const TrackId id = addTrack(name);
        EXPECT_TRUE(QFile::remove(m_dir.filePath(name)));
        return id;
    }

    void arrive(const QString& name) {
        ASSERT_TRUE(QFile::copy(getTestDir().filePath(kTrackFile), m_dir.filePath(name)));
    }

    TrackId queueTop() const {
        PlaylistTableModel* pModel = pProcessor->getTableModel();
        return pModel->getTrackId(pModel->index(0, 0));
    }

    QTemporaryDir m_dir;
    QTemporaryDir m_cacheDir;
    crate::test::FakeNetworkAccessManager m_network;
    crate::test::FakeScheduler m_scheduler;
    crate::test::FakeTokenSource m_tokens;
    std::unique_ptr<crate::Client> m_pClient;
    std::unique_ptr<crate::cache::AudioCache> m_pCache;
    std::unique_ptr<crate::library::CrateLoadCoordinator> m_pCoordinator;
    std::unique_ptr<crate::library::CratePrefetcher> m_pPrefetcher;
    FakeMixer mixer;
    FakeDeck deck1;
    FakeDeck deck2;
    FakeDeck deck3;
    FakeDeck deck4;
    QScopedPointer<MockPlayerManager> pPlayerManager;
    QScopedPointer<RecordingAutoDJProcessor> pProcessor;
};

TEST_F(AutoDJCratePendingTest, WaitsForPendingCrateTrack) {
    const TrackId pending = addMissingTrack(QStringLiteral("pending.mp3"));
    const TrackId next = addTrack(QStringLiteral("next.mp3"));
    ASSERT_TRUE(pending.isValid());
    ASSERT_TRUE(next.isValid());
    const QString pendingPath = m_dir.filePath(QStringLiteral("pending.mp3"));
    int asked = 0;
    AutoDJProcessor::setPendingTrackHook([&asked, pendingPath](const TrackPointer& pTrack) {
        ++asked;
        return pTrack->getLocation() == pendingPath;
    });
    pProcessor->getTableModel()->appendTrack(pending);
    pProcessor->getTableModel()->appendTrack(next);

    EXPECT_EQ(AutoDJProcessor::ADJ_OK, pProcessor->toggleAutoDJ(true));

    // Nothing is loaded yet, and the track keeps its place
    EXPECT_GT(asked, 0);
    EXPECT_TRUE(pProcessor->loads.isEmpty());
    EXPECT_EQ(AutoDJProcessor::ADJ_ENABLE_P1LOADED, pProcessor->getState());
    EXPECT_EQ(pending, queueTop());
    QTest::qWait(600);
    EXPECT_TRUE(pProcessor->loads.isEmpty());

    // The download ends: the track is loaded and played
    arrive(QStringLiteral("pending.mp3"));
    ASSERT_TRUE(QTest::qWaitFor([this] { return !pProcessor->loads.isEmpty(); }, kWaitMs));
    ASSERT_EQ(1, pProcessor->loads.size());
    EXPECT_EQ(pendingPath, pProcessor->loads[0].location);
    EXPECT_EQ(kDeck1, pProcessor->loads[0].group);
    EXPECT_TRUE(pProcessor->loads[0].play);
    EXPECT_EQ(pending, queueTop());
}

TEST_F(AutoDJCratePendingTest, SkipsToNextAvailableAfterTimeout) {
    mixxx::Time::setTestMode(true);
    const TrackId pending = addMissingTrack(QStringLiteral("late.mp3"));
    const TrackId next = addTrack(QStringLiteral("next.mp3"));
    AutoDJProcessor::setPendingTrackHook([](const TrackPointer&) {
        return true;
    });
    pProcessor->getTableModel()->appendTrack(pending);
    pProcessor->getTableModel()->appendTrack(next);

    EXPECT_EQ(AutoDJProcessor::ADJ_OK, pProcessor->toggleAutoDJ(true));
    EXPECT_TRUE(pProcessor->loads.isEmpty());

    // The file does not come in time: the next available track plays
    mixxx::Time::addTestTime(std::chrono::milliseconds(
            AutoDJProcessor::kPendingTrackTimeout.toIntegerMillis() + 1000));
    ASSERT_TRUE(QTest::qWaitFor([this] { return !pProcessor->loads.isEmpty(); }, kWaitMs));
    ASSERT_EQ(1, pProcessor->loads.size());
    EXPECT_EQ(m_dir.filePath(QStringLiteral("next.mp3")), pProcessor->loads[0].location);
    EXPECT_EQ(kDeck1, pProcessor->loads[0].group);
    EXPECT_TRUE(pProcessor->loads[0].play);
    // The late one left the queue, as a missing track does
    EXPECT_EQ(next, queueTop());
}

TEST_F(AutoDJCratePendingTest, NonCrateBehaviourUnchanged) {
    const TrackId missing = addMissingTrack(QStringLiteral("missing.mp3"));
    const TrackId next = addTrack(QStringLiteral("next.mp3"));
    // A hook that knows nothing of the track, as for any non-Crate track
    AutoDJProcessor::setPendingTrackHook([](const TrackPointer&) {
        return false;
    });
    pProcessor->getTableModel()->appendTrack(missing);
    pProcessor->getTableModel()->appendTrack(next);

    EXPECT_EQ(AutoDJProcessor::ADJ_OK, pProcessor->toggleAutoDJ(true));

    // Skipped at once, as upstream does
    ASSERT_EQ(1, pProcessor->loads.size());
    EXPECT_EQ(m_dir.filePath(QStringLiteral("next.mp3")), pProcessor->loads[0].location);
    EXPECT_TRUE(pProcessor->loads[0].play);
    EXPECT_EQ(next, queueTop());

    // Without a hook too, and a queue of missing tracks only is empty
    EXPECT_EQ(AutoDJProcessor::ADJ_OK, pProcessor->toggleAutoDJ(false));
    AutoDJProcessor::setPendingTrackHook(nullptr);
    pProcessor->getTableModel()->removeTrack(pProcessor->getTableModel()->index(0, 0));
    pProcessor->getTableModel()->appendTrack(addMissingTrack(QStringLiteral("gone.mp3")));
    EXPECT_EQ(AutoDJProcessor::ADJ_QUEUE_EMPTY, pProcessor->toggleAutoDJ(true));
    EXPECT_FALSE(queueTop().isValid());
}

// "Add to Auto DJ" on an uncached Crate track (F4.C-PR3 decision 1, Diego,
// 2026-11-11): with the prefetch off, nothing downloads until the track's turn;
// then the hook starts its download with Auto DJ's priority and Auto DJ loads
// the track once its file is there.
TEST_F(AutoDJCratePendingTest, DownloadsQueuedUncachedTrackWhenItsTurnComes) {
    makeCrate(false);
    const TrackId uncached = queueUncached(QStringLiteral("e1"));
    ASSERT_TRUE(uncached.isValid());
    const TrackId next = addTrack(QStringLiteral("next.mp3"));
    pProcessor->getTableModel()->appendTrack(next);
    QTest::qWait(50);
    EXPECT_EQ(nullptr, downloadOf(QStringLiteral("e1")));

    EXPECT_EQ(AutoDJProcessor::ADJ_OK, pProcessor->toggleAutoDJ(true));

    // Its turn: Auto DJ waits for its file, which downloads now
    EXPECT_TRUE(pProcessor->loads.isEmpty());
    EXPECT_EQ(uncached, queueTop());
    ASSERT_TRUE(QTest::qWaitFor([this] { return downloadOf(QStringLiteral("e1")); }, kWaitMs));
    const QList<crate::cache::DownloadInfo> downloads = m_pCache->downloads();
    ASSERT_EQ(1, downloads.size());
    EXPECT_EQ(crate::cache::Priority::AutoDj, downloads[0].priority);
    EXPECT_TRUE(pProcessor->loads.isEmpty());

    QFile audio(getTestDir().filePath(kTrackFile));
    ASSERT_TRUE(audio.open(QIODevice::ReadOnly));
    const QByteArray bytes = audio.readAll();
    downloadOf(QStringLiteral("e1"))
            ->respond(200,
                    bytes,
                    {{QByteArrayLiteral("Content-Length"), QByteArray::number(bytes.size())},
                            {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/mpeg")}});
    ASSERT_TRUE(QTest::qWaitFor([this] { return !pProcessor->loads.isEmpty(); }, kWaitMs));
    ASSERT_EQ(1, pProcessor->loads.size());
    EXPECT_EQ(cratePath(QStringLiteral("e1")), pProcessor->loads[0].location);
    EXPECT_EQ(kDeck1, pProcessor->loads[0].group);
    EXPECT_TRUE(pProcessor->loads[0].play);
    EXPECT_EQ(uncached, queueTop());
    // The queued library track is the cached one
    const TrackPointer pCached = m_pCoordinator->cachedTrack(kServer, QStringLiteral("e1"));
    ASSERT_NE(nullptr, pCached);
    EXPECT_EQ(uncached, pCached->getId());
}

// The file of a queued uncached track does not come in time: Auto DJ goes on
// with the next track after kPendingTrackTimeout, as for any late Crate
// track.
TEST_F(AutoDJCratePendingTest, SkipsQueuedUncachedTrackAfterTimeout) {
    mixxx::Time::setTestMode(true);
    makeCrate(true);
    const TrackId uncached = queueUncached(QStringLiteral("e1"));
    ASSERT_TRUE(uncached.isValid());
    const TrackId next = addTrack(QStringLiteral("next.mp3"));
    pProcessor->getTableModel()->appendTrack(next);
    // The prefetch asks for it as soon as it is queued
    ASSERT_TRUE(QTest::qWaitFor([this] { return downloadOf(QStringLiteral("e1")); }, kWaitMs));

    EXPECT_EQ(AutoDJProcessor::ADJ_OK, pProcessor->toggleAutoDJ(true));
    EXPECT_TRUE(pProcessor->loads.isEmpty());
    EXPECT_EQ(uncached, queueTop());

    // The server never answers
    mixxx::Time::addTestTime(std::chrono::milliseconds(
            AutoDJProcessor::kPendingTrackTimeout.toIntegerMillis() + 1000));
    ASSERT_TRUE(QTest::qWaitFor([this] { return !pProcessor->loads.isEmpty(); }, kWaitMs));
    ASSERT_EQ(1, pProcessor->loads.size());
    EXPECT_EQ(m_dir.filePath(QStringLiteral("next.mp3")), pProcessor->loads[0].location);
    EXPECT_TRUE(pProcessor->loads[0].play);
    EXPECT_EQ(next, queueTop());
}

} // namespace crate_autodj_pending
