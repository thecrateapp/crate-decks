// Auto DJ's Crate Smart Mix mode (plan F6.C-PR3), on the fixture of
// upstream's autodjprocessor_test.cpp: a TransitionDirector plays the
// transitions of TransitionMode::CrateSmartMix in Auto DJ's place, and Auto
// DJ keeps the queue, the loads and the ejects. Without a director, and in
// every other mode, Auto DJ does what it always did. The second half drives
// the real adapter, plan client and Smart Mix director with the network,
// the clock and the engine positions faked.
//
// TEST_GAP: how the real engine plays these transitions (start deviation,
// beat phase, levels) is measured on rendered transitions by G4's metrics.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <cmath>
#include <optional>
#include <tuple>

#include "control/controllinpotmeter.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "crate/client/crateclient.h"
#include "crate/cratepreferences.h"
#include "crate/smartmix/autodjsmartmixadapter.h"
#include "crate/smartmix/planclient.h"
#include "crate/smartmix/smartmixdirector.h"
#include "library/autodj/autodjprocessor.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/playlisttablemodel.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "sources/soundsourceproxy.h"
#include "test/crate/fakecratenetwork.h"
#include "test/librarytest.h"
#include "track/track.h"

// Its own namespace: upstream's test defines classes of the same names
namespace crate_autodj_smartmix {

using ::testing::Return;
using TransitionMode = AutoDJProcessor::TransitionMode;

const QString kAppGroup = QStringLiteral("[App]");
const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");
const QStringList kTrackFiles = {QStringLiteral("id3-test-data/artist.mp3"),
        QStringLiteral("id3-test-data/all.mp3"),
        QStringLiteral("id3-test-data/cover-test-øé~ł€˚-png.mp3")};
/// Every track lasts 100 s, so a position of 0.01 is one second.
constexpr double kTrackSeconds = 100.0;
/// How far the director takes the transition before Auto DJ's own fade.
constexpr double kLead = AutoDJProcessor::kTransitionDirectorLeadSeconds / kTrackSeconds;
constexpr double kStep = 0.005;

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
              orientation(ConfigKey(group, "orientation")),
              pregain(ConfigKey(group, "pregain"), true, false, false, 1.0),
              volume(ConfigKey(group, "volume"), true, false, false, 1.0) {
        play.setButtonMode(mixxx::control::ButtonMode::Toggle);
        repeat.setButtonMode(mixxx::control::ButtonMode::Toggle);
        outroStartPos.set(Cue::kNoPosition);
        outroEndPos.set(Cue::kNoPosition);
        orientation.set(orient);
    }

    /// The deck loads `pTrack` and says so, as the engine would.
    void load(const TrackPointer& pTrack, bool bPlay) {
        emit loadingTrack(pTrack, loadedTrack);
        loadedTrack = pTrack;
        PlayerInfo::instance().setTrackInfo(getGroup(), pTrack);
        play.set(bPlay);
        trackSamples.set(pTrack->getDuration() * pTrack->getSampleRate() * 2);
        samplerate.set(pTrack->getSampleRate());
        emit newTrackLoaded(pTrack);
    }

    void eject() {
        const TrackPointer pTrack = loadedTrack;
        play.set(0.0);
        emit loadingTrack(TrackPointer(), pTrack);
        loadedTrack.reset();
        PlayerInfo::instance().setTrackInfo(getGroup(), TrackPointer());
        emit playerEmpty();
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
        load(pTrack, bPlay);
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
    // PlayerInfo reads them to tell the deck on air
    ControlObject pregain;
    ControlObject volume;
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
    TrackPointer pTrack;
    QString group;
    bool play = false;
};

/// Records the loads instead of sending them to the decks, and the states.
class RecordingAutoDJProcessor : public AutoDJProcessor {
  public:
    using AutoDJProcessor::AutoDJProcessor;

    QList<Load> loads;
    QList<AutoDJState> states;

  protected:
    void emitLoadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play) override {
        loads.append({pTrack, group, play});
    }
    void emitAutoDJStateChanged(AutoDJProcessor::AutoDJState state) override {
        states.append(state);
    }
};

/// A director that answers as told and records what Auto DJ asks it.
class FakeDirector : public AutoDJProcessor::TransitionDirector {
  public:
    bool startTransition(AutoDJProcessor* pProcessor,
            const QString& fromGroup,
            const QString& toGroup) override {
        starts.append({fromGroup, toGroup});
        processor = pProcessor;
        return takes;
    }
    bool fadeNow() override {
        ++fadeNows;
        return playsFadeNow;
    }
    void cancelTransition() override {
        ++cancels;
    }

    bool takes = true;
    bool playsFadeNow = false;
    QList<std::pair<QString, QString>> starts;
    AutoDJProcessor* processor = nullptr;
    int fadeNows = 0;
    int cancels = 0;
};

class AutoDJSmartMixTest : public LibraryTest {
  protected:
    AutoDJSmartMixTest()
            : deck1(kDeck1, EngineChannel::LEFT),
              deck2(kDeck2, EngineChannel::RIGHT),
              deck3(QStringLiteral("[Channel3]"), EngineChannel::LEFT),
              deck4(QStringLiteral("[Channel4]"), EngineChannel::RIGHT) {
        PlaylistDAO& playlistDao = internalCollection()->getPlaylistDAO();
        m_autoDjPlaylistId = playlistDao.getPlaylistIdFromName(AUTODJ_TABLE);
        if (m_autoDjPlaylistId < 0) {
            m_autoDjPlaylistId =
                    playlistDao.createPlaylist(AUTODJ_TABLE, PlaylistDAO::PLHT_AUTO_DJ);
        }
        pPlayerManager.reset(new MockPlayerManager());
        PlayerInfo::create();
        ON_CALL(*pPlayerManager, getDeckBase(0)).WillByDefault(Return(&deck1));
        ON_CALL(*pPlayerManager, getDeckBase(1)).WillByDefault(Return(&deck2));
        ON_CALL(*pPlayerManager, getDeckBase(2)).WillByDefault(Return(&deck3));
        ON_CALL(*pPlayerManager, getDeckBase(3)).WillByDefault(Return(&deck4));
        EXPECT_CALL(*pPlayerManager, getDeckBase(::testing::_)).Times(::testing::AnyNumber());
        pPlayerManager->numDecks.set(4);
        for (const QString& file : kTrackFiles) {
            const TrackPointer pTrack = getOrAddTrackByLocation(getTestDir().filePath(file));
            EXPECT_TRUE(pTrack);
            if (pTrack) {
                SoundSourceProxy(pTrack).updateTrackFromSource(
                        SoundSourceProxy::UpdateTrackFromSourceMode::Always,
                        SyncTrackMetadataParams{});
                pTrack->setDuration(kTrackSeconds);
                m_tracks.append(pTrack);
            }
        }
        makeProcessor();
    }

    ~AutoDJSmartMixTest() override {
        AutoDJProcessor::setTransitionDirector(nullptr);
        pProcessor.reset();
        PlayerInfo::destroy();
    }

    void makeProcessor() {
        // The old one first: its controls go with it
        pProcessor.reset();
        pProcessor.reset(new RecordingAutoDJProcessor(nullptr,
                config(),
                pPlayerManager.data(),
                trackCollectionManager(),
                m_autoDjPlaylistId));
    }

    PlaylistTableModel* queue() const {
        return pProcessor->getTableModel();
    }

    QList<TrackId> queuedIds() const {
        QList<TrackId> ids;
        for (int row = 0; row < queue()->rowCount(); ++row) {
            ids.append(queue()->getTrackId(queue()->index(row, 0)));
        }
        return ids;
    }

    /// Deck 1 plays the first track; the queue holds the other two; Auto DJ
    /// is enabled in `mode` and has loaded the queue's first track into deck
    /// 2, which is ready.
    void startAutoDJ(TransitionMode mode) {
        mixer.crossfader.set(-1.0);
        deck1.load(m_tracks[0], true);
        deck1.playposition.set(0.0);
        queue()->appendTrack(m_tracks[1]->getId());
        queue()->appendTrack(m_tracks[2]->getId());
        pProcessor->setTransitionMode(mode);
        ASSERT_EQ(AutoDJProcessor::ADJ_OK, pProcessor->toggleAutoDJ(true));
        ASSERT_EQ(AutoDJProcessor::ADJ_IDLE, pProcessor->getState());
        ASSERT_EQ(1, pProcessor->loads.size());
        EXPECT_EQ(kDeck2, pProcessor->loads.last().group);
        EXPECT_FALSE(pProcessor->loads.last().play);
        deck2.load(pProcessor->loads.last().pTrack, false);
    }

    /// Plays deck 1 on, a step at a time, until `done` or the end of its
    /// track; the position where it stopped, or nullopt at the end.
    /// The positions are steps of kStep, the same in every run.
    template<typename Done>
    std::optional<double> playDeck1Until(Done done, double limit = 0.99) {
        for (long step = std::lround(deck1.playposition.get() / kStep) + 1;
                static_cast<double>(step) * kStep <= limit + 1e-9;
                ++step) {
            const double position = static_cast<double>(step) * kStep;
            deck1.playposition.set(position);
            if (done()) {
                return position;
            }
        }
        return std::nullopt;
    }

    bool fading() const {
        return pProcessor->getState() == AutoDJProcessor::ADJ_LEFT_FADING;
    }

    /// Where Auto DJ's own transition from deck 1 starts in `mode`, without a
    /// director; Auto DJ is disabled again afterwards and the decks and the
    /// queue are reset.
    double ownFadeStart(TransitionMode mode) {
        startAutoDJ(mode);
        const auto start = playDeck1Until([this] { return fading(); });
        EXPECT_TRUE(start.has_value());
        reset();
        return start.value_or(-1.0);
    }

    void reset() {
        pProcessor->toggleAutoDJ(false);
        for (FakeDeck* pDeck : {&deck1, &deck2}) {
            pDeck->play.set(0.0);
            pDeck->playposition.set(0.0);
        }
        QModelIndexList rows;
        for (int row = 0; row < queue()->rowCount(); ++row) {
            rows.append(queue()->index(row, 0));
        }
        queue()->removeTracks(rows);
        makeProcessor();
    }

    /// What the director does when it plays a transition: it starts deck 2,
    /// puts the crossfader on it and stops deck 1.
    void directorPlays() {
        deck2.play.set(1.0);
        mixer.crossfader.set(1.0);
        deck1.play.set(0.0);
        deck2.playposition.set(0.05);
    }

    FakeMixer mixer;
    FakeDeck deck1;
    FakeDeck deck2;
    FakeDeck deck3;
    FakeDeck deck4;
    FakeDirector director;
    QScopedPointer<MockPlayerManager> pPlayerManager;
    int m_autoDjPlaylistId = -1;
    QList<TrackPointer> m_tracks;
    QScopedPointer<RecordingAutoDJProcessor> pProcessor;
};

// The director takes the transition 30 s before Auto DJ's own fade; Auto DJ
// then neither starts the incoming deck nor moves the crossfader, waits in
// FADING until the outgoing deck stops, and goes on as always.
TEST_F(AutoDJSmartMixTest, DelegatesTransitionToDirector) {
    const double ownStart = ownFadeStart(TransitionMode::FullIntroOutro);
    AutoDJProcessor::setTransitionDirector(&director);
    startAutoDJ(TransitionMode::CrateSmartMix);

    const auto handover = playDeck1Until([this] { return !director.starts.isEmpty(); });
    ASSERT_TRUE(handover.has_value());
    EXPECT_NEAR(*handover, ownStart - kLead, kStep + 1e-9);
    ASSERT_EQ(1, director.starts.size());
    EXPECT_EQ(kDeck1, director.starts.first().first);
    EXPECT_EQ(kDeck2, director.starts.first().second);
    EXPECT_EQ(pProcessor.data(), director.processor);
    EXPECT_TRUE(fading());

    // Past Auto DJ's own fade, nothing moves but the outgoing deck
    playDeck1Until([] { return false; }, ownStart + 0.05);
    EXPECT_TRUE(fading());
    EXPECT_EQ(0.0, deck2.play.get());
    EXPECT_EQ(-1.0, mixer.crossfader.get());
    EXPECT_EQ(1.0, deck1.play.get());
    EXPECT_EQ(1, director.starts.size());
    // The incoming track leaves the queue only once it is on air
    EXPECT_EQ(2, queuedIds().size());
    EXPECT_EQ(m_tracks[1]->getId(), queuedIds().first());

    directorPlays();
    EXPECT_EQ(AutoDJProcessor::ADJ_IDLE, pProcessor->getState());
    EXPECT_EQ(1.0, mixer.crossfader.get());
    EXPECT_EQ(QList<TrackId>{m_tracks[2]->getId()}, queuedIds());
    ASSERT_EQ(2, pProcessor->loads.size());
    EXPECT_EQ(kDeck1, pProcessor->loads.last().group);
    EXPECT_EQ(m_tracks[2], pProcessor->loads.last().pTrack);
    EXPECT_FALSE(pProcessor->loads.last().play);
}

// Without a director the mode is FullIntroOutro; with one that does not take
// the transition, Auto DJ makes its own at its own fade point.
TEST_F(AutoDJSmartMixTest, DeclinedTransitionStartsAtAutoDJsFadePoint) {
    const double ownStart = ownFadeStart(TransitionMode::FullIntroOutro);

    startAutoDJ(TransitionMode::CrateSmartMix);
    const auto withoutDirector = playDeck1Until([this] { return fading(); });
    ASSERT_TRUE(withoutDirector.has_value());
    EXPECT_NEAR(*withoutDirector, ownStart, 1e-9);
    reset();

    AutoDJProcessor::setTransitionDirector(&director);
    director.takes = false;
    startAutoDJ(TransitionMode::CrateSmartMix);
    const auto offered = playDeck1Until([this] { return !director.starts.isEmpty(); });
    ASSERT_TRUE(offered.has_value());
    EXPECT_NEAR(*offered, ownStart - kLead, kStep + 1e-9);
    EXPECT_EQ(AutoDJProcessor::ADJ_IDLE, pProcessor->getState());
    const auto own = playDeck1Until([this] { return fading(); });
    ASSERT_TRUE(own.has_value());
    EXPECT_NEAR(*own, ownStart, 1e-9);
    EXPECT_EQ(1.0, deck2.play.get());
    EXPECT_EQ(1, director.starts.size());
}

// The director gives the transition back when no plan came (or it could not
// play one): Auto DJ makes its FullIntroOutro transition at its own fade
// point, as if Smart Mix had not been there.
TEST_F(AutoDJSmartMixTest, FallsBackToFullIntroOutroWithoutPlan) {
    startAutoDJ(TransitionMode::FullIntroOutro);
    const auto ownStart = playDeck1Until([this] { return fading(); });
    ASSERT_TRUE(ownStart.has_value());
    const auto ownCrossfader = [this] {
        playDeck1Until([] { return false; }, deck1.playposition.get() + 0.03);
        return mixer.crossfader.get();
    }();
    reset();

    AutoDJProcessor::setTransitionDirector(&director);
    startAutoDJ(TransitionMode::CrateSmartMix);
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    ASSERT_EQ(1, director.starts.size());
    playDeck1Until([] { return false; }, deck1.playposition.get() + 0.03);

    director.processor->handBackTransition();
    EXPECT_EQ(AutoDJProcessor::ADJ_IDLE, pProcessor->getState());
    EXPECT_EQ(0.0, deck2.play.get());
    const auto start = playDeck1Until([this] { return fading(); });
    ASSERT_TRUE(start.has_value());
    EXPECT_NEAR(*start, *ownStart, 1e-9);
    EXPECT_EQ(1.0, deck2.play.get());
    playDeck1Until([] { return false; }, deck1.playposition.get() + 0.03);
    EXPECT_NEAR(ownCrossfader, mixer.crossfader.get(), 1e-9);
    EXPECT_EQ(1, director.starts.size());
    // The incoming track left the queue once, at Auto DJ's own fade
    EXPECT_EQ(QList<TrackId>{m_tracks[2]->getId()}, queuedIds());
    // A second hand-back means nothing
    director.processor->handBackTransition();
    EXPECT_TRUE(fading());
}

/// The run of QueueAndEjectBehaviourUnchanged: Auto DJ's own FullIntroOutro
/// without a director, or the director's transition.
class AutoDJSmartMixQueueTest : public AutoDJSmartMixTest,
                                public ::testing::WithParamInterface<bool> {};

// The same loads, queue and ejects whether Auto DJ or the director plays the
// transition.
TEST_P(AutoDJSmartMixQueueTest, QueueAndEjectBehaviourUnchanged) {
    const bool withDirector = GetParam();
    if (withDirector) {
        AutoDJProcessor::setTransitionDirector(&director);
    }
    startAutoDJ(withDirector ? TransitionMode::CrateSmartMix : TransitionMode::FullIntroOutro);
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    if (withDirector) {
        directorPlays();
    } else {
        // The engine stops the outgoing deck at its end
        playDeck1Until([] { return false; }, 1.0);
        deck1.play.set(0.0);
        deck2.playposition.set(0.05);
    }
    ASSERT_EQ(AutoDJProcessor::ADJ_IDLE, pProcessor->getState());
    EXPECT_EQ(QList<TrackId>{m_tracks[2]->getId()}, queuedIds());
    ASSERT_EQ(2, pProcessor->loads.size());
    EXPECT_EQ(kDeck1, pProcessor->loads[1].group);
    EXPECT_EQ(m_tracks[2], pProcessor->loads[1].pTrack);

    // The DJ ejects the track Auto DJ loaded next: it leaves the queue, and
    // with nothing left Auto DJ empties the deck and stops.
    deck1.load(m_tracks[2], false);
    deck1.eject();
    EXPECT_TRUE(queuedIds().isEmpty());
    EXPECT_EQ(AutoDJProcessor::ADJ_DISABLED, pProcessor->getState());
    ASSERT_EQ(3, pProcessor->loads.size());
    EXPECT_EQ(kDeck1, pProcessor->loads[2].group);
    EXPECT_FALSE(pProcessor->loads[2].pTrack);
    EXPECT_EQ(0, director.cancels);
}

INSTANTIATE_TEST_SUITE_P(AutoDJSmartMix,
        AutoDJSmartMixQueueTest,
        ::testing::Values(false, true),
        [](const ::testing::TestParamInfo<bool>& info) {
            return info.param ? std::string("Director") : std::string("FullIntroOutro");
        });

/// The five upstream modes.
class AutoDJSmartMixModesTest : public AutoDJSmartMixTest,
                                public ::testing::WithParamInterface<TransitionMode> {};

// A director changes nothing in the other modes: it is never asked, and the
// fade starts and moves the crossfader as without one.
TEST_P(AutoDJSmartMixModesTest, OtherModesUnchanged) {
    const TransitionMode mode = GetParam();
    const auto run = [this, mode] {
        startAutoDJ(mode);
        const auto start = playDeck1Until([this] { return fading(); });
        QList<double> crossfader;
        for (int i = 0; i < 4 && start; ++i) {
            playDeck1Until([] { return false; }, deck1.playposition.get() + 0.01);
            crossfader.append(mixer.crossfader.get());
        }
        const QList<AutoDJProcessor::AutoDJState> states = pProcessor->states;
        reset();
        return std::make_tuple(start, crossfader, states);
    };
    const auto without = run();
    AutoDJProcessor::setTransitionDirector(&director);
    const auto with = run();
    EXPECT_TRUE(std::get<0>(without).has_value());
    EXPECT_EQ(std::get<0>(without), std::get<0>(with));
    EXPECT_EQ(std::get<1>(without), std::get<1>(with));
    EXPECT_EQ(std::get<2>(without), std::get<2>(with));
    EXPECT_TRUE(director.starts.isEmpty());
}

INSTANTIATE_TEST_SUITE_P(AutoDJSmartMix,
        AutoDJSmartMixModesTest,
        ::testing::Values(TransitionMode::FullIntroOutro,
                TransitionMode::FadeAtOutroStart,
                TransitionMode::FixedFullTrack,
                TransitionMode::FixedSkipSilence,
                TransitionMode::FixedStartCenterSkipSilence),
        [](const ::testing::TestParamInfo<TransitionMode>& info) {
            switch (info.param) {
            case TransitionMode::FullIntroOutro:
                return std::string("FullIntroOutro");
            case TransitionMode::FadeAtOutroStart:
                return std::string("FadeAtOutroStart");
            case TransitionMode::FixedFullTrack:
                return std::string("FixedFullTrack");
            case TransitionMode::FixedSkipSilence:
                return std::string("FixedSkipSilence");
            case TransitionMode::FixedStartCenterSkipSilence:
                return std::string("FixedStartCenterSkipSilence");
            case TransitionMode::CrateSmartMix:
                break;
            }
            return std::string("CrateSmartMix");
        });

// Decision 1 (Diego, 2026-11-15): "Fade now" before the director's
// transition starts never starts it early; Auto DJ fades now itself.
TEST_F(AutoDJSmartMixTest, FadeNowBeforeTheDirectorStartsIsAutoDJs) {
    AutoDJProcessor::setTransitionDirector(&director);
    startAutoDJ(TransitionMode::CrateSmartMix);
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());

    pProcessor->fadeNow();
    EXPECT_EQ(1, director.fadeNows);
    EXPECT_EQ(AutoDJProcessor::ADJ_IDLE, pProcessor->getState());
    playDeck1Until([this] { return fading(); });
    EXPECT_TRUE(fading());
    EXPECT_EQ(1.0, deck2.play.get());
    const double crossfader = mixer.crossfader.get();
    playDeck1Until([] { return false; }, deck1.playposition.get() + 0.03);
    EXPECT_GT(mixer.crossfader.get(), crossfader);
    EXPECT_EQ(1, director.starts.size());
}

// "Fade now" in IDLE, before the director was asked, is Auto DJ's too.
TEST_F(AutoDJSmartMixTest, FadeNowBeforeTheHandoverIsAutoDJs) {
    AutoDJProcessor::setTransitionDirector(&director);
    startAutoDJ(TransitionMode::CrateSmartMix);
    deck1.playposition.set(0.2);
    pProcessor->fadeNow();
    EXPECT_EQ(0, director.fadeNows);
    deck1.playposition.set(0.2 + kStep);
    EXPECT_TRUE(fading());
    EXPECT_EQ(1.0, deck2.play.get());
    EXPECT_TRUE(director.starts.isEmpty());
}

// While the director plays its transition, "Fade now" is its own to play.
TEST_F(AutoDJSmartMixTest, FadeNowWhileTheDirectorPlaysIsTheDirectors) {
    AutoDJProcessor::setTransitionDirector(&director);
    director.playsFadeNow = true;
    startAutoDJ(TransitionMode::CrateSmartMix);
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());

    pProcessor->fadeNow();
    EXPECT_EQ(1, director.fadeNows);
    EXPECT_TRUE(fading());
    playDeck1Until([] { return false; }, deck1.playposition.get() + 0.05);
    EXPECT_EQ(0.0, deck2.play.get());
    EXPECT_EQ(-1.0, mixer.crossfader.get());
}

TEST_F(AutoDJSmartMixTest, DisablingAutoDJCancelsTheTransition) {
    AutoDJProcessor::setTransitionDirector(&director);
    startAutoDJ(TransitionMode::CrateSmartMix);
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());

    pProcessor->toggleAutoDJ(false);
    EXPECT_EQ(1, director.cancels);
    EXPECT_EQ(AutoDJProcessor::ADJ_DISABLED, pProcessor->getState());
    // Nothing of the old transition is handed back later
    pProcessor->handBackTransition();
    EXPECT_EQ(AutoDJProcessor::ADJ_DISABLED, pProcessor->getState());
}

// A new track on the incoming deck while the director waits for its cue is
// not started by Auto DJ: the director lets go and hands the transition back.
TEST_F(AutoDJSmartMixTest, IncomingTrackChangeIsLeftToTheDirector) {
    AutoDJProcessor::setTransitionDirector(&director);
    startAutoDJ(TransitionMode::CrateSmartMix);
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());

    ASSERT_EQ(AutoDJProcessor::ADJ_OK, pProcessor->skipNext());
    ASSERT_EQ(2, pProcessor->loads.size());
    EXPECT_EQ(kDeck2, pProcessor->loads.last().group);
    deck2.load(pProcessor->loads.last().pTrack, false);
    EXPECT_EQ(0.0, deck2.play.get());
    EXPECT_TRUE(fading());

    director.processor->handBackTransition();
    EXPECT_EQ(AutoDJProcessor::ADJ_IDLE, pProcessor->getState());
    EXPECT_EQ(QList<TrackId>{m_tracks[2]->getId()}, queuedIds());
}

// The real adapter, plan client and director.

const QString kServer = QStringLiteral("srv-1");
// The anonymised uids of the real plan fixtures (src/test/crate/data/transition-plans/)
const QStringList kUids = {QStringLiteral("637bd7c0-380e-5bc7-b841-dc2234cdfaf7"),
        QStringLiteral("34137088-fcab-5f93-8866-31f1bf6d0871"),
        QStringLiteral("a89833d1-75e5-52ce-8766-6df5b057d027")};
const QList<double> kSeconds = {427.298, 148.052, 301.826};
const QString kPlansPath = QStringLiteral("/api/playback/transition-plans");
constexpr double kTickMs = crate::SmartMixDirector::kTickMs;

/// The clock, and the engine positions and tracks of the fake decks.
class FakeEnvironment : public crate::SmartMixEnvironment {
  public:
    double nowMs() const override {
        return now;
    }
    double enginePlayPosition(const QString& group) const override {
        return ControlObject::get(ConfigKey(group, QStringLiteral("playposition")));
    }
    QString trackKey(const QString& group) const override {
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
        return pTrack ? pTrack->getLocation() : QString();
    }

    double now = 1000.0;
};

class AutoDJSmartMixAdapterTest : public AutoDJSmartMixTest {
  protected:
    AutoDJSmartMixAdapterTest()
            : duration1(ConfigKey(kDeck1, QStringLiteral("duration"))),
              duration2(ConfigKey(kDeck2, QStringLiteral("duration"))),
              keylock1(ConfigKey(kDeck1, QStringLiteral("keylock"))),
              keylock2(ConfigKey(kDeck2, QStringLiteral("keylock"))) {
        for (int i = 0; i < m_tracks.size(); ++i) {
            m_tracks[i]->setDuration(kSeconds[i]);
            m_crateFiles.append(m_tracks[i]->getLocation());
        }
        crate::ClientOptions clientOptions;
        clientOptions.deviceFingerprint = QStringLiteral("install-1");
        m_pClient = std::make_unique<crate::Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, clientOptions);
        m_pClient->setTokenSource(&m_tokens);
        crate::PlanClient::Options planOptions;
        planOptions.schedule = m_scheduler.function();
        m_pPlans = std::make_unique<crate::PlanClient>(m_pClient.get(), std::move(planOptions));
    }

    ~AutoDJSmartMixAdapterTest() override {
        m_pAdapter.reset();
        m_pPlans.reset();
        m_pClient.reset();
    }

    void makeAdapter(bool defaultEq = true) {
        crate::AutoDJSmartMixAdapter::Options options;
        options.pConfig = config();
        options.pPlaylistDao = &internalCollection()->getPlaylistDAO();
        options.pDbConnectionPool = dbConnectionPooler();
        options.pPreferences = &m_preferences;
        options.trackOfFile =
                [this](const QString& location) -> std::optional<crate::cache::TrackKey> {
            const int index = static_cast<int>(m_crateFiles.indexOf(location));
            if (index < 0) {
                return std::nullopt;
            }
            return crate::cache::TrackKey{kServer, kUids[index]};
        };
        options.planClientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pPlans.get() : nullptr;
        };
        options.capabilitiesOf =
                [](const QString&) -> std::optional<crate::Capabilities> {
            const auto capabilities = crate::Capabilities::fromJson(
                    QJsonDocument::fromJson(R"({"decks": {"available": true,
                        "min_client_version": "0.1.0", "contract_version": 1,
                        "features": ["beatmatch", "bass_handoff", "long_beatmatch"]}})"));
            return capabilities.value();
        };
        options.defaultEqOn = [this, defaultEq](const QStringList& decks) {
            m_eqDecks = decks;
            return defaultEq;
        };
        auto pEnvironment = std::make_unique<FakeEnvironment>();
        m_pEnvironment = pEnvironment.get();
        options.pDirector = std::make_unique<crate::SmartMixDirector>(std::move(pEnvironment));
        options.settleTime = std::chrono::milliseconds(1);
        m_pAdapter = std::make_unique<crate::AutoDJSmartMixAdapter>(std::move(options));
    }

    crate::SmartMixDirector* director() const {
        return m_pAdapter->director();
    }

    /// Auto DJ in Crate Smart Mix with the real adapter, deck 2 ready.
    void startSmartMix() {
        startAutoDJ(TransitionMode::CrateSmartMix);
        duration1.set(kSeconds[0]);
        duration2.set(kSeconds[1]);
    }

    QList<crate::test::FakeNetworkAccessManager::Sent> planRequests() const {
        QList<crate::test::FakeNetworkAccessManager::Sent> requests;
        for (const auto& sent : m_network.sent) {
            if (sent.request.url().path() == kPlansPath) {
                requests.append(sent);
            }
        }
        return requests;
    }

    static QJsonObject bodyOf(const crate::test::FakeNetworkAccessManager::Sent& sent) {
        return QJsonDocument::fromJson(sent.body).object();
    }

    QJsonObject fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/transition-plans/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    /// Answers the open plan requests with `name` for every edge.
    void answerPlans(const QString& name = QStringLiteral("real-beatmatch.json")) {
        const QJsonObject realPlan = fixture(name);
        for (const auto& sent : std::as_const(m_network.sent)) {
            if (sent.request.url().path() != kPlansPath || !sent.pReply ||
                    sent.pReply->isFinished()) {
                continue;
            }
            QJsonArray plans;
            const QJsonArray edges = bodyOf(sent).value(QStringLiteral("edges")).toArray();
            for (const QJsonValue& edge : edges) {
                QJsonObject plan = realPlan;
                for (const QString& key : {QStringLiteral("outgoingTrackEntityUid"),
                             QStringLiteral("incomingTrackEntityUid")}) {
                    plan.insert(key, edge.toObject().value(key));
                }
                plans.append(plan);
            }
            sent.pReply->respond(200,
                    QJsonDocument(QJsonObject{
                                          {QStringLiteral("plannerVersion"),
                                                  QStringLiteral("smart-mix-v3")},
                                          {QStringLiteral("plans"), plans},
                                  })
                            .toJson());
        }
    }

    /// Plays the decks on for `ms`, as the engine would, and ticks the
    /// director at 60 Hz.
    void play(double ms) {
        for (double elapsed = 0.0; elapsed < ms; elapsed += kTickMs) {
            m_pEnvironment->now += kTickMs;
            for (FakeDeck* pDeck : {&deck1, &deck2}) {
                const double seconds = pDeck == &deck1 ? kSeconds[0] : kSeconds[1];
                if (pDeck->play.get() > 0.5) {
                    pDeck->playposition.set(std::min(1.0,
                            pDeck->playposition.get() +
                                    kTickMs * pDeck->rateratio.get() / (seconds * 1000.0)));
                }
            }
            director()->tick();
            QCoreApplication::processEvents();
        }
    }

    bool waitFor(const std::function<bool()>& condition) {
        return QTest::qWaitFor(condition, 5000);
    }

    ControlObject duration1;
    ControlObject duration2;
    ControlObject keylock1;
    ControlObject keylock2;
    crate::CratePreferences m_preferences{UserSettingsPointer()};
    crate::test::FakeNetworkAccessManager m_network;
    crate::test::FakeTokenSource m_tokens;
    crate::test::FakeScheduler m_scheduler;
    std::unique_ptr<crate::Client> m_pClient;
    std::unique_ptr<crate::PlanClient> m_pPlans;
    FakeEnvironment* m_pEnvironment = nullptr;
    QStringList m_eqDecks;
    /// The files of the Crate tracks of kUids.
    QStringList m_crateFiles;
    std::unique_ptr<crate::AutoDJSmartMixAdapter> m_pAdapter;
};

// The director plays the server's plan: armed at the handover, the plan's
// beatmatch from its cue, the incoming deck with keylock on, and Auto DJ goes
// on with the queue once the outgoing deck stops.
TEST_F(AutoDJSmartMixAdapterTest, PlaysTheServersPlanThroughAutoDJ) {
    makeAdapter();
    startSmartMix();
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    // Armed when the plan is asked for
    EXPECT_EQ(crate::SmartMixDirector::State::Armed, director()->state());
    ASSERT_TRUE(waitFor([this] { return !planRequests().isEmpty(); }));
    answerPlans();
    ASSERT_TRUE(waitFor([this] {
        return director()->state() != crate::SmartMixDirector::State::Armed ||
                director()->report().plannedStartOutgoingMs > 0.0;
    }));
    EXPECT_EQ(crate::SmartMixDirector::State::Armed, director()->state());
    EXPECT_NEAR(418135.0, director()->report().plannedStartOutgoingMs, 1e-6);

    // To the plan's cue and through the transition
    const double toCueMs =
            418135.0 - deck1.playposition.get() * kSeconds[0] * 1000.0 + 2 * kTickMs;
    play(toCueMs);
    ASSERT_EQ(crate::SmartMixDirector::State::Running, director()->state());
    EXPECT_EQ(1.0, deck2.play.get());
    EXPECT_EQ(1.0, keylock2.get());
    EXPECT_NEAR(1.023355, deck2.rateratio.get(), 1e-6);
    EXPECT_TRUE(fading());
    play(8000.0 + 4 * kTickMs);
    EXPECT_EQ(crate::SmartMixDirector::State::Completed, director()->state());
    EXPECT_EQ(0.0, deck1.play.get());
    EXPECT_EQ(1.0, mixer.crossfader.get());
    // Auto DJ goes on as always
    EXPECT_EQ(AutoDJProcessor::ADJ_IDLE, pProcessor->getState());
    EXPECT_EQ(QList<TrackId>{m_tracks[2]->getId()}, queuedIds());
    EXPECT_EQ(kDeck1, pProcessor->loads.last().group);
    EXPECT_EQ(m_tracks[2], pProcessor->loads.last().pTrack);
}

// The request: smart-mix-v3, Auto DJ's own transition time within the
// server's limits, and bass handoff only with the default EQ on Auto DJ's
// decks.
TEST_F(AutoDJSmartMixAdapterTest, AsksSmartMixV3ForAutoDJsTransition) {
    makeAdapter(false);
    startSmartMix();
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    ASSERT_TRUE(waitFor([this] { return !planRequests().isEmpty(); }));
    const QJsonObject body = bodyOf(planRequests().last());
    EXPECT_EQ(QStringLiteral("smart-mix-v3"), body.value(QStringLiteral("plannerVersion")));
    const QJsonObject context = body.value(QStringLiteral("edges"))
                                        .toArray()
                                        .first()
                                        .toObject()
                                        .value(QStringLiteral("context"))
                                        .toObject();
    EXPECT_EQ(QStringLiteral("decks"), context.value(QStringLiteral("source")).toString());
    EXPECT_TRUE(context.value(QStringLiteral("automatic")).toBool());
    EXPECT_EQ(10000, context.value(QStringLiteral("preferredDurationMs")).toInt());
    EXPECT_TRUE(context.value(QStringLiteral("allowBeatmatch")).toBool());
    EXPECT_TRUE(context.value(QStringLiteral("allowLongBeatmatch")).toBool());
    EXPECT_FALSE(context.value(QStringLiteral("allowBassHandoff")).toBool());
    EXPECT_EQ((QStringList{kDeck1, kDeck2}), m_eqDecks);

    // With the default EQ on, and a longer Auto DJ transition than the
    // server's 12 s
    m_pAdapter.reset();
    makeAdapter(true);
    pProcessor->setTransitionTime(20);
    const crate::PlanContext withEq = m_pAdapter->contextFor(kServer);
    EXPECT_TRUE(withEq.allowBassHandoff);
    EXPECT_EQ(12000, withEq.preferredDurationMs);
    pProcessor->setTransitionTime(-5);
    EXPECT_EQ(0, m_pAdapter->contextFor(kServer).preferredDurationMs);
}

// Ahead of the handover the plans of the next two edges are asked for, so the
// director starts with a cached plan and asks nothing more.
TEST_F(AutoDJSmartMixAdapterTest, AsksForTheNextTwoEdgesAhead) {
    makeAdapter();
    startSmartMix();
    ASSERT_TRUE(waitFor([this] { return !planRequests().isEmpty(); }));
    const QJsonArray edges =
            bodyOf(planRequests().first()).value(QStringLiteral("edges")).toArray();
    ASSERT_EQ(2, edges.size());
    EXPECT_EQ(kUids[0], edges[0].toObject().value(QStringLiteral("outgoingTrackEntityUid")));
    EXPECT_EQ(kUids[1], edges[0].toObject().value(QStringLiteral("incomingTrackEntityUid")));
    EXPECT_EQ(kUids[1], edges[1].toObject().value(QStringLiteral("outgoingTrackEntityUid")));
    EXPECT_EQ(kUids[2], edges[1].toObject().value(QStringLiteral("incomingTrackEntityUid")));
    answerPlans();
    ASSERT_TRUE(waitFor([this] { return m_pPlans->cachedPlans() == 2; }));
    const int requests = static_cast<int>(planRequests().size());

    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    EXPECT_EQ(crate::SmartMixDirector::State::Armed, director()->state());
    EXPECT_NEAR(418135.0, director()->report().plannedStartOutgoingMs, 1e-6);
    QTest::qWait(20);
    EXPECT_EQ(requests, planRequests().size());
}

// No plan within the director's 3 s: it hands the transition back and Auto DJ
// makes its FullIntroOutro one at its own fade point.
TEST_F(AutoDJSmartMixAdapterTest, HandsBackWhenNoPlanComes) {
    makeAdapter();
    startSmartMix();
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    ASSERT_EQ(crate::SmartMixDirector::State::Armed, director()->state());

    play(crate::SmartMixDirector::kPlanDeadlineMs + kTickMs);
    EXPECT_EQ(crate::SmartMixDirector::State::FellBack, director()->state());
    EXPECT_EQ(QStringLiteral("missing_plan"), director()->report().reason);
    ASSERT_TRUE(waitFor([this] { return pProcessor->getState() == AutoDJProcessor::ADJ_IDLE; }));
    EXPECT_EQ(0.0, deck2.play.get());
    // A late plan changes nothing
    answerPlans();
    QTest::qWait(20);
    EXPECT_EQ(crate::SmartMixDirector::State::FellBack, director()->state());
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    EXPECT_EQ(1.0, deck2.play.get());
}

// Decision 1: "Fade now" before the plan's cue cancels the director and
// Auto DJ fades now itself.
TEST_F(AutoDJSmartMixAdapterTest, FadeNowBeforeTheCueIsAutoDJs) {
    makeAdapter();
    startSmartMix();
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    ASSERT_TRUE(waitFor([this] { return !planRequests().isEmpty(); }));
    answerPlans();
    ASSERT_TRUE(waitFor([this] { return director()->report().plannedStartOutgoingMs > 0.0; }));

    pProcessor->fadeNow();
    EXPECT_EQ(crate::SmartMixDirector::State::Cancelled, director()->state());
    deck1.playposition.set(deck1.playposition.get() + kStep);
    EXPECT_TRUE(fading());
    EXPECT_EQ(1.0, deck2.play.get());
    // The queued finished() hands nothing back
    QTest::qWait(20);
    EXPECT_TRUE(fading());
}

TEST_F(AutoDJSmartMixAdapterTest, DisablingAutoDJCancelsTheDirector) {
    makeAdapter();
    startSmartMix();
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    ASSERT_TRUE(director()->isActive());
    pProcessor->toggleAutoDJ(false);
    EXPECT_EQ(crate::SmartMixDirector::State::Cancelled, director()->state());
    QTest::qWait(20);
    EXPECT_EQ(AutoDJProcessor::ADJ_DISABLED, pProcessor->getState());
}

// Decision 3: keylock on by default; the preference off leaves the deck's
// own.
TEST_F(AutoDJSmartMixAdapterTest, KeylockFollowsThePreference) {
    makeAdapter();
    EXPECT_EQ(std::optional<bool>(true), m_pAdapter->directorOptions().keylock);
    m_preferences.setSmartMixKeylock(false);
    EXPECT_FALSE(m_pAdapter->directorOptions().keylock.has_value());
}

// Edges that are not two tracks of one Crate server are Auto DJ's own.
TEST_F(AutoDJSmartMixAdapterTest, LeavesOtherTracksToAutoDJ) {
    makeAdapter();
    m_tracks[1] = getOrAddTrackByLocation(getTestDir().filePath(
            QStringLiteral("id3-test-data/cover-test-øé~ł€˚-jpg.mp3")));
    ASSERT_TRUE(m_tracks[1]);
    SoundSourceProxy(m_tracks[1])
            .updateTrackFromSource(
                    SoundSourceProxy::UpdateTrackFromSourceMode::Always,
                    SyncTrackMetadataParams{});
    m_tracks[1]->setDuration(kSeconds[1]);
    startSmartMix();
    ASSERT_TRUE(playDeck1Until([this] { return fading(); }).has_value());
    EXPECT_FALSE(director()->isActive());
    EXPECT_EQ(1.0, deck2.play.get());
}

// Auto DJ offers Crate Smart Mix only with a server that announces beatmatch.
TEST(AutoDJSmartMixOfferTest, OfferedWithABeatmatchServer) {
    const QByteArray withBeatmatch = R"({"decks": {"available": true,
        "min_client_version": "0.1.0", "contract_version": 1,
        "features": ["library_cursor", "beatmatch"]}})";
    const QByteArray without = R"({"decks": {"available": true,
        "min_client_version": "0.1.0", "contract_version": 1,
        "features": ["library_cursor"]}})";
    EXPECT_FALSE(crate::smartMixOffered({}));
    EXPECT_FALSE(crate::smartMixOffered({without, QByteArray()}));
    EXPECT_TRUE(crate::smartMixOffered({without, withBeatmatch}));
}

} // namespace crate_autodj_smartmix
