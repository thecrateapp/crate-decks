#include "crate/smartmix/transitionplan.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
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

QString uidOf(const QString& text) {
    const QUuid uid = QUuid::fromString(text);
    return uid.isNull() ? QString() : uid.toString(QUuid::WithoutBraces);
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
    if (!reader.ok()) {
        return reader.error();
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

std::optional<QString> TransitionPlan::validate(qint64 outgoingDurationMs,
        qint64 incomingDurationMs,
        const Limits& limits) const {
    if (outgoingDurationMs <= 0 || incomingDurationMs <= 0) {
        return QStringLiteral("The durations of the tracks are unknown");
    }
    Problem problem;
    problem.check(incomingTempoRatio >= kMinTempoRatio && incomingTempoRatio <= kMaxTempoRatio,
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
