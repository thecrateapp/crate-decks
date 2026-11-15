#include "crate/smartmix/smartmixdirector.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <cmath>
#include <map>
#include <memory>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "crate/smartmix/curves.h"
#include "crate/smartmix/eqmap.h"
#include "engine/enginexfader.h"
#include "test/mixxxtest.h"

// TEST_GAP: these tests drive the director against fake controls and a fake
// clock and engine; how precisely the real engine starts and mixes the decks
// (start deviation, beat phase, clicks) is measured on rendered transitions
// by F6's G4 metrics, not here.

namespace {

using crate::SmartMixDirector;
using crate::TransitionPlan;
using crate::curves::Role;
using State = SmartMixDirector::State;

const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");
const QString kMaster = QStringLiteral("[Master]");
const QString kMixerProfile = QStringLiteral("[Mixer Profile]");

// The anonymised uids of the real fixtures (src/test/crate/data/transition-plans/).
const QString kBeatmatchOutgoing = QStringLiteral("637bd7c0-380e-5bc7-b841-dc2234cdfaf7");
const QString kBeatmatchIncoming = QStringLiteral("34137088-fcab-5f93-8866-31f1bf6d0871");
const QString kFallbackIncoming = QStringLiteral("a89833d1-75e5-52ce-8766-6df5b057d027");

constexpr double kTickMs = SmartMixDirector::kTickMs;

/// The clock and the engine positions, which the test moves.
class FakeEnvironment : public crate::SmartMixEnvironment {
  public:
    double nowMs() const override {
        return now;
    }
    double enginePlayPosition(const QString& group) const override {
        return positions.value(group);
    }
    QString trackKey(const QString& group) const override {
        return tracks.value(group);
    }

    double now = 1000.0;
    QHash<QString, double> positions;
    QHash<QString, QString> tracks;
};

/// Two decks and a mixer made of plain controls, without an engine: the
/// test plays the decks by moving their positions with the clock, as the
/// engine would, and seeks them when the director writes `playposition`.
class CrateSmartMixDirectorTest : public MixxxTest {
  protected:
    void SetUp() override {
        // CrossfaderFollowsCurve sets the fixture up again for each case.
        m_pDirector.reset();
        m_controls.clear();
        m_lastPlayposition.clear();
        for (const QString& group : {kDeck1, kDeck2}) {
            for (const QString& item : {QStringLiteral("play"),
                         QStringLiteral("playposition"),
                         QStringLiteral("duration"),
                         QStringLiteral("bpm"),
                         QStringLiteral("beat_distance"),
                         QStringLiteral("quantize"),
                         QStringLiteral("sync_enabled"),
                         QStringLiteral("keylock"),
                         QStringLiteral("orientation")}) {
                addControl(group, item, 0.0);
            }
            addControl(group, QStringLiteral("pregain"), 1.0);
            addControl(group, QStringLiteral("rate_ratio"), 1.0);
            const QString eq = crate::eqmap::effectGroup(group);
            for (const QString& item : {crate::eqmap::kLowGain,
                         crate::eqmap::kMidGain,
                         crate::eqmap::kHighGain}) {
                addControl(eq, item, crate::eqmap::kNeutralGain);
            }
            for (const QString& item : {crate::eqmap::kLowKill,
                         crate::eqmap::kMidKill,
                         crate::eqmap::kHighKill}) {
                addControl(eq, item, 0.0);
            }
        }
        set(kDeck1, QStringLiteral("orientation"), 0.0);
        set(kDeck2, QStringLiteral("orientation"), 2.0);
        addControl(kMaster, QStringLiteral("crossfader"), -1.0);
        addControl(kMixerProfile, QStringLiteral("xFaderMode"), MIXXX_XFADER_ADDITIVE);
        addControl(kMixerProfile, QStringLiteral("xFaderCurve"), EngineXfader::kTransformDefault);
        addControl(kMixerProfile,
                QStringLiteral("xFaderCalibration"),
                EngineXfader::getPowerCalibration(EngineXfader::kTransformDefault));
        addControl(kMixerProfile, QStringLiteral("xFaderReverse"), 0.0);

        auto pEnvironment = std::make_unique<FakeEnvironment>();
        m_pEnvironment = pEnvironment.get();
        m_pDirector = std::make_unique<SmartMixDirector>(std::move(pEnvironment));
    }

    void addControl(const QString& group, const QString& item, double value) {
        const ConfigKey key(group, item);
        m_controls[key] = std::make_unique<ControlObject>(key, true, false, false, value);
    }

    static void set(const QString& group, const QString& item, double value) {
        ControlObject::set(ConfigKey(group, item), value);
    }
    static double get(const QString& group, const QString& item) {
        return ControlObject::get(ConfigKey(group, item));
    }
    static double eq(const QString& group, const QString& item) {
        return get(crate::eqmap::effectGroup(group), item);
    }

    TransitionPlan plan(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/transition-plans/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        const auto result = TransitionPlan::fromJson(file.readAll());
        EXPECT_TRUE(result.ok());
        return result.ok() ? result.value() : TransitionPlan();
    }

    qint64 trackDurationMs(const QString& uid) const {
        QFile file(getTestDir().filePath(
                QStringLiteral("crate/data/transition-plans/real-tracks.json")));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return QJsonDocument::fromJson(file.readAll())
                .object()
                .value(QStringLiteral("durationMs"))
                .toObject()
                .value(uid)
                .toInteger();
    }

    /// Loads the fixture track `uid` into `group`, stopped at `positionMs`.
    void load(const QString& group, const QString& uid, double positionMs = 0.0) {
        set(group, QStringLiteral("duration"), static_cast<double>(trackDurationMs(uid)) / 1000.0);
        m_pEnvironment->tracks.insert(group, uid);
        seek(group, positionMs);
    }

    /// The two decks of the beatmatch fixtures, deck 1 playing `msBeforeCue`
    /// before the plan's cue.
    void loadBeatmatchEdge(const TransitionPlan& beatmatch, double msBeforeCue) {
        load(kDeck1,
                kBeatmatchOutgoing,
                static_cast<double>(beatmatch.outgoingCueMs) - msBeforeCue);
        load(kDeck2, kBeatmatchIncoming);
        set(kDeck1, QStringLiteral("play"), 1.0);
    }

    double positionMs(const QString& group) const {
        return m_pEnvironment->positions.value(group) * get(group, QStringLiteral("duration")) *
                1000.0;
    }

    void seek(const QString& group, double positionMs) {
        const double fraction = positionMs / (get(group, QStringLiteral("duration")) * 1000.0);
        m_pEnvironment->positions.insert(group, fraction);
        set(group, QStringLiteral("playposition"), fraction);
        m_lastPlayposition[group] = fraction;
    }

    /// Moves the clock and the playing decks by `ms`, then lets the director
    /// tick, and applies the seeks it wrote.
    void advance(double ms) {
        m_pEnvironment->now += ms;
        for (const QString& group : {kDeck1, kDeck2}) {
            if (get(group, QStringLiteral("play")) > 0.5) {
                seek(group, positionMs(group) + ms * get(group, QStringLiteral("rate_ratio")));
            }
        }
        m_pDirector->tick();
        applySeeks();
    }

    void applySeeks() {
        for (const QString& group : {kDeck1, kDeck2}) {
            const double written = get(group, QStringLiteral("playposition"));
            if (written != m_lastPlayposition.value(group)) {
                seek(group, written * get(group, QStringLiteral("duration")) * 1000.0);
            }
        }
    }

    /// Ticks at 60 Hz for `ms`, the last tick shorter if needed.
    void runFor(double ms) {
        while (ms > 1e-9) {
            const double step = std::min(kTickMs, ms);
            advance(step);
            ms -= step;
        }
    }

    /// Ticks until the director leaves `state`, for `limitMs` at most.
    void runWhile(State state, double limitMs = 200000.0) {
        for (double elapsed = 0.0; m_pDirector->state() == state && elapsed < limitMs;
                elapsed += kTickMs) {
            advance(kTickMs);
        }
    }

    /// Arms deck 1 → deck 2 and gives the plan, then runs until the start.
    void armAndStart(const TransitionPlan& transition) {
        ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
        ASSERT_TRUE(m_pDirector->setPlan(transition));
        runWhile(State::Armed);
        runWhile(State::Preparing);
        ASSERT_EQ(m_pDirector->state(), State::Running);
        m_runStartMs = m_pEnvironment->now;
    }

    /// Ticks until `progress` of a transition of `durationMs`.
    void runUntilProgress(double progress, double durationMs) {
        runFor(m_runStartMs + progress * durationMs - m_pEnvironment->now);
    }

    /// The level each deck reaches the main mix with: its crossfader gain
    /// times its pregain.
    std::pair<double, double> deckLevels() const {
        CSAMPLE_GAIN left = 1.0f;
        CSAMPLE_GAIN right = 1.0f;
        EngineXfader::getXfadeGains(get(kMaster, QStringLiteral("crossfader")),
                get(kMixerProfile, QStringLiteral("xFaderCurve")),
                get(kMixerProfile, QStringLiteral("xFaderCalibration")),
                get(kMixerProfile, QStringLiteral("xFaderMode")),
                get(kMixerProfile, QStringLiteral("xFaderReverse")) > 0.5,
                &left,
                &right);
        const auto gainOf = [&](const QString& group) {
            const int orientation = static_cast<int>(get(group, QStringLiteral("orientation")));
            const double crossfaderGain = orientation == 0 ? left : orientation == 2 ? right
                                                                                     : 1.0;
            return crossfaderGain * get(group, QStringLiteral("pregain"));
        };
        return {gainOf(kDeck1), gainOf(kDeck2)};
    }

    std::map<ConfigKey, std::unique_ptr<ControlObject>> m_controls;
    QHash<QString, double> m_lastPlayposition;
    FakeEnvironment* m_pEnvironment = nullptr;
    double m_runStartMs = 0.0;
    std::unique_ptr<SmartMixDirector> m_pDirector;
};

TEST_F(CrateSmartMixDirectorTest, PreparesIncomingDeckBeforeStart) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 20000.0);
    // Leftovers on the incoming deck that would spoil the plan.
    set(kDeck2, QStringLiteral("quantize"), 1.0);
    set(kDeck2, QStringLiteral("sync_enabled"), 1.0);
    set(kDeck2, QStringLiteral("pregain"), 0.7);
    set(crate::eqmap::effectGroup(kDeck2), crate::eqmap::kMidGain, 0.5);
    set(crate::eqmap::effectGroup(kDeck2), crate::eqmap::kHighKill, 1.0);
    SmartMixDirector::Options options;
    options.keylock = true;

    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2, options));
    ASSERT_TRUE(m_pDirector->setPlan(beatmatch));
    EXPECT_EQ(m_pDirector->state(), State::Armed);
    // Still more than 8 s ahead: the incoming deck is not touched yet.
    runFor(11990.0);
    EXPECT_EQ(m_pDirector->state(), State::Armed);
    EXPECT_EQ(get(kDeck2, QStringLiteral("quantize")), 1.0);

    runFor(2 * kTickMs);
    ASSERT_EQ(m_pDirector->state(), State::Preparing);
    EXPECT_NEAR(positionMs(kDeck2), static_cast<double>(beatmatch.startIncomingCueMs()), 1e-6);
    EXPECT_EQ(beatmatch.startIncomingCueMs(), 198 + std::lround(298 * 1.023355));
    EXPECT_EQ(get(kDeck2, QStringLiteral("play")), 0.0);
    EXPECT_EQ(get(kDeck2, QStringLiteral("quantize")), 0.0);
    EXPECT_EQ(get(kDeck2, QStringLiteral("sync_enabled")), 0.0);
    EXPECT_EQ(get(kDeck2, QStringLiteral("keylock")), 1.0);
    EXPECT_EQ(eq(kDeck2, crate::eqmap::kMidGain), crate::eqmap::kNeutralGain);
    EXPECT_EQ(eq(kDeck2, crate::eqmap::kHighKill), 0.0);
    EXPECT_EQ(eq(kDeck2, crate::eqmap::kLowGain), crate::eqmap::kNeutralGain);
    // Android's incoming level at the start: its staging gain.
    EXPECT_NEAR(get(kDeck2, QStringLiteral("pregain")),
            crate::curves::stagingGain(beatmatch.incomingGainDb),
            1e-9);
    EXPECT_NEAR(get(kDeck2, QStringLiteral("rate_ratio")), beatmatch.incomingTempoRatio, 1e-9);
    EXPECT_EQ(get(kMaster, QStringLiteral("crossfader")), -1.0);
    // The outgoing deck plays on as it was.
    EXPECT_EQ(get(kDeck1, QStringLiteral("pregain")), 1.0);

    // Quantize and sync come back at the end.
    runWhile(State::Preparing);
    runWhile(State::Running);
    EXPECT_EQ(m_pDirector->state(), State::Completed);
    EXPECT_EQ(get(kDeck2, QStringLiteral("quantize")), 1.0);
    EXPECT_EQ(get(kDeck2, QStringLiteral("sync_enabled")), 1.0);
}

TEST_F(CrateSmartMixDirectorTest, PreparesAtOnceWhenThePlanComesLate) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 3000.0);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    runFor(500.0);
    ASSERT_TRUE(m_pDirector->setPlan(beatmatch));
    EXPECT_EQ(m_pDirector->state(), State::Preparing);
}

TEST_F(CrateSmartMixDirectorTest, StartsOnPredictedBeat) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 5000.0);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    ASSERT_TRUE(m_pDirector->setPlan(beatmatch));
    ASSERT_EQ(m_pDirector->state(), State::Preparing);

    // 25 ms before the cue by the play position, but the beat clock puts the
    // outgoing deck's next beat, on the cue, 20 ms away.
    constexpr double kBpm = 124.0;
    const double beatMs = 60000.0 / kBpm;
    seek(kDeck1, static_cast<double>(beatmatch.outgoingCueMs) - 25.0);
    set(kDeck1, QStringLiteral("bpm"), kBpm);
    set(kDeck1, QStringLiteral("beat_distance"), 1.0 - 20.0 / beatMs);
    m_pDirector->tick();
    EXPECT_EQ(m_pDirector->state(), State::Preparing);
    EXPECT_TRUE(m_pDirector->report().usedBeatClock);

    // The position would say 25 ms; the start waits for the beat, at 20 ms.
    advance(19.0);
    EXPECT_EQ(get(kDeck2, QStringLiteral("play")), 0.0);
    advance(1.0);
    EXPECT_EQ(m_pDirector->state(), State::Running);
    EXPECT_EQ(get(kDeck2, QStringLiteral("play")), 1.0);
    EXPECT_EQ(m_pDirector->report().lateMs, 0.0);
    // On time: the incoming deck starts where it was prepared.
    EXPECT_NEAR(m_pDirector->report().incomingStartMs,
            static_cast<double>(beatmatch.startIncomingCueMs()),
            1e-9);
    EXPECT_NEAR(positionMs(kDeck2), static_cast<double>(beatmatch.startIncomingCueMs()), 1e-6);
}

TEST_F(CrateSmartMixDirectorTest, LateStartJumpsTheIncomingDeckAhead) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    // The plan arrives 1.5 s after the cue went by.
    loadBeatmatchEdge(beatmatch, -1500.0);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    ASSERT_TRUE(m_pDirector->setPlan(beatmatch));
    applySeeks();
    ASSERT_EQ(m_pDirector->state(), State::Running);
    const auto& report = m_pDirector->report();
    EXPECT_NEAR(report.lateMs, 1500.0, 1e-6);
    const double expectedMs = static_cast<double>(beatmatch.startIncomingCueMs()) +
            1500.0 * beatmatch.incomingTempoRatio;
    EXPECT_NEAR(report.incomingStartMs, expectedMs, 1e-6);
    EXPECT_NEAR(positionMs(kDeck2), expectedMs, 1e-6);
    // What is left of the outgoing track bounds the transition.
    EXPECT_NEAR(report.durationMs,
            std::min(8000.0,
                    static_cast<double>(trackDurationMs(kBeatmatchOutgoing)) -
                            positionMs(kDeck1) - SmartMixDirector::kStartSafetyLeadMs),
            1e-6);
}

TEST_F(CrateSmartMixDirectorTest, AppliesTempoRatio) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 6000.0);
    set(kDeck1, QStringLiteral("rate_ratio"), 1.02);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    ASSERT_TRUE(m_pDirector->setPlan(beatmatch));
    EXPECT_NEAR(get(kDeck2, QStringLiteral("rate_ratio")),
            beatmatch.incomingTempoRatio * 1.02,
            1e-9);

    // The incoming deck follows the outgoing deck's tempo.
    set(kDeck1, QStringLiteral("rate_ratio"), 1.03);
    advance(kTickMs);
    EXPECT_NEAR(get(kDeck2, QStringLiteral("rate_ratio")),
            beatmatch.incomingTempoRatio * 1.03,
            1e-9);

    runWhile(State::Preparing);
    runFor(1000.0);
    EXPECT_NEAR(get(kDeck2, QStringLiteral("rate_ratio")),
            beatmatch.incomingTempoRatio * 1.03,
            1e-9);

    // After the transition the deck eases back to its own tempo in 4 s.
    runWhile(State::Running);
    ASSERT_EQ(m_pDirector->state(), State::Completed);
    ASSERT_TRUE(m_pDirector->isRestoringTempo());
    const double fromRate = get(kDeck2, QStringLiteral("rate_ratio"));
    EXPECT_NEAR(fromRate, beatmatch.incomingTempoRatio * 1.03, 1e-9);
    runFor(2000.0);
    EXPECT_NEAR(get(kDeck2, QStringLiteral("rate_ratio")), (fromRate + 1.0) / 2.0, 0.01);
    runFor(2100.0);
    EXPECT_FALSE(m_pDirector->isRestoringTempo());
    EXPECT_EQ(get(kDeck2, QStringLiteral("rate_ratio")), 1.0);
}

// F6.C-PR3 decision 2 (Diego, 2026-11-15): after a beatmatch the incoming
// deck eases back to the tempo it had before, not to 1 and not kept.
TEST_F(CrateSmartMixDirectorTest, EasesBackToTheIncomingDecksOwnTempo) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 6000.0);
    set(kDeck2, QStringLiteral("rate_ratio"), 0.97);
    armAndStart(beatmatch);
    EXPECT_NEAR(get(kDeck2, QStringLiteral("rate_ratio")), beatmatch.incomingTempoRatio, 1e-9);
    runWhile(State::Running);
    ASSERT_EQ(m_pDirector->state(), State::Completed);
    ASSERT_TRUE(m_pDirector->isRestoringTempo());
    runFor(crate::curves::kTempoRestoreMs + kTickMs);
    EXPECT_FALSE(m_pDirector->isRestoringTempo());
    EXPECT_NEAR(get(kDeck2, QStringLiteral("rate_ratio")), 0.97, 1e-9);
}

TEST_F(CrateSmartMixDirectorTest, AdaptivePlanLeavesTheTempo) {
    const TransitionPlan adaptive = plan(QStringLiteral("real-adaptive-fallback.json"));
    load(kDeck1, kBeatmatchIncoming, static_cast<double>(adaptive.outgoingCueMs) - 9000.0);
    load(kDeck2, kFallbackIncoming);
    set(kDeck1, QStringLiteral("play"), 1.0);
    set(kDeck1, QStringLiteral("rate_ratio"), 1.04);
    set(kDeck2, QStringLiteral("rate_ratio"), 0.98);
    armAndStart(adaptive);
    EXPECT_EQ(get(kDeck2, QStringLiteral("rate_ratio")), 0.98);
    runWhile(State::Running);
    EXPECT_EQ(get(kDeck2, QStringLiteral("rate_ratio")), 0.98);
    EXPECT_FALSE(m_pDirector->isRestoringTempo());
}

TEST_F(CrateSmartMixDirectorTest, AppliesGainRamps) {
    // A constant-power crossfader carries the equal-power curve by itself,
    // so the pregains are the plan's staging alone.
    set(kMixerProfile, QStringLiteral("xFaderMode"), MIXXX_XFADER_CONSTPWR);
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 6000.0);
    armAndStart(beatmatch);
    const double outgoingStaging = crate::curves::stagingGain(beatmatch.outgoingGainDb);
    const double incomingStaging = crate::curves::stagingGain(beatmatch.incomingGainDb);

    for (const double progress : {0.25, 0.5, 0.75}) {
        runUntilProgress(progress, static_cast<double>(beatmatch.durationMs));
        SCOPED_TRACE(progress);
        EXPECT_NEAR(get(kDeck1, QStringLiteral("pregain")),
                crate::curves::stagingFactor(Role::Outgoing, progress, outgoingStaging),
                1e-3);
        EXPECT_NEAR(get(kDeck2, QStringLiteral("pregain")),
                crate::curves::stagingFactor(Role::Incoming, progress, incomingStaging),
                1e-3);
    }
    // Halfway the outgoing deck is down to its gain; the incoming one is
    // back to unity at the end.
    EXPECT_NEAR(crate::curves::stagingFactor(Role::Outgoing, 0.5, outgoingStaging),
            outgoingStaging,
            1e-12);

    runWhile(State::Running);
    EXPECT_EQ(m_pDirector->state(), State::Completed);
    EXPECT_EQ(m_pDirector->report().reason, QStringLiteral("completed"));
    EXPECT_EQ(get(kDeck2, QStringLiteral("pregain")), 1.0);
    EXPECT_EQ(get(kDeck1, QStringLiteral("pregain")), 1.0);
    EXPECT_EQ(get(kDeck1, QStringLiteral("play")), 0.0);
    EXPECT_EQ(get(kMaster, QStringLiteral("crossfader")), 1.0);
}

TEST_F(CrateSmartMixDirectorTest, BassHandoffAtProgress) {
    const TransitionPlan longBlend = plan(QStringLiteral("synthetic-assist-long-blend.json"));
    ASSERT_TRUE(longBlend.hasBassHandoff());
    ASSERT_EQ(longBlend.handoffProgress, 0.5);
    loadBeatmatchEdge(longBlend, 9000.0);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    ASSERT_TRUE(m_pDirector->setPlan(longBlend));
    runWhile(State::Armed);
    // Prepared with its lows cut, as Android starts the incoming track.
    EXPECT_NEAR(eq(kDeck2, crate::eqmap::kLowGain), crate::eqmap::lowGainForCutDb(-24.0), 1e-9);
    runWhile(State::Preparing);
    m_runStartMs = m_pEnvironment->now;

    const double durationMs = static_cast<double>(longBlend.durationMs);
    const auto expectCuts = [&](double outgoingDb, double incomingDb) {
        EXPECT_NEAR(eq(kDeck1, crate::eqmap::kLowGain),
                crate::eqmap::lowGainForCutDb(outgoingDb),
                1e-3);
        EXPECT_NEAR(eq(kDeck2, crate::eqmap::kLowGain),
                crate::eqmap::lowGainForCutDb(incomingDb),
                1e-3);
    };
    // Before the swap window (0.4375 to 0.5625): the outgoing bass only.
    runUntilProgress(0.4, durationMs);
    expectCuts(0.0, -24.0);
    // In the middle of the swap both are half way.
    runUntilProgress(0.5, durationMs);
    expectCuts(-12.0, -12.0);
    // After it: the incoming bass only.
    runUntilProgress(0.6, durationMs);
    expectCuts(-24.0, 0.0);

    runWhile(State::Running);
    EXPECT_EQ(eq(kDeck1, crate::eqmap::kLowGain), crate::eqmap::kNeutralGain);
    EXPECT_EQ(eq(kDeck2, crate::eqmap::kLowGain), crate::eqmap::kNeutralGain);
}

TEST_F(CrateSmartMixDirectorTest, CrossfaderFollowsCurve) {
    struct Case {
        const char* name;
        double mode;
        double outgoingOrientation;
        double reverse;
        const char* fixture;
    };
    const Case cases[] = {
            {"additive", MIXXX_XFADER_ADDITIVE, 0.0, 0.0, "real-beatmatch.json"},
            {"constant power", MIXXX_XFADER_CONSTPWR, 0.0, 0.0, "real-beatmatch.json"},
            {"outgoing on the right", MIXXX_XFADER_ADDITIVE, 2.0, 0.0, "real-beatmatch.json"},
            {"reversed crossfader", MIXXX_XFADER_CONSTPWR, 0.0, 1.0, "real-beatmatch.json"},
            {"dj-blend", MIXXX_XFADER_ADDITIVE, 0.0, 0.0, "synthetic-assist-long-blend.json"},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.name);
        SetUp();
        set(kMixerProfile, QStringLiteral("xFaderMode"), test.mode);
        set(kMixerProfile, QStringLiteral("xFaderReverse"), test.reverse);
        set(kDeck1, QStringLiteral("orientation"), test.outgoingOrientation);
        set(kDeck2, QStringLiteral("orientation"), 2.0 - test.outgoingOrientation);
        // On the outgoing deck's end, where Auto DJ leaves it
        const bool outgoingOnLeft = test.outgoingOrientation == 0.0;
        set(kMaster,
                QStringLiteral("crossfader"),
                outgoingOnLeft != (test.reverse > 0.5) ? -1.0 : 1.0);
        const TransitionPlan transition = plan(QString::fromLatin1(test.fixture));
        const bool djBlend = transition.curve == TransitionPlan::Curve::DjBlend;
        loadBeatmatchEdge(transition, 6000.0);
        armAndStart(transition);
        const double durationMs = static_cast<double>(transition.durationMs);
        for (const double progress : {0.1, 0.35, 0.5, 0.8}) {
            runUntilProgress(progress, durationMs);
            SCOPED_TRACE(progress);
            const auto [outgoingLevel, incomingLevel] = deckLevels();
            EXPECT_NEAR(outgoingLevel,
                    crate::curves::transitionGain(
                            Role::Outgoing, progress, transition.outgoingGainDb, djBlend),
                    1e-3);
            EXPECT_NEAR(incomingLevel,
                    crate::curves::transitionGain(
                            Role::Incoming, progress, transition.incomingGainDb, djBlend),
                    1e-3);
        }
        runWhile(State::Running);
        const auto [outgoingLevel, incomingLevel] = deckLevels();
        EXPECT_NEAR(outgoingLevel, 0.0, 1e-6);
        EXPECT_NEAR(incomingLevel, 1.0, 1e-6);
    }
}

// Auto DJ leaves the crossfader on the outgoing deck's end, but a DJ may have
// centred it: the director then moves it to the end without the outgoing
// deck jumping +3 dB on a constant-power curve, and the transition still
// ends on the incoming deck at its full level.
TEST_F(CrateSmartMixDirectorTest, KeepsTheOutgoingLevelOfACentredCrossfader) {
    set(kMixerProfile, QStringLiteral("xFaderMode"), MIXXX_XFADER_CONSTPWR);
    set(kMaster, QStringLiteral("crossfader"), 0.0);
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 12000.0);
    const double levelBefore = deckLevels().first;
    EXPECT_NEAR(levelBefore, std::sqrt(0.5), 1e-3);

    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    ASSERT_TRUE(m_pDirector->setPlan(beatmatch));
    runWhile(State::Armed);
    ASSERT_EQ(m_pDirector->state(), State::Preparing);
    EXPECT_EQ(get(kMaster, QStringLiteral("crossfader")), -1.0);
    EXPECT_NEAR(deckLevels().first, levelBefore, 1e-6);
    EXPECT_NEAR(deckLevels().second, 0.0, 1e-6);

    runWhile(State::Preparing);
    ASSERT_EQ(m_pDirector->state(), State::Running);
    runFor(kTickMs);
    EXPECT_NEAR(deckLevels().first, levelBefore, 0.01);
    runWhile(State::Running);
    EXPECT_EQ(m_pDirector->state(), State::Completed);
    EXPECT_NEAR(deckLevels().second, 1.0, 1e-6);
    // The outgoing deck is stopped with its own pregain back.
    EXPECT_EQ(get(kDeck1, QStringLiteral("pregain")), 1.0);
}

TEST_F(CrateSmartMixDirectorTest, FallsBackWhenPlanHasFallbackReason) {
    // The server could not beatmatch the edge and planned an adaptive
    // crossfade instead: Android plays that plan, and so does the director.
    const TransitionPlan adaptive = plan(QStringLiteral("real-adaptive-fallback.json"));
    ASSERT_TRUE(adaptive.fallbackReason.has_value());
    load(kDeck1, kBeatmatchIncoming, static_cast<double>(adaptive.outgoingCueMs) - 9000.0);
    load(kDeck2, kFallbackIncoming);
    set(kDeck1, QStringLiteral("play"), 1.0);
    QSignalSpy finished(m_pDirector.get(), &SmartMixDirector::finished);
    armAndStart(adaptive);
    // The cue leaves room for the transition before the outgoing track ends
    // (Android's effectiveCueMs).
    EXPECT_NEAR(m_pDirector->report().plannedStartOutgoingMs,
            static_cast<double>(trackDurationMs(kBeatmatchIncoming)) -
                    static_cast<double>(adaptive.durationMs) -
                    SmartMixDirector::kStartSafetyLeadMs,
            1e-9);
    EXPECT_EQ(eq(kDeck2, crate::eqmap::kLowGain), crate::eqmap::kNeutralGain);
    runWhile(State::Running);

    EXPECT_EQ(m_pDirector->state(), State::FellBack);
    EXPECT_EQ(m_pDirector->report().reason, QStringLiteral("incompatible_tempo"));
    EXPECT_TRUE(m_pDirector->report().mixed);
    ASSERT_EQ(finished.count(), 1);
    EXPECT_EQ(finished.at(0).at(1).toString(), QStringLiteral("incompatible_tempo"));
    EXPECT_EQ(get(kDeck1, QStringLiteral("play")), 0.0);
    EXPECT_EQ(get(kDeck2, QStringLiteral("play")), 1.0);
    EXPECT_EQ(get(kMaster, QStringLiteral("crossfader")), 1.0);
}

TEST_F(CrateSmartMixDirectorTest, FallsBackWhenPlanMissingAtDeadline) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 6000.0);
    set(kDeck2, QStringLiteral("quantize"), 1.0);
    QSignalSpy finished(m_pDirector.get(), &SmartMixDirector::finished);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    runFor(SmartMixDirector::kPlanDeadlineMs - kTickMs);
    EXPECT_EQ(m_pDirector->state(), State::Armed);
    runFor(kTickMs);
    EXPECT_EQ(m_pDirector->state(), State::FellBack);
    EXPECT_EQ(m_pDirector->report().reason, QStringLiteral("missing_plan"));
    EXPECT_FALSE(m_pDirector->report().mixed);
    EXPECT_EQ(finished.count(), 1);
    // The caller makes the transition: nothing was touched.
    EXPECT_EQ(get(kDeck2, QStringLiteral("quantize")), 1.0);
    EXPECT_EQ(get(kMaster, QStringLiteral("crossfader")), -1.0);
    // A plan after the deadline is too late.
    EXPECT_FALSE(m_pDirector->setPlan(beatmatch));
}

TEST_F(CrateSmartMixDirectorTest, FallsBackOnAPlanThatDoesNotFit) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    // The incoming deck holds a shorter track than the plan's.
    loadBeatmatchEdge(beatmatch, 6000.0);
    set(kDeck1, QStringLiteral("duration"), 400.0);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    EXPECT_FALSE(m_pDirector->setPlan(beatmatch));
    EXPECT_EQ(m_pDirector->state(), State::FellBack);
    EXPECT_EQ(m_pDirector->report().reason, QStringLiteral("invalid_plan"));
    EXPECT_FALSE(m_pDirector->report().detail.isEmpty());
}

TEST_F(CrateSmartMixDirectorTest, StopsAutomatingOverriddenControl) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 6000.0);
    QSignalSpy overridden(m_pDirector.get(), &SmartMixDirector::controlOverridden);
    armAndStart(beatmatch);
    const double durationMs = static_cast<double>(beatmatch.durationMs);
    runUntilProgress(0.3, durationMs);

    // The DJ takes the crossfader.
    set(kMaster, QStringLiteral("crossfader"), 0.3);
    const double incomingPregain = get(kDeck2, QStringLiteral("pregain"));
    runUntilProgress(0.6, durationMs);
    EXPECT_EQ(get(kMaster, QStringLiteral("crossfader")), 0.3);
    ASSERT_EQ(overridden.count(), 1);
    EXPECT_EQ(overridden.at(0).at(0).value<ConfigKey>(),
            ConfigKey(kMaster, QStringLiteral("crossfader")));
    EXPECT_EQ(m_pDirector->report().overridden.size(), 1);
    // The rest goes on: the pregains keep the plan's staging.
    EXPECT_EQ(m_pDirector->state(), State::Running);
    EXPECT_NE(get(kDeck2, QStringLiteral("pregain")), incomingPregain);
    EXPECT_NEAR(get(kDeck1, QStringLiteral("pregain")),
            crate::curves::stagingGain(beatmatch.outgoingGainDb),
            1e-9);

    // The DJ also sets the outgoing gain: that one stays too.
    set(kDeck1, QStringLiteral("pregain"), 0.9);
    runWhile(State::Running);
    EXPECT_EQ(overridden.count(), 2);
    EXPECT_EQ(get(kDeck1, QStringLiteral("pregain")), 0.9);
    EXPECT_EQ(get(kMaster, QStringLiteral("crossfader")), 0.3);
    EXPECT_EQ(m_pDirector->state(), State::Completed);
    EXPECT_EQ(get(kDeck2, QStringLiteral("pregain")), 1.0);
}

TEST_F(CrateSmartMixDirectorTest, NeverWritesSameControlTwicePerTick) {
    QList<ControlProxy*> watched;
    QHash<ControlProxy*, int> writes;
    for (const QString& group : {kDeck1, kDeck2}) {
        for (const QString& item : {QStringLiteral("play"),
                     QStringLiteral("playposition"),
                     QStringLiteral("pregain"),
                     QStringLiteral("rate_ratio"),
                     QStringLiteral("quantize"),
                     QStringLiteral("sync_enabled")}) {
            watched.append(new ControlProxy(group, item, m_pDirector.get()));
        }
        watched.append(new ControlProxy(
                crate::eqmap::effectGroup(group), crate::eqmap::kLowGain, m_pDirector.get()));
    }
    watched.append(new ControlProxy(kMaster, QStringLiteral("crossfader"), m_pDirector.get()));
    for (ControlProxy* pProxy : std::as_const(watched)) {
        pProxy->connectValueChanged(m_pDirector.get(), [&writes, pProxy](double) {
            ++writes[pProxy];
        });
    }
    const auto expectOneWriteEach = [&](const char* when) {
        for (ControlProxy* pProxy : std::as_const(watched)) {
            EXPECT_LE(writes.value(pProxy), 1)
                    << when << " " << pProxy->getKey().group.toStdString()
                    << pProxy->getKey().item.toStdString();
        }
        writes.clear();
    };

    // A late plan: preparing, starting late with a jump and the first step
    // of the mix all happen in the tick that takes the plan.
    const TransitionPlan longBlend = plan(QStringLiteral("synthetic-assist-long-blend.json"));
    loadBeatmatchEdge(longBlend, -700.0);
    set(kDeck2, QStringLiteral("quantize"), 1.0);
    set(kDeck2, QStringLiteral("rate_ratio"), 0.97);
    writes.clear();
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    ASSERT_TRUE(m_pDirector->setPlan(longBlend));
    ASSERT_EQ(m_pDirector->state(), State::Running);
    expectOneWriteEach("plan");
    while (m_pDirector->state() == State::Running || m_pDirector->isRestoringTempo()) {
        m_pEnvironment->now += kTickMs;
        m_pDirector->tick();
        expectOneWriteEach("tick");
        applySeeks();
    }
    EXPECT_EQ(m_pDirector->state(), State::Completed);
}

TEST_F(CrateSmartMixDirectorTest, CancelsWhenTheOutgoingDeckJumpsBack) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    loadBeatmatchEdge(beatmatch, 6000.0);
    set(kDeck2, QStringLiteral("quantize"), 1.0);
    set(kDeck2, QStringLiteral("pregain"), 0.8);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    ASSERT_TRUE(m_pDirector->setPlan(beatmatch));
    ASSERT_EQ(m_pDirector->state(), State::Preparing);
    // A loop: the outgoing deck goes back four beats.
    seek(kDeck1, positionMs(kDeck1) - 2000.0);
    advance(kTickMs);
    EXPECT_EQ(m_pDirector->state(), State::Cancelled);
    EXPECT_EQ(m_pDirector->report().reason, QStringLiteral("outgoing_jumped_back"));
    // The incoming deck is as it was.
    EXPECT_EQ(get(kDeck2, QStringLiteral("quantize")), 1.0);
    EXPECT_EQ(get(kDeck2, QStringLiteral("pregain")), 0.8);
    EXPECT_EQ(get(kDeck2, QStringLiteral("rate_ratio")), 1.0);
    EXPECT_EQ(get(kDeck2, QStringLiteral("play")), 0.0);
}

TEST_F(CrateSmartMixDirectorTest, CancelsWhenADeckStopsOrChangesTrack) {
    const TransitionPlan longBlend = plan(QStringLiteral("synthetic-assist-long-blend.json"));
    loadBeatmatchEdge(longBlend, 6000.0);
    armAndStart(longBlend);
    runUntilProgress(0.2, static_cast<double>(longBlend.durationMs));
    set(kDeck2, QStringLiteral("play"), 0.0);
    advance(kTickMs);
    EXPECT_EQ(m_pDirector->state(), State::Cancelled);
    EXPECT_EQ(m_pDirector->report().reason, QStringLiteral("incoming_stopped"));
    // No deck keeps the plan's bass cut.
    EXPECT_EQ(eq(kDeck2, crate::eqmap::kLowGain), crate::eqmap::kNeutralGain);
    EXPECT_EQ(eq(kDeck1, crate::eqmap::kLowGain), crate::eqmap::kNeutralGain);

    SetUp();
    loadBeatmatchEdge(longBlend, 20000.0);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    ASSERT_TRUE(m_pDirector->setPlan(longBlend));
    m_pEnvironment->tracks.insert(kDeck2, kFallbackIncoming);
    advance(kTickMs);
    EXPECT_EQ(m_pDirector->state(), State::Cancelled);
    EXPECT_EQ(m_pDirector->report().reason, QStringLiteral("track_changed"));
}

TEST_F(CrateSmartMixDirectorTest, GaplessCutsToTheIncomingDeck) {
    const TransitionPlan gapless = plan(QStringLiteral("real-gapless.json"));
    load(kDeck1, kBeatmatchOutgoing, static_cast<double>(gapless.outgoingCueMs) - 5000.0);
    load(kDeck2, kBeatmatchIncoming);
    set(kDeck1, QStringLiteral("play"), 1.0);
    ASSERT_TRUE(m_pDirector->arm(kDeck1, kDeck2));
    ASSERT_TRUE(m_pDirector->setPlan(gapless));
    runWhile(State::Preparing);
    EXPECT_EQ(m_pDirector->state(), State::Completed);
    EXPECT_TRUE(m_pDirector->report().mixed);
    EXPECT_EQ(m_pDirector->report().durationMs, 0.0);
    EXPECT_EQ(get(kDeck2, QStringLiteral("play")), 1.0);
    EXPECT_EQ(get(kDeck1, QStringLiteral("play")), 0.0);
    EXPECT_EQ(get(kMaster, QStringLiteral("crossfader")), 1.0);
    // Started just before the outgoing track's end.
    EXPECT_NEAR(m_pDirector->report().startOutgoingMs,
            static_cast<double>(gapless.outgoingCueMs) - SmartMixDirector::kStartSafetyLeadMs,
            kTickMs);
}

} // namespace
