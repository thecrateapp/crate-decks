#include "crate/smartmix/transitionplan.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "crate/client/cratejson.h"

namespace crate {

namespace {

const QString kPath = QStringLiteral("plan");

/// Rounding the server's window may leave at the end of a track.
constexpr double kRoundingMs = 1.0;
/// How far from 1 a tempo ratio outside beatmatch may be: the server writes
/// exactly 1.0, rounded to six decimals.
constexpr double kUnitRatioTolerance = 1e-6;

constexpr std::array<std::pair<TransitionPlan::Mode, const char*>, 3> kModes{{
        {TransitionPlan::Mode::Gapless, "gapless"},
        {TransitionPlan::Mode::Adaptive, "adaptive"},
        {TransitionPlan::Mode::Beatmatch, "beatmatch"},
}};

constexpr std::array<std::pair<TransitionPlan::Curve, const char*>, 2> kCurves{{
        {TransitionPlan::Curve::EqualPower, "equal-power"},
        {TransitionPlan::Curve::DjBlend, "dj-blend"},
}};

constexpr std::array<std::pair<TransitionPlan::BassHandoff, const char*>, 2> kBassHandoffs{{
        {TransitionPlan::BassHandoff::None, "none"},
        {TransitionPlan::BassHandoff::Balanced, "balanced"},
}};

constexpr std::array<std::pair<TransitionPlan::FallbackReason, const char*>, 8> kFallbackReasons{{
        {TransitionPlan::FallbackReason::MissingProfile, "missing_profile"},
        {TransitionPlan::FallbackReason::LowConfidence, "low_confidence"},
        {TransitionPlan::FallbackReason::UnstableTempo, "unstable_tempo"},
        {TransitionPlan::FallbackReason::IncompatibleTempo, "incompatible_tempo"},
        {TransitionPlan::FallbackReason::IncompatibleMeter, "incompatible_meter"},
        {TransitionPlan::FallbackReason::PreloadDeadline, "preload_deadline"},
        {TransitionPlan::FallbackReason::OfflineUnavailable, "offline_unavailable"},
        {TransitionPlan::FallbackReason::InsufficientWindow, "insufficient_window"},
}};

template<typename Enum, std::size_t size>
std::optional<Enum> enumOf(const std::array<std::pair<Enum, const char*>, size>& names,
        const QString& text) {
    for (const auto& [value, name] : names) {
        if (text == QLatin1String(name)) {
            return value;
        }
    }
    return std::nullopt;
}

template<typename Enum, std::size_t size>
QString nameOf(const std::array<std::pair<Enum, const char*>, size>& names, Enum value) {
    for (const auto& [candidate, name] : names) {
        if (candidate == value) {
            return QString::fromLatin1(name);
        }
    }
    return QString();
}

/// Collects the first problem that is not about a field's presence or type.
class Problem {
  public:
    void check(bool holds, const QString& field, const char* problem) {
        if (!holds && !m_problem) {
            m_problem = kPath + QLatin1Char('.') + field + QLatin1Char(' ') +
                    QLatin1String(problem);
        }
    }
    const std::optional<QString>& problem() const {
        return m_problem;
    }

  private:
    std::optional<QString> m_problem;
};

/// The tolerances of the server's limit check (evaluation.py): `_EPSILON`,
/// `_TRUE_PEAK_TOLERANCE_DB` and `_TEMPO_ADJUSTMENT_FLOOR`.
constexpr double kLimitEpsilon = 1e-6;
constexpr double kTruePeakToleranceDb = 1e-3;
constexpr double kTempoAdjustmentFloor = 0.001;

/// A plan's `policyLimits`. `rulesRevision` and `phraseLengths` are not
/// read: no check uses them.
Result<TransitionPlanLimits> limitsFromJson(const QJsonObject& object) {
    const QString path = kPath + QStringLiteral(".policyLimits");
    json::ObjectReader reader(object, path);
    TransitionPlanLimits limits;
    limits.beatmatchAllowed = reader.requiredBool(u"beatmatchAllowed");
    limits.tempoAdjustmentAllowed = reader.requiredBool(u"tempoAdjustmentAllowed");
    limits.bassHandoffAllowed = reader.requiredBool(u"bassHandoffAllowed");
    limits.longBeatmatchAllowed = reader.requiredBool(u"longBeatmatchAllowed");
    limits.longBeatmatchEligible = reader.requiredBool(u"longBeatmatchEligible");
    limits.maxTempoAdjustment = reader.requiredDouble(u"maximumTempoAdjustment");
    limits.minTransitionMs = reader.requiredInteger(u"minimumTransitionMs");
    limits.maxTransitionMs = reader.requiredInteger(u"maximumTransitionMs");
    limits.maxLongBeatmatchMs = reader.requiredInteger(u"maximumLongBeatmatchMs");
    limits.maxBoostDb = reader.requiredDouble(u"maximumBoostDb");
    limits.maxCutDb = reader.requiredDouble(u"maximumCutDb");
    limits.outgoingPeakLimitedGainDb = reader.requiredDouble(u"outgoingPeakLimitedGainDb");
    limits.incomingPeakLimitedGainDb = reader.requiredDouble(u"incomingPeakLimitedGainDb");
    limits.outgoingEarliestCueMs = reader.requiredInteger(u"outgoingEarliestCueMs");
    limits.outgoingLatestEndMs = reader.requiredInteger(u"outgoingLatestEndMs");
    limits.incomingLatestEndMs = reader.requiredInteger(u"incomingLatestEndMs");
    if (!reader.ok()) {
        return reader.error();
    }
    if (limits.maxTransitionMs <= 0 || limits.maxLongBeatmatchMs < limits.maxTransitionMs) {
        return Error::decode(path + QStringLiteral(" has no room for a transition"));
    }
    if (!(limits.maxTempoAdjustment >= 0.0 &&
                limits.maxTempoAdjustment <= TransitionPlan::kMaxTempoRatio - 1.0)) {
        return Error::decode(path + QStringLiteral(".maximumTempoAdjustment is out of range"));
    }
    for (const double gain : {limits.maxBoostDb,
                 limits.maxCutDb,
                 limits.outgoingPeakLimitedGainDb,
                 limits.incomingPeakLimitedGainDb}) {
        if (!std::isfinite(gain)) {
            return Error::decode(path + QStringLiteral(" has a gain that is not finite"));
        }
    }
    return limits;
}

std::optional<int> optionalInt(json::ObjectReader& reader, QStringView key) {
    const std::optional<qint64> value = reader.optionalInteger(key);
    if (!value) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

QString uidOf(const QString& text) {
    const QUuid uid = QUuid::fromString(text);
    return uid.isNull() ? QString() : uid.toString(QUuid::WithoutBraces);
}

/// One candidate of the menu of `edge`, at `position`.
Result<TransitionPlanCandidate> candidateFromJson(
        const QJsonValue& value, int position, const TransitionPlan& edge) {
    const QString path = kPath + QStringLiteral(".candidates[%1]").arg(position);
    if (!value.isObject()) {
        return Error::decode(path + QStringLiteral(" is not an object"));
    }
    json::ObjectReader reader(value.toObject(), path);
    TransitionPlanCandidate candidate;
    candidate.index = static_cast<int>(reader.requiredInteger(u"index"));
    candidate.outgoingPhrase = optionalInt(reader, u"outgoingPhrase");
    candidate.incomingPhrase = optionalInt(reader, u"incomingPhrase");
    candidate.lengthBars = optionalInt(reader, u"lengthBars");
    candidate.structureUsed = reader.requiredBool(u"structureUsed");
    candidate.stemsUsed = reader.optionalBool(u"stemsUsed", false);
    candidate.score = reader.requiredDouble(u"score");
    const QJsonObject breakdown = reader.requiredObject(u"breakdown");
    const QJsonObject plan = reader.requiredObject(u"plan");
    if (!reader.ok()) {
        return reader.error();
    }
    json::ObjectReader scores(breakdown, path + QStringLiteral(".breakdown"));
    TransitionPlanCandidate::Breakdown& parts = candidate.breakdown;
    parts.alignment = scores.requiredDouble(u"alignment");
    parts.energy = scores.requiredDouble(u"energy");
    parts.harmonic = scores.requiredDouble(u"harmonic");
    parts.stability = scores.requiredDouble(u"stability");
    parts.bass = scores.requiredDouble(u"bass");
    parts.position = scores.requiredDouble(u"position");
    parts.vocal = scores.optionalDouble(u"vocal");
    const std::optional<QJsonObject> stems = scores.optionalObject(u"stems");
    if (!scores.ok()) {
        return scores.error();
    }
    if (stems) {
        json::ObjectReader stemReader(*stems, path + QStringLiteral(".breakdown.stems"));
        TransitionPlanCandidate::StemScore stemScore;
        stemScore.vocalOverlapMs = stemReader.requiredInteger(u"vocalOverlapMs");
        stemScore.bassOverlapMs = stemReader.requiredInteger(u"bassOverlapMs");
        stemScore.drumsOnlyEntry = stemReader.requiredDouble(u"drumsOnlyEntry");
        stemScore.vocalFreeExit = stemReader.requiredDouble(u"vocalFreeExit");
        if (!stemReader.ok()) {
            return stemReader.error();
        }
        parts.stems = stemScore;
    }
    const Result<TransitionPlan> parsed = TransitionPlan::fromJson(plan);
    if (!parsed.ok()) {
        return Error::decode(path + QStringLiteral(": ") + parsed.error().message);
    }
    if (parsed.value().outgoingTrackEntityUid != edge.outgoingTrackEntityUid ||
            parsed.value().incomingTrackEntityUid != edge.incomingTrackEntityUid) {
        return Error::decode(path + QStringLiteral(" is for another edge"));
    }
    if (candidate.index < 0) {
        return Error::decode(path + QStringLiteral(".index is negative"));
    }
    candidate.plan = parsed.value();
    candidate.plan.structureUsed = candidate.structureUsed;
    candidate.plan.stemsUsed = candidate.stemsUsed;
    candidate.plan.policyLimits = edge.policyLimits;
    candidate.plan.candidateIndex = candidate.index;
    return candidate;
}

} // namespace

Result<TransitionPlan> TransitionPlan::fromJson(const QJsonObject& object) {
    json::ObjectReader reader(object, kPath);
    const qint64 plannerVersion = reader.requiredInteger(u"plannerVersion");
    const QString outgoingUid = reader.requiredString(u"outgoingTrackEntityUid");
    const QString incomingUid = reader.requiredString(u"incomingTrackEntityUid");
    const QString mode = reader.requiredString(u"mode");
    TransitionPlan plan;
    plan.durationMs = reader.requiredInteger(u"durationMs");
    plan.outgoingCueMs = reader.requiredInteger(u"outgoingCueMs");
    plan.incomingCueMs = reader.requiredInteger(u"incomingCueMs");
    plan.incomingTempoRatio = reader.requiredDouble(u"incomingTempoRatio");
    plan.beatPhaseOffsetMs = reader.requiredInteger(u"beatPhaseOffsetMs");
    plan.handoffProgress = reader.requiredDouble(u"handoffProgress");
    plan.outgoingGainDb = reader.requiredDouble(u"outgoingGainDb");
    plan.incomingGainDb = reader.requiredDouble(u"incomingGainDb");
    const QString curve = reader.requiredString(u"curve");
    const QString bassHandoff = reader.requiredString(u"bassHandoff");
    plan.confidence = reader.requiredDouble(u"confidence");
    const QString fallbackReason = reader.optionalString(u"fallbackReason");
    const QJsonValue structureUsed = object.value(QStringLiteral("structureUsed"));
    if (!structureUsed.isUndefined() && !structureUsed.isNull()) {
        plan.structureUsed = reader.optionalBool(u"structureUsed", false);
    }
    plan.stemsUsed = reader.optionalBool(u"stemsUsed", false);
    const std::optional<QJsonObject> policyLimits = reader.optionalObject(u"policyLimits");
    const QJsonValue candidates = object.value(QStringLiteral("candidates"));
    if (!reader.ok()) {
        return reader.error();
    }
    if (!candidates.isUndefined() && !candidates.isNull() && !candidates.isArray()) {
        return Error::decode(kPath + QStringLiteral(".candidates is not an array"));
    }
    if (policyLimits) {
        const Result<Limits> limits = limitsFromJson(*policyLimits);
        if (!limits.ok()) {
            return limits.error();
        }
        plan.policyLimits = limits.value();
    }

    plan.outgoingTrackEntityUid = uidOf(outgoingUid);
    plan.incomingTrackEntityUid = uidOf(incomingUid);
    const auto parsedMode = enumOf(kModes, mode);
    const auto parsedCurve = enumOf(kCurves, curve);
    const auto parsedBassHandoff = enumOf(kBassHandoffs, bassHandoff);
    const auto parsedFallbackReason = enumOf(kFallbackReasons, fallbackReason);
    Problem problem;
    problem.check(plannerVersion == kPlannerVersion,
            QStringLiteral("plannerVersion"),
            "is not a supported plan version");
    problem.check(!plan.outgoingTrackEntityUid.isEmpty(),
            QStringLiteral("outgoingTrackEntityUid"),
            "is not a uid");
    problem.check(!plan.incomingTrackEntityUid.isEmpty(),
            QStringLiteral("incomingTrackEntityUid"),
            "is not a uid");
    problem.check(parsedMode.has_value(), QStringLiteral("mode"), "is not a known mode");
    problem.check(parsedCurve.has_value(), QStringLiteral("curve"), "is not a known curve");
    problem.check(parsedBassHandoff.has_value(),
            QStringLiteral("bassHandoff"),
            "is not a known bass handoff");
    problem.check(fallbackReason.isEmpty() || parsedFallbackReason.has_value(),
            QStringLiteral("fallbackReason"),
            "is not a known reason");
    for (const auto& [field, value] : {
                 std::pair{QStringLiteral("durationMs"), plan.durationMs},
                 std::pair{QStringLiteral("outgoingCueMs"), plan.outgoingCueMs},
                 std::pair{QStringLiteral("incomingCueMs"), plan.incomingCueMs},
                 std::pair{QStringLiteral("beatPhaseOffsetMs"), plan.beatPhaseOffsetMs},
         }) {
        problem.check(value >= 0, field, "is negative");
    }
    problem.check(plan.incomingTempoRatio >= kMinTempoRatio &&
                    plan.incomingTempoRatio <= kMaxTempoRatio,
            QStringLiteral("incomingTempoRatio"),
            "is out of range");
    problem.check(plan.handoffProgress >= 0.0 && plan.handoffProgress <= 1.0,
            QStringLiteral("handoffProgress"),
            "is out of range");
    problem.check(plan.confidence >= 0.0 && plan.confidence <= 1.0,
            QStringLiteral("confidence"),
            "is out of range");
    problem.check(std::isfinite(plan.outgoingGainDb),
            QStringLiteral("outgoingGainDb"),
            "is not finite");
    problem.check(std::isfinite(plan.incomingGainDb),
            QStringLiteral("incomingGainDb"),
            "is not finite");
    if (problem.problem()) {
        return Error::decode(*problem.problem());
    }
    plan.mode = *parsedMode;
    plan.curve = *parsedCurve;
    plan.bassHandoff = *parsedBassHandoff;
    plan.fallbackReason = parsedFallbackReason;
    const QJsonArray menu = candidates.toArray();
    plan.candidates.reserve(static_cast<std::size_t>(menu.size()));
    for (int position = 0; position < menu.size(); ++position) {
        const Result<TransitionPlanCandidate> candidate =
                candidateFromJson(menu.at(position), position, plan);
        if (!candidate.ok()) {
            return candidate.error();
        }
        plan.candidates.push_back(candidate.value());
    }
    return plan;
}

Result<TransitionPlan> TransitionPlan::fromJson(const QByteArray& json) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return Error::decode(QStringLiteral("The transition plan is not JSON: %1")
                        .arg(parseError.errorString()));
    }
    const Result<QJsonObject> object = json::requireObject(document);
    if (!object.ok()) {
        return object.error();
    }
    return fromJson(object.value());
}

std::optional<QString> TransitionPlan::validate(
        qint64 outgoingDurationMs, qint64 incomingDurationMs) const {
    if (auto problem = validate(
                outgoingDurationMs, incomingDurationMs, policyLimits.value_or(Limits()))) {
        return problem;
    }
    if (!policyLimits || !mixes()) {
        return std::nullopt;
    }
    const QStringList codes = limitViolations(*policyLimits);
    if (codes.isEmpty()) {
        return std::nullopt;
    }
    return QStringLiteral("plan breaks the server's limits: ") + codes.join(QStringLiteral(", "));
}

std::optional<QString> TransitionPlan::validate(qint64 outgoingDurationMs,
        qint64 incomingDurationMs,
        const Limits& limits) const {
    if (outgoingDurationMs <= 0 || incomingDurationMs <= 0) {
        return QStringLiteral("The durations of the tracks are unknown");
    }
    Problem problem;
    problem.check(incomingTempoRatio >= kMinTempoRatio && incomingTempoRatio <= kMaxTempoRatio &&
                    std::abs(incomingTempoRatio - 1.0) <=
                            limits.maxTempoAdjustment + kUnitRatioTolerance,
            QStringLiteral("incomingTempoRatio"),
            "is out of range");
    problem.check(isBeatmatch() || std::abs(incomingTempoRatio - 1.0) <= kUnitRatioTolerance,
            QStringLiteral("incomingTempoRatio"),
            "changes the tempo outside beatmatch");
    const bool longBlend = isBeatmatch() && curve == Curve::DjBlend;
    if (mode == Mode::Gapless) {
        problem.check(durationMs == 0, QStringLiteral("durationMs"), "overlaps a gapless plan");
    } else {
        problem.check(durationMs > 0, QStringLiteral("durationMs"), "is zero");
        problem.check(durationMs <=
                        (longBlend ? limits.maxLongBeatmatchMs : limits.maxTransitionMs),
                QStringLiteral("durationMs"),
                "is longer than the policy allows");
        problem.check(durationMs <= limits.maxTransitionMs || limits.longBeatmatchEligible,
                QStringLiteral("durationMs"),
                "is a long blend of tracks whose tempo is not steady enough");
    }
    problem.check(isBeatmatch() || beatPhaseOffsetMs == 0,
            QStringLiteral("beatPhaseOffsetMs"),
            "is set outside beatmatch");
    problem.check(beatPhaseOffsetMs <= durationMs,
            QStringLiteral("beatPhaseOffsetMs"),
            "falls after the transition");
    problem.check(isBeatmatch() || !hasBassHandoff(),
            QStringLiteral("bassHandoff"),
            "is set outside beatmatch");
    problem.check(isBeatmatch() || curve != Curve::DjBlend,
            QStringLiteral("curve"),
            "is dj-blend outside beatmatch");
    problem.check(outgoingCueMs + durationMs <= outgoingDurationMs,
            QStringLiteral("outgoingCueMs"),
            "leaves the transition past the end of the outgoing track");
    problem.check(static_cast<double>(incomingCueMs) +
                            static_cast<double>(durationMs) * incomingTempoRatio <=
                    static_cast<double>(incomingDurationMs) + kRoundingMs,
            QStringLiteral("incomingCueMs"),
            "leaves the transition past the end of the incoming track");
    return problem.problem();
}

QStringList TransitionPlan::limitViolations(const Limits& limits) const {
    // As `limit_violations` in the server's evaluation.py, check by check.
    QStringList codes;
    const bool beatmatch = isBeatmatch();
    const double ratio = incomingTempoRatio;
    const bool longBlend = durationMs > limits.maxTransitionMs;
    if (durationMs < limits.minTransitionMs || durationMs > limits.maxLongBeatmatchMs) {
        codes.append(QStringLiteral("duration_out_of_range"));
    } else if (longBlend &&
            !(beatmatch && curve == Curve::DjBlend && limits.longBeatmatchEligible)) {
        codes.append(QStringLiteral("long_beatmatch_ineligible"));
    }
    const double maxAdjustment = beatmatch ? limits.maxTempoAdjustment : 0.0;
    if (std::abs(ratio - 1.0) > maxAdjustment + kLimitEpsilon) {
        codes.append(QStringLiteral("tempo_out_of_range"));
    }
    const std::array<std::pair<double, double>, 2> gains{{
            {outgoingGainDb, limits.outgoingPeakLimitedGainDb},
            {incomingGainDb, limits.incomingPeakLimitedGainDb},
    }};
    const bool gainsInRange = std::all_of(
            gains.cbegin(), gains.cend(), [&limits](const auto& gain) {
                return gain.first <= limits.maxBoostDb + kLimitEpsilon &&
                        (gain.first >= -limits.maxCutDb - kLimitEpsilon ||
                                std::abs(gain.first - gain.second) <=
                                        kTruePeakToleranceDb);
            });
    if (!gainsInRange) {
        codes.append(QStringLiteral("gain_out_of_range"));
    }
    if (std::any_of(gains.cbegin(), gains.cend(), [](const auto& gain) {
            return gain.first > gain.second + kTruePeakToleranceDb;
        })) {
        codes.append(QStringLiteral("true_peak_exceeded"));
    }
    if (outgoingCueMs + durationMs > limits.outgoingLatestEndMs) {
        codes.append(QStringLiteral("outgoing_window_out_of_range"));
    }
    if (outgoingCueMs < limits.outgoingEarliestCueMs) {
        codes.append(QStringLiteral("outgoing_body_too_short"));
    }
    const double incomingRate = beatmatch ? ratio : 1.0;
    if (static_cast<double>(incomingCueMs) + static_cast<double>(durationMs) * incomingRate >
            static_cast<double>(limits.incomingLatestEndMs)) {
        codes.append(QStringLiteral("incoming_body_too_short"));
    }
    if (beatmatch && !limits.beatmatchAllowed) {
        codes.append(QStringLiteral("beatmatch_not_allowed"));
    }
    if (std::abs(ratio - 1.0) > kTempoAdjustmentFloor && !limits.tempoAdjustmentAllowed) {
        codes.append(QStringLiteral("tempo_adjustment_not_allowed"));
    }
    if (hasBassHandoff() && !limits.bassHandoffAllowed) {
        codes.append(QStringLiteral("bass_handoff_not_allowed"));
    }
    if (longBlend && !limits.longBeatmatchAllowed) {
        codes.append(QStringLiteral("long_beatmatch_not_allowed"));
    }
    return codes;
}

bool TransitionPlan::mixes() const {
    const bool shortCut = mode == Mode::Adaptive &&
            fallbackReason == FallbackReason::InsufficientWindow &&
            durationMs <= kShortCutMaxMs;
    return mode != Mode::Gapless && !shortCut;
}

std::optional<TransitionPlan> TransitionPlan::playable(
        qint64 outgoingDurationMs, qint64 incomingDurationMs, QString* pProblem) const {
    const std::optional<QString> problem = validate(outgoingDurationMs, incomingDurationMs);
    if (pProblem) {
        *pProblem = problem.value_or(QString());
    }
    if (!problem) {
        return *this;
    }
    // The server's next best, in its order; the first entry is this plan.
    for (const TransitionPlanCandidate& candidate : candidates) {
        if (candidate.index == candidateIndex) {
            continue;
        }
        if (!candidate.plan.validate(outgoingDurationMs, incomingDurationMs)) {
            return candidate.plan;
        }
    }
    return std::nullopt;
}

qint64 TransitionPlan::startIncomingCueMs() const {
    if (!isBeatmatch()) {
        return incomingCueMs;
    }
    return incomingCueMs +
            std::llround(static_cast<double>(beatPhaseOffsetMs) * incomingTempoRatio);
}

QString toString(TransitionPlan::Mode mode) {
    return nameOf(kModes, mode);
}

QString toString(TransitionPlan::FallbackReason reason) {
    return nameOf(kFallbackReasons, reason);
}

} // namespace crate
