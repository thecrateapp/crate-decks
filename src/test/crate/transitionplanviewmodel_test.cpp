// The Smart Mix plan's timeline without widgets (plan V3.PR1b): where the
// real plans of F6.C-PR1 put their moments on the bars of real grids. The
// grids are the dev stack analyzer's (src/test/crate/data/mix-profiles/):
// the outgoing deck's as Decks imports it (CrateBeatsImporter, 124 BPM) and
// the incoming deck's as the server sent it (a beat map at 118.31 BPM). The
// plans' tracks and the profiles' are not the same ones, so the tests cue
// the tracks where they need a cue in range.

#include "crate/ui/widgets/transitionplanviewmodel.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <algorithm>
#include <cmath>

#include "audio/signalinfo.h"
#include "audio/streaminfo.h"
#include "crate/analysis/analysisimporter.h"
#include "crate/analysis/mixprofile.h"
#include "crate/smartmix/curves.h"
#include "crate/smartmix/smartmixdirector.h"
#include "crate/smartmix/transitionplan.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

using crate::TransitionPlan;
using crate::analysis::MixProfile;
using crate::widgets::PlanDeck;
using crate::widgets::PlanTrackCue;
using crate::widgets::TransitionPlanViewModel;
using Side = TransitionPlanViewModel::Side;
using MomentKind = TransitionPlanViewModel::MomentKind;
using TickKind = TransitionPlanViewModel::TickKind;

constexpr mixxx::audio::SampleRate::value_t kSampleRate = 44100;
// The anonymised uids of the plan fixtures' tracks
const QString kOutgoingUid = QStringLiteral("637bd7c0-380e-5bc7-b841-dc2234cdfaf7");
const QString kIncomingUid = QStringLiteral("34137088-fcab-5f93-8866-31f1bf6d0871");
const QString kNextUid = QStringLiteral("a89833d1-75e5-52ce-8766-6df5b057d027");

class CrateTransitionPlanViewModelTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    CrateTransitionPlanViewModelTest() {
        m_outgoingProfile = profile(QStringLiteral("real-full-whole-bpm.json"));
        m_incomingProfile = profile(QStringLiteral("real-full-fractional-bpm.json"));
        QFile tracks(dataPath(QStringLiteral("transition-plans/real-tracks.json")));
        EXPECT_TRUE(tracks.open(QIODevice::ReadOnly));
        m_durations = QJsonDocument::fromJson(tracks.readAll())
                              .object()
                              .value(QStringLiteral("durationMs"))
                              .toObject();
    }

    QString dataPath(const QString& name) const {
        return getTestDir().filePath(QStringLiteral("crate/data/") + name);
    }

    MixProfile profile(const QString& name) const {
        QFile file(dataPath(QStringLiteral("mix-profiles/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        const auto parsed = crate::analysis::parseMixProfile(file.readAll());
        EXPECT_TRUE(parsed.ok()) << name.toStdString();
        return parsed.ok() ? parsed.value() : MixProfile();
    }

    TransitionPlan plan(const QString& name) const {
        QFile file(dataPath(QStringLiteral("transition-plans/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        const auto parsed = TransitionPlan::fromJson(file.readAll());
        EXPECT_TRUE(parsed.ok()) << name.toStdString();
        return parsed.ok() ? parsed.value() : TransitionPlan();
    }

    double durationMs(const QString& uid) const {
        return m_durations.value(uid).toDouble();
    }

    /// The outgoing grid as Decks imports it: constant, on the downbeat
    mixxx::BeatsPointer outgoingGrid() const {
        const auto grid = crate::analysis::checkBeatGrid(m_outgoingProfile);
        EXPECT_TRUE(grid.passed) << grid.reason.toStdString();
        crate::analysis::CrateBeatsImporter importer(grid, m_outgoingProfile.profileRevision);
        return importer.importBeatsAndApplyTimingOffset(QString(),
                QString(),
                mixxx::audio::StreamInfo(mixxx::audio::SignalInfo(
                        mixxx::audio::ChannelCount::stereo(),
                        mixxx::audio::SampleRate(kSampleRate))));
    }

    /// The incoming grid as the server sent it: every beat
    mixxx::BeatsPointer incomingGrid() const {
        QVector<mixxx::audio::FramePos> beats;
        for (const qint64 beatMs : m_incomingProfile.beatGridMs) {
            beats.append(mixxx::audio::FramePos(std::round(beatMs * kSampleRate / 1000.0)));
        }
        return mixxx::Beats::fromBeatPositions(mixxx::audio::SampleRate(kSampleRate), beats);
    }

    /// The Intro and Outro AnalysisImporter places from a profile
    static QList<PlanTrackCue> profileCues(const MixProfile& profile) {
        QList<PlanTrackCue> cues;
        PlanTrackCue intro;
        intro.type = PlanTrackCue::Type::Intro;
        intro.startMs = static_cast<double>(profile.activeStartMs.value_or(0));
        intro.endMs = static_cast<double>(*profile.introCueMs);
        cues.append(intro);
        PlanTrackCue outro;
        outro.type = PlanTrackCue::Type::Outro;
        outro.startMs = static_cast<double>(*profile.outroCueMs);
        outro.endMs = static_cast<double>(*profile.activeEndMs);
        cues.append(outro);
        return cues;
    }

    static PlanTrackCue cue(PlanTrackCue::Type type, double startMs, int number = 0) {
        PlanTrackCue cue;
        cue.type = type;
        cue.startMs = startMs;
        cue.number = number;
        return cue;
    }

    PlanDeck outgoingDeck(double durationMs) const {
        PlanDeck deck;
        deck.group = QStringLiteral("[Channel1]");
        deck.title = QStringLiteral("Lattice");
        deck.durationMs = durationMs;
        deck.pBeats = outgoingGrid();
        deck.bpm = *m_outgoingProfile.bpm;
        deck.cues = profileCues(m_outgoingProfile);
        return deck;
    }

    PlanDeck incomingDeck(double durationMs) const {
        PlanDeck deck;
        deck.group = QStringLiteral("[Channel2]");
        deck.title = QStringLiteral("Northbound Static");
        deck.durationMs = durationMs;
        deck.pBeats = incomingGrid();
        deck.bpm = *m_incomingProfile.bpm;
        deck.cues = profileCues(m_incomingProfile);
        return deck;
    }

    /// The outgoing bar at `ms`, from the profile's tempo and downbeat alone
    int outgoingBar(double ms) const {
        const double beatMs = 60000.0 / *m_outgoingProfile.bpm;
        const double beats =
                (ms -
                        static_cast<double>(
                                *m_outgoingProfile.downbeatAnchorMs)) /
                beatMs;
        return static_cast<int>(std::floor(beats / 4.0)) + 1;
    }

    /// The incoming bar at `ms`, counting the profile's beats
    int incomingBar(double ms) const {
        const QVector<qint64>& beats = m_incomingProfile.beatGridMs;
        const auto after = std::upper_bound(beats.begin(), beats.end(), static_cast<qint64>(ms));
        const int index = static_cast<int>(after - beats.begin()) - 1;
        return index / 4 + 1;
    }

    static const TransitionPlanViewModel::Moment* moment(
            const TransitionPlanViewModel& model, Side side, const QString& label) {
        for (const auto& moment : model.moments()) {
            if (moment.side == side && moment.label == label) {
                return &moment;
            }
        }
        return nullptr;
    }

    static QStringList labels(const TransitionPlanViewModel& model) {
        QStringList labels;
        for (const auto& moment : model.moments()) {
            labels.append(moment.label);
        }
        return labels;
    }

    MixProfile m_outgoingProfile;
    MixProfile m_incomingProfile;
    QJsonObject m_durations;
};

// The assistant's long blend (96 s, dj-blend, bass handoff at 0.5): the
// window where the director plays it and its length in the outgoing deck's
// bars, the moments on both rulers with their bars, and the rulers' beats
// and bars.
TEST_F(CrateTransitionPlanViewModelTest, MapsPlanToSegments) {
    const TransitionPlan longBlend = plan(QStringLiteral("synthetic-assist-long-blend.json"));
    const double outgoingMs = durationMs(kOutgoingUid);
    PlanDeck outgoing = outgoingDeck(outgoingMs);
    // A track whose outro starts where the plan leaves it, and a hotcue on
    // its exit
    outgoing.cues.append(cue(PlanTrackCue::Type::Outro, 330298.0));
    outgoing.cues.append(cue(PlanTrackCue::Type::HotCue, 426298.0 + 120.0, 4));
    PlanDeck incoming = incomingDeck(durationMs(kIncomingUid));
    incoming.cues.append(cue(PlanTrackCue::Type::HotCue, 32600.0, 3));
    TransitionPlanViewModel model;
    QSignalSpy changed(&model, &TransitionPlanViewModel::changed);
    model.setPlan(longBlend, outgoing, incoming);
    EXPECT_EQ(1, changed.count());
    ASSERT_TRUE(model.hasPlan());

    // The window: where the director starts it, 96 s on
    const double start = crate::SmartMixDirector::startAtMs(longBlend, outgoingMs);
    EXPECT_DOUBLE_EQ(330298.0, start);
    EXPECT_DOUBLE_EQ(start, model.windowStartMs());
    EXPECT_DOUBLE_EQ(start + 96000.0, model.windowEndMs());
    // 96 s at 124 BPM: 49.6 bars
    ASSERT_TRUE(model.windowBars());
    EXPECT_EQ(50, *model.windowBars());
    EXPECT_EQ(QStringLiteral("Transition · 50 bars"), model.windowLabel());

    // The range: half the window before it and a quarter after, on bar lines
    EXPECT_LE(model.rangeStartMs(), start - 48000.0);
    EXPECT_GT(model.rangeStartMs(), start - 48000.0 - 4 * 60000.0 / 124.0);
    EXPECT_GE(model.rangeEndMs(), model.windowEndMs() + 24000.0);
    const auto first = model.barAt(Side::Outgoing, model.rangeStartMs());
    ASSERT_TRUE(first);
    EXPECT_EQ(outgoingBar(model.rangeStartMs() + 1.0), *first);

    // The moments, by side and time
    EXPECT_EQ((QStringList{QStringLiteral("Outro in"),
                      QStringLiteral("Exit · hotcue 4"),
                      QStringLiteral("Entry · intro cue"),
                      QStringLiteral("Hotcue 3")}),
            labels(model));
    const auto* pOutro = moment(model, Side::Outgoing, QStringLiteral("Outro in"));
    ASSERT_NE(nullptr, pOutro);
    EXPECT_EQ(MomentKind::Track, pOutro->kind);
    EXPECT_DOUBLE_EQ(330298.0, pOutro->atMs);
    ASSERT_TRUE(pOutro->bar);
    EXPECT_EQ(outgoingBar(330298.0), *pOutro->bar);
    EXPECT_EQ(171, *pOutro->bar);

    const auto* pExit = moment(model, Side::Outgoing, QStringLiteral("Exit · hotcue 4"));
    ASSERT_NE(nullptr, pExit);
    EXPECT_EQ(MomentKind::Plan, pExit->kind);
    EXPECT_DOUBLE_EQ(model.windowEndMs(), pExit->atMs);
    EXPECT_EQ(outgoingBar(model.windowEndMs()), pExit->bar);
    EXPECT_EQ(*pOutro->bar + 50, *pExit->bar);

    // The incoming deck enters on its cue plus the phase offset, which the
    // plan put on the track's Intro (its end, the analyzer's intro cue)
    const auto* pEntry = moment(model, Side::Incoming, QStringLiteral("Entry · intro cue"));
    ASSERT_NE(nullptr, pEntry);
    EXPECT_EQ(MomentKind::Entry, pEntry->kind);
    EXPECT_DOUBLE_EQ(start, pEntry->atMs);
    EXPECT_DOUBLE_EQ(static_cast<double>(longBlend.startIncomingCueMs()),
            model.incomingPositionAt(start));
    EXPECT_EQ(incomingBar(static_cast<double>(longBlend.startIncomingCueMs())), pEntry->bar);
    EXPECT_EQ(1, pEntry->bar);

    // Its hotcue at its own time, at its tempo against the outgoing deck's
    const auto* pHotcue = moment(model, Side::Incoming, QStringLiteral("Hotcue 3"));
    ASSERT_NE(nullptr, pHotcue);
    EXPECT_EQ(MomentKind::Track, pHotcue->kind);
    EXPECT_NEAR(start + (32600.0 - 503.0) / longBlend.incomingTempoRatio, pHotcue->atMs, 1e-6);
    EXPECT_NEAR(32600.0, model.incomingPositionAt(pHotcue->atMs), 1e-6);
    EXPECT_EQ(incomingBar(32600.0), pHotcue->bar);

    // The outgoing ruler: every beat of the range up to the track's end, a
    // bar every 4, numbered as the grid
    const auto& outgoingTicks = model.ticks(Side::Outgoing);
    ASSERT_FALSE(outgoingTicks.isEmpty());
    EXPECT_LE(outgoingTicks.last().atMs, outgoingMs);
    // From a bar line 24 bars or more before the window to the track's end,
    // 97 s later
    EXPECT_GE(outgoingTicks.size(), 4 * 74);
    for (int index = 0; index < outgoingTicks.size(); ++index) {
        const auto& tick = outgoingTicks[index];
        EXPECT_GE(tick.atMs, model.rangeStartMs() - 1e-6);
        EXPECT_EQ(outgoingBar(tick.atMs + 1.0), tick.bar);
        EXPECT_EQ(index % 4 == 0, tick.kind == TickKind::Bar) << index;
    }
    // The incoming ruler starts at the entry, in the track's first bar
    const auto& incomingTicks = model.ticks(Side::Incoming);
    ASSERT_FALSE(incomingTicks.isEmpty());
    EXPECT_GE(incomingTicks.first().atMs, start);
    EXPECT_EQ(1, incomingTicks.first().bar);
    for (const auto& tick : incomingTicks) {
        EXPECT_EQ(incomingBar(model.incomingPositionAt(tick.atMs) + 1.0), tick.bar);
    }
}

// The bass swap is where curves::bassCutDb() crosses the basses: at the
// plan's handoffProgress of the window, over the swap's eighth of it.
TEST_F(CrateTransitionPlanViewModelTest, PlacesTheBassSwapAtHandoffProgress) {
    const TransitionPlan longBlend = plan(QStringLiteral("synthetic-assist-long-blend.json"));
    TransitionPlanViewModel model;
    model.setPlan(longBlend,
            outgoingDeck(durationMs(kOutgoingUid)),
            incomingDeck(durationMs(kIncomingUid)));
    ASSERT_TRUE(longBlend.hasBassHandoff());
    const auto& swap = model.bassSwap();
    ASSERT_TRUE(swap);
    const double windowMs = model.windowEndMs() - model.windowStartMs();
    EXPECT_NEAR(model.windowStartMs() + longBlend.handoffProgress * windowMs, swap->atMs, 1.0);
    EXPECT_NEAR(crate::curves::kBassSwapFraction * windowMs, swap->endMs - swap->startMs, 1.0);
    EXPECT_NEAR(swap->atMs, (swap->startMs + swap->endMs) / 2.0, 1.0);
    ASSERT_TRUE(swap->bar);
    EXPECT_EQ(outgoingBar(swap->atMs), *swap->bar);
    EXPECT_EQ(196, *swap->bar);
    EXPECT_EQ(QStringLiteral("Bass swap · bar 196"), model.bassSwapLabel());

    // Without a handoff, no swap
    TransitionPlan noHandoff = longBlend;
    noHandoff.bassHandoff = TransitionPlan::BassHandoff::None;
    model.setPlan(noHandoff,
            outgoingDeck(durationMs(kOutgoingUid)),
            incomingDeck(durationMs(kIncomingUid)));
    EXPECT_FALSE(model.bassSwap());
    EXPECT_TRUE(model.bassSwapLabel().isEmpty());
}

// The tempo and the levels are the director's: the incoming deck at the
// plan's ratio of the outgoing tempo until the exit, then back to its own
// along the tempo restore; both decks' levels along the curve, with their
// staging gains.
TEST_F(CrateTransitionPlanViewModelTest, TempoAndGainAtTheEnds) {
    const TransitionPlan longBlend = plan(QStringLiteral("synthetic-assist-long-blend.json"));
    PlanDeck outgoing = outgoingDeck(durationMs(kOutgoingUid));
    PlanDeck incoming = incomingDeck(durationMs(kIncomingUid));
    // The DJ had the incoming deck a little faster
    incoming.rate = 1.01;
    TransitionPlanViewModel model;
    model.setPlan(longBlend, outgoing, incoming);

    const auto& tempo = model.tempo();
    ASSERT_TRUE(tempo);
    const double bpm = *m_incomingProfile.bpm;
    // The grid's tempo around the entry: its beats are on whole
    // milliseconds, so it is the profile's within 0.3 %
    EXPECT_NEAR(bpm * longBlend.incomingTempoRatio, tempo->fromBpm, bpm * 0.003);
    EXPECT_NEAR(bpm * 1.01, tempo->toBpm, bpm * 0.003);
    EXPECT_NEAR(longBlend.incomingTempoRatio / 1.01, tempo->fromBpm / tempo->toBpm, 1e-9);
    EXPECT_TRUE(tempo->changes());
    ASSERT_GE(tempo->points.size(), 3);
    EXPECT_DOUBLE_EQ(model.windowStartMs(), tempo->points.first().atMs);
    EXPECT_DOUBLE_EQ(tempo->fromBpm, tempo->points.first().value);
    EXPECT_DOUBLE_EQ(model.windowEndMs(), tempo->points[1].atMs);
    EXPECT_DOUBLE_EQ(tempo->fromBpm, tempo->points[1].value);
    EXPECT_DOUBLE_EQ(model.windowEndMs() + crate::curves::kTempoRestoreMs,
            tempo->points.last().atMs);
    EXPECT_NEAR(tempo->toBpm, tempo->points.last().value, 1e-9);
    // Halfway through the restore, halfway between both tempos
    const auto& middle = tempo->points[1 + TransitionPlanViewModel::kCurvePoints / 2];
    EXPECT_NEAR((tempo->fromBpm + tempo->toBpm) / 2.0, middle.value, 1e-9);

    const auto& levels = model.levels();
    ASSERT_TRUE(levels);
    ASSERT_EQ(TransitionPlanViewModel::kCurvePoints + 1, levels->incoming.size());
    EXPECT_DOUBLE_EQ(1.0, levels->outgoing.first().value);
    EXPECT_DOUBLE_EQ(0.0, levels->outgoing.last().value);
    EXPECT_DOUBLE_EQ(0.0, levels->incoming.first().value);
    EXPECT_DOUBLE_EQ(1.0, levels->incoming.last().value);
    EXPECT_DOUBLE_EQ(model.windowEndMs(), levels->incoming.last().atMs);
    // The dj-blend holds both decks at full blend in its middle, each at
    // its staging gain
    const auto& half = levels->incoming[TransitionPlanViewModel::kCurvePoints / 2];
    EXPECT_DOUBLE_EQ(crate::curves::transitionGain(
                             crate::curves::Role::Incoming, 0.5, longBlend.incomingGainDb, true),
            half.value);
    EXPECT_EQ(QStringLiteral("A −4.3 dB"), model.gainLabel(Side::Outgoing));
    EXPECT_EQ(QStringLiteral("B −3.8 dB"), model.gainLabel(Side::Incoming));
    ASSERT_TRUE(model.beatmatch());
    EXPECT_DOUBLE_EQ(model.windowStartMs(), model.beatmatch()->first);
    EXPECT_DOUBLE_EQ(model.windowEndMs(), model.beatmatch()->second);

    // After the exit the incoming deck runs along the restore: the ruler
    // follows it
    const double afterRestore = model.windowEndMs() + 2.0 * crate::curves::kTempoRestoreMs;
    const double atExit = model.incomingPositionAt(model.windowEndMs());
    EXPECT_NEAR(atExit + (longBlend.incomingTempoRatio + 1.01) / 2.0 * 4000.0 + 1.01 * 4000.0,
            model.incomingPositionAt(afterRestore),
            1e-6);
    EXPECT_NEAR(afterRestore,
            model.atMsOfIncoming(model.incomingPositionAt(afterRestore)),
            1e-3);
}

// The real short beatmatch: 8 s are 4 bars at 124 BPM, without a bass
// handoff or a long blend.
TEST_F(CrateTransitionPlanViewModelTest, ShortBeatmatchIsFourBars) {
    const TransitionPlan beatmatch = plan(QStringLiteral("real-beatmatch.json"));
    TransitionPlanViewModel model;
    model.setPlan(beatmatch,
            outgoingDeck(durationMs(kOutgoingUid)),
            incomingDeck(durationMs(kIncomingUid)));
    EXPECT_EQ(4, model.windowBars());
    EXPECT_EQ(QStringLiteral("Transition · 4 bars"), model.windowLabel());
    EXPECT_FALSE(model.bassSwap());
    ASSERT_TRUE(model.beatmatch());
    // At least 8 bars before and 4 after the window
    EXPECT_LE(model.rangeStartMs(), model.windowStartMs() - 8 * 4 * 60000.0 / 124.0 + 1.0);
    EXPECT_GE(model.rangeEndMs(), model.windowEndMs() + 4 * 4 * 60000.0 / 124.0 - 1.0);
}

// A plan the server fell back on (adaptive, incompatible tempo) draws as a
// plain fade: no tempo change, no bass swap, no beatmatch, equal power.
TEST_F(CrateTransitionPlanViewModelTest, FallbackPlanRendersAsSimpleFade) {
    const TransitionPlan fallback = plan(QStringLiteral("real-adaptive-fallback.json"));
    ASSERT_TRUE(fallback.fallbackReason);
    PlanDeck outgoing = outgoingDeck(durationMs(kIncomingUid));
    PlanDeck incoming = incomingDeck(durationMs(kNextUid));
    incoming.rate = 0.98;
    TransitionPlanViewModel model;
    model.setPlan(fallback, outgoing, incoming);

    EXPECT_TRUE(model.fellBack());
    EXPECT_FALSE(model.bassSwap());
    EXPECT_FALSE(model.beatmatch());
    const auto& tempo = model.tempo();
    ASSERT_TRUE(tempo);
    EXPECT_FALSE(tempo->changes());
    // The grid's tempo where the deck enters: its beats are on whole
    // milliseconds, so it is the profile's within 0.3 %
    EXPECT_NEAR(*m_incomingProfile.bpm * 0.98, tempo->fromBpm, *m_incomingProfile.bpm * 0.003);
    EXPECT_EQ(2, tempo->points.size());
    const auto& levels = model.levels();
    ASSERT_TRUE(levels);
    const int half = TransitionPlanViewModel::kCurvePoints / 2;
    EXPECT_DOUBLE_EQ(crate::curves::transitionGain(
                             crate::curves::Role::Outgoing, 0.5, fallback.outgoingGainDb, false),
            levels->outgoing[half].value);
    // The incoming deck at its own rate, against the outgoing one's
    EXPECT_NEAR(0.98,
            (model.incomingPositionAt(model.windowEndMs()) -
                    model.incomingPositionAt(model.windowStartMs())) /
                    (model.windowEndMs() - model.windowStartMs()),
            1e-9);
    EXPECT_EQ(QStringLiteral("Transition · 4 bars"), model.windowLabel());
}

// Gapless: the incoming deck starts as the outgoing one ends, 50 ms before
// its end, as the director plays it; nothing blends.
TEST_F(CrateTransitionPlanViewModelTest, GaplessHasNoBlend) {
    const TransitionPlan gapless = plan(QStringLiteral("real-gapless.json"));
    const double outgoingMs = durationMs(kOutgoingUid);
    TransitionPlanViewModel model;
    model.setPlan(gapless, outgoingDeck(outgoingMs), incomingDeck(durationMs(kIncomingUid)));
    EXPECT_DOUBLE_EQ(outgoingMs - crate::SmartMixDirector::kStartSafetyLeadMs,
            model.windowStartMs());
    EXPECT_DOUBLE_EQ(model.windowStartMs(), model.windowEndMs());
    EXPECT_EQ(QStringLiteral("Gapless"), model.windowLabel());
    EXPECT_FALSE(model.levels());
    EXPECT_FALSE(model.tempo());
    EXPECT_FALSE(model.bassSwap());
    EXPECT_FALSE(model.beatmatch());
    ASSERT_NE(nullptr, moment(model, Side::Outgoing, QStringLiteral("Exit")));
    // The outgoing ruler stops at the track's end
    EXPECT_LE(model.ticks(Side::Outgoing).last().atMs, outgoingMs);
}

// What the data lacks is left out rather than invented: without a grid
// there are no bars, without an Intro no "Intro end", cues out of range or
// before the incoming deck plays are not shown, and nothing names a drop.
TEST_F(CrateTransitionPlanViewModelTest, LeavesOutWhatTheDataLacks) {
    const TransitionPlan longBlend = plan(QStringLiteral("synthetic-assist-long-blend.json"));
    PlanDeck outgoing = outgoingDeck(durationMs(kOutgoingUid));
    outgoing.pBeats.reset();
    outgoing.cues.append(cue(PlanTrackCue::Type::HotCue, 10000.0, 1));
    PlanDeck incoming = incomingDeck(durationMs(kIncomingUid));
    incoming.cues.clear();
    // Before the entry, and after the track's end
    incoming.cues.append(cue(PlanTrackCue::Type::HotCue, 100.0, 2));
    incoming.cues.append(cue(PlanTrackCue::Type::HotCue, 200000.0, 5));
    TransitionPlanViewModel model;
    model.setPlan(longBlend, outgoing, incoming);

    EXPECT_FALSE(model.windowBars());
    EXPECT_EQ(QStringLiteral("Transition · 96.0 s"), model.windowLabel());
    EXPECT_TRUE(model.ticks(Side::Outgoing).isEmpty());
    EXPECT_FALSE(model.ticks(Side::Incoming).isEmpty());
    EXPECT_EQ(QStringLiteral("Bass swap"), model.bassSwapLabel());
    EXPECT_EQ((QStringList{QStringLiteral("Exit"), QStringLiteral("Entry")}), labels(model));
    for (const auto& moment : model.moments()) {
        EXPECT_EQ(moment.side == Side::Incoming, moment.bar.has_value())
                << moment.label.toStdString();
        EXPECT_FALSE(moment.label.contains(QStringLiteral("Drop"), Qt::CaseInsensitive));
    }
    // The range still frames the window, by time
    EXPECT_LT(model.rangeStartMs(), model.windowStartMs());
    EXPECT_GT(model.rangeEndMs(), model.windowEndMs());
    model.setOutgoingPositionMs(300000.0);
    EXPECT_EQ(QStringLiteral("Now"), model.nowLabel());
}

// Now follows the outgoing deck, with its bar, and says so only when it
// moves.
TEST_F(CrateTransitionPlanViewModelTest, NowFollowsTheOutgoingDeck) {
    TransitionPlanViewModel model;
    model.setPlan(plan(QStringLiteral("synthetic-assist-long-blend.json")),
            outgoingDeck(durationMs(kOutgoingUid)),
            incomingDeck(durationMs(kIncomingUid)));
    QSignalSpy now(&model, &TransitionPlanViewModel::nowChanged);
    QSignalSpy changed(&model, &TransitionPlanViewModel::changed);
    model.setOutgoingPositionMs(330298.0 - 7700.0);
    model.setOutgoingPositionMs(330298.0 - 7700.0);
    EXPECT_EQ(1, now.count());
    EXPECT_EQ(0, changed.count());
    EXPECT_DOUBLE_EQ(330298.0 - 7700.0, model.nowMs());
    ASSERT_TRUE(model.nowBar());
    EXPECT_EQ(outgoingBar(330298.0 - 7700.0), *model.nowBar());
    EXPECT_EQ(QStringLiteral("Now · bar %1").arg(*model.nowBar()), model.nowLabel());

    model.clear();
    EXPECT_FALSE(model.hasPlan());
    EXPECT_EQ(1, changed.count());
    EXPECT_TRUE(model.moments().isEmpty());
}

// A deck read from its track: the grid, the BPM, the Intro and Outro that
// AnalysisImporter places from the real profile, and the hotcues, in ms.
TEST_F(CrateTransitionPlanViewModelTest, ReadsTheDeckFromItsTrack) {
    // As a Crate track loads: the profile applied, then the file opened
    const TrackPointer pTrack = Track::newTemporary(
            getTestDir().filePath(QStringLiteral("id3-test-data/cover-test-øé~ł€˚.flac")));
    crate::analysis::Policy policy;
    policy.crateBeats = true;
    crate::analysis::AnalysisImporter::apply(pTrack, m_outgoingProfile, policy);
    {
        SoundSourceProxy proxy(pTrack);
        ASSERT_TRUE(proxy.openAudioSource());
    }
    const double perMs = pTrack->getSampleRate().toDouble() / 1000.0;
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            2,
            mixxx::audio::FramePos(60000.0 * perMs),
            mixxx::audio::kInvalidFramePos);

    const PlanDeck deck = PlanDeck::fromTrack(QStringLiteral("[Channel2]"), pTrack, 205179.0, 1.02);
    EXPECT_EQ(QStringLiteral("[Channel2]"), deck.group);
    EXPECT_FALSE(deck.title.isEmpty());
    EXPECT_DOUBLE_EQ(205179.0, deck.durationMs);
    EXPECT_DOUBLE_EQ(1.02, deck.rate);
    ASSERT_NE(nullptr, deck.pBeats);
    EXPECT_NEAR(124.0, deck.bpm, 1e-6);
    // Bar 1 on the profile's downbeat
    const auto index = crate::widgets::beatIndexAt(
            deck.pBeats, static_cast<double>(*m_outgoingProfile.downbeatAnchorMs));
    ASSERT_TRUE(index);
    EXPECT_NEAR(0.0, *index, 0.01);

    const auto find = [&deck](PlanTrackCue::Type type) -> std::optional<PlanTrackCue> {
        for (const PlanTrackCue& cue : deck.cues) {
            if (cue.type == type) {
                return cue;
            }
        }
        return std::nullopt;
    };
    const auto intro = find(PlanTrackCue::Type::Intro);
    ASSERT_TRUE(intro && intro->endMs);
    EXPECT_NEAR(static_cast<double>(*m_outgoingProfile.introCueMs), *intro->endMs, 0.1);
    const auto outro = find(PlanTrackCue::Type::Outro);
    ASSERT_TRUE(outro && outro->startMs);
    EXPECT_NEAR(static_cast<double>(*m_outgoingProfile.outroCueMs), *outro->startMs, 0.1);
    const auto hotcue = find(PlanTrackCue::Type::HotCue);
    ASSERT_TRUE(hotcue && hotcue->startMs);
    EXPECT_EQ(3, hotcue->number);
    EXPECT_NEAR(60000.0, *hotcue->startMs, 0.1);
    EXPECT_FALSE(hotcue->endMs);
}

} // namespace
