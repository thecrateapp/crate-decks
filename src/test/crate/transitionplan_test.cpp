#include "crate/smartmix/transitionplan.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "test/mixxxtest.h"

namespace {

using crate::TransitionPlan;

// The anonymised uids of the real fixtures (src/test/crate/data/transition-plans/).
const QString kBeatmatchOutgoing = QStringLiteral("637bd7c0-380e-5bc7-b841-dc2234cdfaf7");
const QString kBeatmatchIncoming = QStringLiteral("34137088-fcab-5f93-8866-31f1bf6d0871");
const QString kFallbackIncoming = QStringLiteral("a89833d1-75e5-52ce-8766-6df5b057d027");

class CrateTransitionPlanTest : public MixxxTest {
  protected:
    QByteArray fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/transition-plans/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return file.readAll();
    }

    QJsonObject object(const QString& name) const {
        return QJsonDocument::fromJson(fixture(name)).object();
    }

    /// The profile duration of a fixture track (real-tracks.json).
    qint64 durationOf(const QString& uid) const {
        return object(QStringLiteral("real-tracks.json"))
                .value(QStringLiteral("durationMs"))
                .toObject()
                .value(uid)
                .toInteger();
    }

    TransitionPlan parse(const QString& name) const {
        const auto result = TransitionPlan::fromJson(fixture(name));
        EXPECT_TRUE(result.ok()) << (result.ok() ? std::string()
                                                 : result.error().message.toStdString());
        return result.ok() ? result.value() : TransitionPlan();
    }

    /// The problem fromJson() finds in `name` with `key` set to `value`.
    QString parseError(const QString& name, const QString& key, const QJsonValue& value) const {
        QJsonObject plan = object(name);
        plan.insert(key, value);
        const auto result = TransitionPlan::fromJson(plan);
        EXPECT_FALSE(result.ok()) << key.toStdString();
        return result.ok() ? QString() : result.error().message;
    }

    std::optional<QString> validateBeatmatchEdge(const TransitionPlan& plan) const {
        return plan.validate(durationOf(kBeatmatchOutgoing), durationOf(kBeatmatchIncoming));
    }
};

TEST_F(CrateTransitionPlanTest, ParsesAllModes) {
    const TransitionPlan beatmatch = parse(QStringLiteral("real-beatmatch.json"));
    EXPECT_EQ(TransitionPlan::Mode::Beatmatch, beatmatch.mode);
    EXPECT_EQ(kBeatmatchOutgoing, beatmatch.outgoingTrackEntityUid);
    EXPECT_EQ(kBeatmatchIncoming, beatmatch.incomingTrackEntityUid);
    EXPECT_EQ(8000, beatmatch.durationMs);
    EXPECT_EQ(418135, beatmatch.outgoingCueMs);
    EXPECT_EQ(198, beatmatch.incomingCueMs);
    EXPECT_DOUBLE_EQ(1.023355, beatmatch.incomingTempoRatio);
    EXPECT_EQ(298, beatmatch.beatPhaseOffsetMs);
    EXPECT_DOUBLE_EQ(0.5, beatmatch.handoffProgress);
    EXPECT_DOUBLE_EQ(-4.2867, beatmatch.outgoingGainDb);
    EXPECT_DOUBLE_EQ(-3.7616, beatmatch.incomingGainDb);
    EXPECT_EQ(TransitionPlan::Curve::EqualPower, beatmatch.curve);
    EXPECT_EQ(TransitionPlan::BassHandoff::None, beatmatch.bassHandoff);
    EXPECT_DOUBLE_EQ(0.6761, beatmatch.confidence);
    EXPECT_FALSE(beatmatch.fallbackReason.has_value());
    EXPECT_EQ(std::optional<bool>(false), beatmatch.structureUsed);
    EXPECT_FALSE(beatmatch.stemsUsed);
    // The dev stack's server predates S5 and sends no limits.
    EXPECT_FALSE(beatmatch.policyLimits.has_value());
    // The phase offset is output time: at the incoming tempo, 298 ms are
    // 305 ms of the incoming track, added to its cue (never subtracted).
    EXPECT_EQ(198 + 305, beatmatch.startIncomingCueMs());
    EXPECT_EQ(std::nullopt, validateBeatmatchEdge(beatmatch));

    const TransitionPlan adaptive = parse(QStringLiteral("real-adaptive.json"));
    EXPECT_EQ(TransitionPlan::Mode::Adaptive, adaptive.mode);
    EXPECT_EQ(350, adaptive.durationMs);
    EXPECT_DOUBLE_EQ(1.0, adaptive.incomingTempoRatio);
    EXPECT_EQ(adaptive.incomingCueMs, adaptive.startIncomingCueMs());
    EXPECT_FALSE(adaptive.fallbackReason.has_value());
    EXPECT_EQ(std::nullopt, validateBeatmatchEdge(adaptive));

    const TransitionPlan gapless = parse(QStringLiteral("real-gapless.json"));
    EXPECT_EQ(TransitionPlan::Mode::Gapless, gapless.mode);
    EXPECT_EQ(0, gapless.durationMs);
    // The incoming track starts where the outgoing one ends.
    EXPECT_EQ(durationOf(kBeatmatchOutgoing), gapless.outgoingCueMs);
    EXPECT_EQ(0, gapless.startIncomingCueMs());
    EXPECT_DOUBLE_EQ(1.0, gapless.confidence);
    EXPECT_EQ(std::nullopt, validateBeatmatchEdge(gapless));
}

TEST_F(CrateTransitionPlanTest, RejectsOutOfRangeTempo) {
    const QString beatmatch = QStringLiteral("real-beatmatch.json");
    const QString ratio = QStringLiteral("incomingTempoRatio");
    EXPECT_EQ(QStringLiteral("plan.incomingTempoRatio is out of range"),
            parseError(beatmatch, ratio, 1.07));
    EXPECT_EQ(QStringLiteral("plan.incomingTempoRatio is out of range"),
            parseError(beatmatch, ratio, 0.9399));
    EXPECT_EQ(QStringLiteral("plan.incomingTempoRatio is not a number"),
            parseError(beatmatch, ratio, QStringLiteral("1.02")));

    // The bounds themselves are allowed.
    TransitionPlan plan = parse(beatmatch);
    plan.incomingTempoRatio = TransitionPlan::kMaxTempoRatio;
    EXPECT_EQ(std::nullopt, validateBeatmatchEdge(plan));
    plan.incomingTempoRatio = TransitionPlan::kMinTempoRatio;
    EXPECT_EQ(std::nullopt, validateBeatmatchEdge(plan));
    plan.incomingTempoRatio = 1.0601;
    EXPECT_EQ(QStringLiteral("plan.incomingTempoRatio is out of range"),
            validateBeatmatchEdge(plan));

    // Outside beatmatch the tempo never changes.
    TransitionPlan adaptive = parse(QStringLiteral("real-adaptive.json"));
    adaptive.incomingTempoRatio = 1.02;
    EXPECT_EQ(QStringLiteral("plan.incomingTempoRatio changes the tempo outside beatmatch"),
            validateBeatmatchEdge(adaptive));
}

TEST_F(CrateTransitionPlanTest, RejectsCueBeyondDuration) {
    const qint64 outgoingDurationMs = durationOf(kBeatmatchOutgoing);
    const qint64 incomingDurationMs = durationOf(kBeatmatchIncoming);
    const TransitionPlan plan = parse(QStringLiteral("real-beatmatch.json"));

    // The transition may end exactly at the end of the outgoing track.
    TransitionPlan outgoing = plan;
    outgoing.outgoingCueMs = outgoingDurationMs - plan.durationMs;
    EXPECT_EQ(std::nullopt, outgoing.validate(outgoingDurationMs, incomingDurationMs));
    ++outgoing.outgoingCueMs;
    EXPECT_EQ(QStringLiteral(
                      "plan.outgoingCueMs leaves the transition past the end of the "
                      "outgoing track"),
            outgoing.validate(outgoingDurationMs, incomingDurationMs));

    // The incoming track plays the transition at its tempo ratio: 8000 ms of
    // output are 8187 ms of it.
    const qint64 incomingNeededMs = plan.incomingCueMs + 8187;
    EXPECT_EQ(std::nullopt, plan.validate(outgoingDurationMs, incomingNeededMs));
    EXPECT_EQ(QStringLiteral(
                      "plan.incomingCueMs leaves the transition past the end of the "
                      "incoming track"),
            plan.validate(outgoingDurationMs, plan.incomingCueMs + plan.durationMs));

    // Without the durations, nothing can be checked.
    EXPECT_EQ(QStringLiteral("The durations of the tracks are unknown"),
            plan.validate(outgoingDurationMs, 0));

    // A cue the server would never send is rejected while parsing.
    EXPECT_EQ(QStringLiteral("plan.outgoingCueMs is negative"),
            parseError(QStringLiteral("real-beatmatch.json"),
                    QStringLiteral("outgoingCueMs"),
                    -1));
}

TEST_F(CrateTransitionPlanTest, ParsesFallbackReason) {
    const TransitionPlan plan = parse(QStringLiteral("real-adaptive-fallback.json"));
    EXPECT_EQ(TransitionPlan::Mode::Adaptive, plan.mode);
    EXPECT_EQ(kBeatmatchIncoming, plan.outgoingTrackEntityUid);
    EXPECT_EQ(kFallbackIncoming, plan.incomingTrackEntityUid);
    ASSERT_TRUE(plan.fallbackReason.has_value());
    EXPECT_EQ(TransitionPlan::FallbackReason::IncompatibleTempo, *plan.fallbackReason);
    EXPECT_EQ(QStringLiteral("incompatible_tempo"), crate::toString(*plan.fallbackReason));
    // A fallback is still a complete plan.
    EXPECT_EQ(std::nullopt,
            plan.validate(durationOf(kBeatmatchIncoming), durationOf(kFallbackIncoming)));

    const QString name = QStringLiteral("real-adaptive-fallback.json");
    QJsonObject withNull = object(name);
    withNull.insert(QStringLiteral("fallbackReason"), QJsonValue::Null);
    const auto parsed = TransitionPlan::fromJson(withNull);
    ASSERT_TRUE(parsed.ok());
    EXPECT_FALSE(parsed.value().fallbackReason.has_value());

    EXPECT_EQ(QStringLiteral("plan.fallbackReason is not a known reason"),
            parseError(name, QStringLiteral("fallbackReason"), QStringLiteral("made_up")));
}

TEST_F(CrateTransitionPlanTest, RejectsWhatTheServerNeverSends) {
    const QString name = QStringLiteral("real-beatmatch.json");
    EXPECT_EQ(QStringLiteral("plan.plannerVersion is not a supported plan version"),
            parseError(name, QStringLiteral("plannerVersion"), 3));
    EXPECT_EQ(QStringLiteral("plan.mode is not a known mode"),
            parseError(name, QStringLiteral("mode"), QStringLiteral("scratch")));
    EXPECT_EQ(QStringLiteral("plan.incomingTrackEntityUid is not a uid"),
            parseError(name, QStringLiteral("incomingTrackEntityUid"), QStringLiteral("42")));
    EXPECT_EQ(QStringLiteral("plan.confidence is out of range"),
            parseError(name, QStringLiteral("confidence"), 1.5));

    QJsonObject missing = object(name);
    missing.remove(QStringLiteral("durationMs"));
    const auto result = TransitionPlan::fromJson(missing);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(crate::Error::Kind::Decode, result.error().kind);
    EXPECT_EQ(QStringLiteral("plan.durationMs is missing"), result.error().message);
}

TEST_F(CrateTransitionPlanTest, KeepsThePolicyOfEachMode) {
    const TransitionPlan beatmatch = parse(QStringLiteral("real-beatmatch.json"));

    TransitionPlan longPlan = beatmatch;
    longPlan.durationMs = 12001;
    longPlan.outgoingCueMs = 400000;
    EXPECT_EQ(QStringLiteral("plan.durationMs is longer than the policy allows"),
            validateBeatmatchEdge(longPlan));
    // A long beatmatch is a dj-blend, up to 48 s.
    longPlan.curve = TransitionPlan::Curve::DjBlend;
    EXPECT_EQ(std::nullopt, validateBeatmatchEdge(longPlan));
    // The assistant may allow longer transitions.
    TransitionPlan::Limits assist;
    assist.maxTransitionMs = 16000;
    longPlan.curve = TransitionPlan::Curve::EqualPower;
    EXPECT_EQ(std::nullopt,
            longPlan.validate(
                    durationOf(kBeatmatchOutgoing), durationOf(kBeatmatchIncoming), assist));

    TransitionPlan adaptive = parse(QStringLiteral("real-adaptive.json"));
    adaptive.bassHandoff = TransitionPlan::BassHandoff::Balanced;
    EXPECT_EQ(QStringLiteral("plan.bassHandoff is set outside beatmatch"),
            validateBeatmatchEdge(adaptive));

    TransitionPlan gapless = parse(QStringLiteral("real-gapless.json"));
    gapless.durationMs = 1000;
    gapless.outgoingCueMs -= 1000;
    EXPECT_EQ(QStringLiteral("plan.durationMs overlaps a gapless plan"),
            validateBeatmatchEdge(gapless));
}

// The assist mode's long blend (S5), built from the server's code because no
// dev stack serves it yet: synthetic-assist-long-blend.json, from
// make_synthetic_assist_plan.py.
TEST_F(CrateTransitionPlanTest, TakesTheLongBlendCeilingFromTheServer) {
    const TransitionPlan plan = parse(QStringLiteral("synthetic-assist-long-blend.json"));
    ASSERT_TRUE(plan.policyLimits.has_value());
    EXPECT_EQ(96000, plan.policyLimits->maxLongBeatmatchMs);
    EXPECT_EQ(12000, plan.policyLimits->maxTransitionMs);
    EXPECT_DOUBLE_EQ(0.04, plan.policyLimits->maxTempoAdjustment);
    EXPECT_TRUE(plan.policyLimits->longBeatmatchEligible);
    EXPECT_EQ(TransitionPlan::Curve::DjBlend, plan.curve);
    EXPECT_EQ(96000, plan.durationMs);
    // At the server's ceiling.
    EXPECT_EQ(std::nullopt, validateBeatmatchEdge(plan));

    // One second more is beyond it, although it still fits the tracks.
    TransitionPlan longer = plan;
    longer.durationMs = 97000;
    longer.outgoingCueMs -= 1000;
    EXPECT_EQ(QStringLiteral("plan.durationMs is longer than the policy allows"),
            validateBeatmatchEdge(longer));

    // Only a long blend gets the ceiling; an equal-power one stays at 12 s.
    TransitionPlan equalPower = plan;
    equalPower.curve = TransitionPlan::Curve::EqualPower;
    EXPECT_EQ(QStringLiteral("plan.durationMs is longer than the policy allows"),
            validateBeatmatchEdge(equalPower));

    // Tracks the server says are not steady enough get no long blend.
    TransitionPlan unsteady = plan;
    unsteady.policyLimits->longBeatmatchEligible = false;
    EXPECT_EQ(QStringLiteral(
                      "plan.durationMs is a long blend of tracks whose tempo is not "
                      "steady enough"),
            validateBeatmatchEdge(unsteady));

    // The server's tempo limit applies too: 4 %, not the schema's 6 %.
    TransitionPlan faster = plan;
    faster.incomingTempoRatio = 1.05;
    EXPECT_EQ(QStringLiteral("plan.incomingTempoRatio is out of range"),
            validateBeatmatchEdge(faster));
}

TEST_F(CrateTransitionPlanTest, FallsBackWithoutPolicyLimits) {
    QJsonObject json = object(QStringLiteral("synthetic-assist-long-blend.json"));
    json.remove(QStringLiteral("policyLimits"));
    const auto result = TransitionPlan::fromJson(json);
    ASSERT_TRUE(result.ok());
    TransitionPlan plan = result.value();
    EXPECT_FALSE(plan.policyLimits.has_value());

    // Today's ceiling: 48 s.
    EXPECT_EQ(QStringLiteral("plan.durationMs is longer than the policy allows"),
            validateBeatmatchEdge(plan));
    plan.durationMs = 48000;
    EXPECT_EQ(std::nullopt, validateBeatmatchEdge(plan));
    plan.durationMs = 48001;
    EXPECT_EQ(QStringLiteral("plan.durationMs is longer than the policy allows"),
            validateBeatmatchEdge(plan));
}

TEST_F(CrateTransitionPlanTest, ReadsPolicyLimitsStrictly) {
    const QString name = QStringLiteral("synthetic-assist-long-blend.json");
    const auto withLimits = [&](const QString& key, const QJsonValue& value) {
        QJsonObject plan = object(name);
        QJsonObject limits = plan.value(QStringLiteral("policyLimits")).toObject();
        if (value.isUndefined()) {
            limits.remove(key);
        } else {
            limits.insert(key, value);
        }
        plan.insert(QStringLiteral("policyLimits"), limits);
        const auto result = TransitionPlan::fromJson(plan);
        return result.ok() ? QString() : result.error().message;
    };
    EXPECT_EQ(QStringLiteral("plan.policyLimits.maximumLongBeatmatchMs is missing"),
            withLimits(QStringLiteral("maximumLongBeatmatchMs"), QJsonValue::Undefined));
    EXPECT_EQ(QStringLiteral("plan.policyLimits has no room for a transition"),
            withLimits(QStringLiteral("maximumLongBeatmatchMs"), 6000));
    EXPECT_EQ(QStringLiteral("plan.policyLimits.maximumTempoAdjustment is out of range"),
            withLimits(QStringLiteral("maximumTempoAdjustment"), 0.1));
    // The fields validate() does not read are not required.
    EXPECT_EQ(QString(), withLimits(QStringLiteral("phraseLengths"), QJsonValue::Undefined));
}

} // namespace
