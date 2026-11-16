#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

#include "crate/client/crateresult.h"

namespace crate {

/// The policy a TransitionPlan must keep to be played, beyond what any plan
/// from the server keeps: the server's `policyLimits`, the planner policy
/// resolved for one edge (`PolicyLimitsResponse`, STEM-D10). The defaults of
/// the first four fields are what the server applies when it sends no limits:
/// its `PlannerPolicy` for automatic mixing, which AutoDJ shares with Android
/// (D8, G0), and the schema's tempo bound; TransitionPlan::validate() checks
/// them on any plan. The rest only come from a server, and only
/// TransitionPlan::limitViolations() reads them.
struct TransitionPlanLimits {
    /// The longest transition that is not a long blend
    /// (`maximumTransitionMs`).
    qint64 maxTransitionMs = 12000;
    /// The longest beatmatched `dj-blend` (`maximumLongBeatmatchMs`): 48 s,
    /// and 96 s in Decks' assist mode (S5, decided by Diego on 2026-11-15),
    /// as the server sets it.
    qint64 maxLongBeatmatchMs = 48000;
    /// How far from 1 a beatmatch may take the incoming tempo
    /// (`maximumTempoAdjustment`).
    double maxTempoAdjustment = 0.06;
    /// Both tracks' tempo is steady enough for a long blend
    /// (`longBeatmatchEligible`).
    bool longBeatmatchEligible = true;

    /// What the context allowed (`beatmatchAllowed`, `tempoAdjustmentAllowed`,
    /// `bassHandoffAllowed`, `longBeatmatchAllowed`).
    bool beatmatchAllowed = true;
    bool tempoAdjustmentAllowed = true;
    bool bassHandoffAllowed = true;
    bool longBeatmatchAllowed = true;
    /// The shortest transition (`minimumTransitionMs`).
    qint64 minTransitionMs = 0;
    /// Each deck's gain: from -maxCutDb up to the lower of maxBoostDb and its
    /// peak-limited gain, or exactly that peak-limited gain
    /// (`maximumBoostDb`, `maximumCutDb`, `outgoingPeakLimitedGainDb`,
    /// `incomingPeakLimitedGainDb`).
    double maxBoostDb = 0.0;
    double maxCutDb = 0.0;
    double outgoingPeakLimitedGainDb = 0.0;
    double incomingPeakLimitedGainDb = 0.0;
    /// The windows, in each track's own time, of the profiles the server
    /// planned on: the outgoing cue from `outgoingEarliestCueMs`, the cue plus
    /// the duration up to `outgoingLatestEndMs`, and the incoming cue plus
    /// the duration at the incoming tempo up to `incomingLatestEndMs`.
    qint64 outgoingEarliestCueMs = 0;
    qint64 outgoingLatestEndMs = 0;
    qint64 incomingLatestEndMs = 0;
};

struct TransitionPlanCandidate;

/// One Smart Mix transition as the server planned it: one item of `plans` in
/// the answer of `POST /api/playback/transition-plans` (`TransitionPlanResponse`
/// on the server, design D8). Field names and meanings are the server's and
/// match the Android player's `NativeTransitionPlan`.
///
/// Times are milliseconds. `outgoingCueMs` and `incomingCueMs` are positions
/// in each track's own audio; `durationMs` and `beatPhaseOffsetMs` are output
/// time, which runs at the outgoing track's tempo.
struct TransitionPlan {
    enum class Mode {
        /// The incoming track starts as the outgoing one ends, without overlap.
        Gapless,
        /// A crossfade without tempo change.
        Adaptive,
        /// A crossfade with the incoming tempo matched and its beats aligned.
        Beatmatch,
    };
    enum class Curve {
        EqualPower,
        /// The long beatmatched blend.
        DjBlend,
    };
    enum class BassHandoff {
        None,
        /// The basses swap at `handoffProgress`.
        Balanced,
    };
    /// Why the planner did not beatmatch the edge. The plan is still complete
    /// and playable: the reason says what it fell back from.
    enum class FallbackReason {
        MissingProfile,
        LowConfidence,
        UnstableTempo,
        IncompatibleTempo,
        IncompatibleMeter,
        PreloadDeadline,
        OfflineUnavailable,
        InsufficientWindow,
    };

    /// The plan format this client reads. Every planner (`smart-mix-v2`,
    /// `smart-mix-v3`) serves it.
    static constexpr int kPlannerVersion = 2;
    /// The bounds of `incomingTempoRatio` in any plan (server schema; design
    /// D8, decided at G0).
    static constexpr double kMinTempoRatio = 0.94;
    static constexpr double kMaxTempoRatio = 1.06;

    using Limits = TransitionPlanLimits;

    /// Lowercase entity uids of the edge.
    QString outgoingTrackEntityUid;
    QString incomingTrackEntityUid;
    Mode mode = Mode::Adaptive;
    qint64 durationMs = 0;
    /// Where the outgoing track is when the transition starts.
    qint64 outgoingCueMs = 0;
    /// Where the incoming track starts, before the beat phase offset.
    qint64 incomingCueMs = 0;
    /// The incoming track's playback rate: 1 outside beatmatch.
    double incomingTempoRatio = 1.0;
    /// Beatmatch only, below one outgoing beat: the incoming track starts
    /// this much output time after the transition does (startIncomingCueMs()).
    qint64 beatPhaseOffsetMs = 0;
    /// Where in the transition, from 0 to 1, the basses swap.
    double handoffProgress = 0.5;
    double outgoingGainDb = 0.0;
    double incomingGainDb = 0.0;
    Curve curve = Curve::EqualPower;
    BassHandoff bassHandoff = BassHandoff::None;
    /// From 0 to 1.
    double confidence = 0.0;
    std::optional<FallbackReason> fallbackReason;
    /// `smart-mix-v3` only: the plan was placed on the analyzed phrases of
    /// both tracks. Missing from `smart-mix-v2` plans.
    std::optional<bool> structureUsed;
    /// `smart-mix-v3` only: the stems layer of both tracks scored the plan.
    bool stemsUsed = false;
    /// The server's limits for the edge (`policyLimits`). A server with S5
    /// sends them with every plan of the decks context when both tracks have
    /// a profile; older servers, and the dev stack's plans in the fixtures,
    /// send none. The candidates carry their edge's.
    std::optional<Limits> policyLimits;
    /// `smart-mix-v3` with `localRefinement` only: the server's candidate
    /// menu (SM3-D3, STEM-D10), best first, whose first entry is this plan.
    /// Empty otherwise, and in the candidates themselves.
    std::vector<TransitionPlanCandidate> candidates;
    /// Which entry of the server's menu this plan is: 0 for the plan the
    /// server picked, or the candidate playable() chose instead.
    int candidateIndex = 0;

    /// The planner's short cut, a crossfade this short when no window fits
    /// (`manual_ramp_ms` of the server's policy, which `policyLimits` does
    /// not carry).
    static constexpr qint64 kShortCutMaxMs = 350;

    /// Reads one plan strictly: every field the server always sends must be
    /// there with its type, enumerations must hold a known value, and numbers
    /// must keep the server schema's bounds (times not negative, tempo ratio
    /// from kMinTempoRatio to kMaxTempoRatio, progress and confidence from 0
    /// to 1, finite gains), `policyLimits`, if any, the fields Limits reads,
    /// and `candidates`, if any, whole candidates of the same edge. Another
    /// plan version, or a uid that is not one, is a Decode error. Fields this
    /// client does not know are ignored.
    static Result<TransitionPlan> fromJson(const QJsonObject& object);
    static Result<TransitionPlan> fromJson(const QByteArray& json);

    /// Whether the plan can be played on tracks of these durations (from
    /// their mix profiles, which the planner used), as the problem if not:
    ///
    /// - the tempo ratio is within its bounds and Limits::maxTempoAdjustment,
    ///   and exactly 1 outside beatmatch;
    /// - a gapless plan has no duration; any other has one, up to
    ///   Limits::maxTransitionMs, or Limits::maxLongBeatmatchMs for a
    ///   beatmatched `dj-blend` of tracks eligible for it;
    /// - a beat phase offset, a bass handoff and the `dj-blend` curve only
    ///   come with beatmatch, and the offset falls within the transition;
    /// - the transition fits in both tracks: the outgoing one from its cue,
    ///   the incoming one from its cue at its tempo ratio;
    /// - with the server's policyLimits, a plan that mixes() keeps them all
    ///   (limitViolations() is empty).
    ///
    /// The limits are the plan's own policyLimits, or the defaults of Limits
    /// when the server sent none.
    std::optional<QString> validate(qint64 outgoingDurationMs, qint64 incomingDurationMs) const;
    /// The same with other limits than the plan's, without limitViolations().
    std::optional<QString> validate(qint64 outgoingDurationMs,
            qint64 incomingDurationMs,
            const Limits& limits) const;

    /// The codes the server's reference check of a plan against the limits
    /// of its edge gives (`limit_violations` in
    /// app/crate/smart_mix/evaluation.py of the Crate monorepo, STEM-D10), in
    /// its order: `duration_out_of_range`, `long_beatmatch_ineligible`,
    /// `tempo_out_of_range`, `gain_out_of_range`, `true_peak_exceeded`,
    /// `outgoing_window_out_of_range`, `outgoing_body_too_short`,
    /// `incoming_body_too_short`, `beatmatch_not_allowed`,
    /// `tempo_adjustment_not_allowed`, `bass_handoff_not_allowed` and
    /// `long_beatmatch_not_allowed`. Empty for a plan within them. Only
    /// meaningful for a plan that mixes().
    QStringList limitViolations(const Limits& limits) const;
    /// Whether the server checks the plan against its limits: neither gapless
    /// nor the planner's short cut (an `adaptive` plan of
    /// `insufficient_window` up to kShortCutMaxMs), which its
    /// `policy_violations` exempts from the duration, peak and window rules.
    bool mixes() const;

    /// The plan to play on tracks of these durations, as the server would
    /// pick it: this one, the server's pick, if validate() passes; otherwise
    /// the first entry of its candidate menu, in the server's order, that
    /// passes, with candidateIndex saying which. None if no entry passes.
    /// `pProblem` gets validate()'s problem with this plan, or an empty
    /// string.
    std::optional<TransitionPlan> playable(qint64 outgoingDurationMs,
            qint64 incomingDurationMs,
            QString* pProblem = nullptr) const;

    /// Where the incoming track starts playing, in its own time. Beatmatch
    /// adds the phase offset at the incoming tempo; it is never subtracted.
    /// As `NativeTransitionPlan.startIncomingCueMs()`.
    qint64 startIncomingCueMs() const;

    bool isBeatmatch() const {
        return mode == Mode::Beatmatch;
    }
    bool hasBassHandoff() const {
        return bassHandoff == BassHandoff::Balanced;
    }
};

/// One entry of the server's candidate menu of an edge
/// (`TransitionCandidateResponse`, SM3-D3): a whole plan, its score and the
/// components of the score, each from 0 to 1.
struct TransitionPlanCandidate {
    /// The stems terms of a candidate (`StemScoreResponse`, STEM-D5).
    struct StemScore {
        qint64 vocalOverlapMs = 0;
        qint64 bassOverlapMs = 0;
        double drumsOnlyEntry = 0.0;
        double vocalFreeExit = 0.0;
    };
    /// `CandidateScoreResponse`.
    struct Breakdown {
        double alignment = 0.0;
        double energy = 0.0;
        double harmonic = 0.0;
        double stability = 0.0;
        double bass = 0.0;
        double position = 0.0;
        /// Only when both tracks carry the voice per phrase.
        std::optional<double> vocal;
        /// Only when both tracks have a stems layer.
        std::optional<StemScore> stems;
    };

    /// Its rank in the menu; 0 is the plan the server served.
    int index = 0;
    /// The phrases it mixes from and into, and its length in outgoing bars;
    /// none on the bar heuristic, and no length for the preferred duration.
    std::optional<int> outgoingPhrase;
    std::optional<int> incomingPhrase;
    std::optional<int> lengthBars;
    bool structureUsed = false;
    bool stemsUsed = false;
    double score = 0.0;
    Breakdown breakdown;
    /// With the edge's policyLimits, its own structureUsed and stemsUsed, its
    /// index as candidateIndex and no candidates.
    TransitionPlan plan;
};

/// The server's names, for logs and requests.
QString toString(TransitionPlan::Mode mode);
QString toString(TransitionPlan::FallbackReason reason);

} // namespace crate
