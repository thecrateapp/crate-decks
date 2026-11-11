// Auto DJ's pending track hook (plan F4.C-PR3), on the fixture of upstream's
// autodjprocessor_test.cpp: Crate tracks whose file is still downloading stay
// in the queue for a while instead of being skipped as missing.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QFile>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>

#include "control/controllinpotmeter.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "library/autodj/autodjprocessor.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/playlisttablemodel.h"
#include "library/trackcollection.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "test/librarytest.h"
#include "track/track.h"
#include "util/time.h"

// Its own namespace: upstream's test defines classes of the same names
namespace crate_autodj_pending {

using ::testing::Return;

const QString kTrackFile = QStringLiteral("id3-test-data/cover-test-øé~ł€˚-png.mp3");
const QString kAppGroup = QStringLiteral("[App]");
const QString kDeck1 = QStringLiteral("[Channel1]");
constexpr int kWaitMs = 5000;

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
        AutoDJProcessor::setPendingTrackHook(nullptr);
        mixxx::Time::setTestMode(false);
        pProcessor.reset();
        PlayerInfo::destroy();
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

} // namespace crate_autodj_pending
