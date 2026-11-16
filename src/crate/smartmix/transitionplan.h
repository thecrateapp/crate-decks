#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <optional>

#include "crate/client/crateresult.h"

namespace crate {

/// The policy a TransitionPlan must keep to be played, beyond what any plan
/// from the server keeps: the parts of the server's `policyLimits` (the
/// planner policy resolved for one edge, `PolicyLimitsResponse`) that
/// TransitionPlan::validate() checks. The defaults are what the server
/// applies when it sends no limits: its `PlannerPolicy` for automatic mixing,
/// which AutoDJ shares with Android (D8, G0), and the schema's tempo bound.
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
};

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
    /// send none.
    std::optional<Limits> policyLimits;

    /// Reads one plan strictly: every field the server always sends must be
    /// there with its type, enumerations must hold a known value, and numbers
    /// must keep the server schema's bounds (times not negative, tempo ratio
    /// from kMinTempoRatio to kMaxTempoRatio, progress and confidence from 0
    /// to 1, finite gains), and `policyLimits`, if any, the fields Limits
    /// reads. Another plan version, or a uid that is not one, is a Decode
    /// error. Fields this client does not know are ignored.
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
    ///   the incoming one from its cue at its tempo ratio.
    ///
    /// The limits are the plan's own policyLimits, or the defaults of Limits
    /// when the server sent none.
    std::optional<QString> validate(qint64 outgoingDurationMs, qint64 incomingDurationMs) const;
    /// The same with other limits than the plan's.
    std::optional<QString> validate(qint64 outgoingDurationMs,
            qint64 incomingDurationMs,
            const Limits& limits) const;

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

/// The server's names, for logs and requests.
QString toString(TransitionPlan::Mode mode);
QString toString(TransitionPlan::FallbackReason reason);

} // namespace crate
