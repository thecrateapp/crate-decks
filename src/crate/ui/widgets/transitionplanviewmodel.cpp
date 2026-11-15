#include "crate/ui/widgets/transitionplanviewmodel.h"

#include <algorithm>
#include <cmath>

#include "crate/smartmix/curves.h"
#include "crate/smartmix/smartmixdirector.h"
#include "moc_transitionplanviewmodel.cpp"
#include "track/cue.h"
#include "track/track.h"

namespace crate::widgets {

namespace {

// The ` · ` between the parts of a MonoLabel (MonoLabel.tsx)
const QString kSeparator = QStringLiteral(" · ");
const QString kMinus = QStringLiteral("−");
// The range around the window: half its length before it and a quarter
// after, at least these many bars (the mockup: 16 + 32 + 8 bars)
constexpr double kLeadShare = 0.5;
constexpr double kTailShare = 0.25;
constexpr double kMinLeadBars = 8.0;
constexpr double kMinTailBars = 4.0;
// Without a grid, a bar of 120 BPM in 4/4 sizes the range
constexpr double kFallbackBarMs = 2000.0;
// A track cue this close to a plan moment joins it: half a beat, or this
// many milliseconds without a grid
constexpr double kJoinBeats = 0.5;
constexpr double kJoinMs = 250.0;
// A position this close below a beat counts as on it
constexpr double kOnBeat = 1e-4;
// The most beats a ruler draws
constexpr int kMaxTicks = 4096;
constexpr int kBisections = 48;
// A gain this close to 0 dB is no gain
constexpr double kNoGainDb = 0.05;

double framesPerMs(const mixxx::BeatsPointer& pBeats) {
    return pBeats->getSampleRate().toDouble() / 1000.0;
}

mixxx::audio::FramePos frameAt(const mixxx::BeatsPointer& pBeats, double ms) {
    return mixxx::audio::FramePos(ms * framesPerMs(pBeats));
}

double msAt(const mixxx::BeatsPointer& pBeats, mixxx::audio::FramePos position) {
    return position.value() / framesPerMs(pBeats);
}

bool hasGrid(const mixxx::BeatsPointer& pBeats) {
    return pBeats && pBeats->getSampleRate().isValid();
}

/// Where beat `index` from the first marker is, in ms
double msOfBeat(const mixxx::BeatsPointer& pBeats, int index) {
    auto beat = pBeats->cfirstmarker();
    beat += index;
    return msAt(pBeats, *beat);
}

int floorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

int barOfBeat(double index, int beatsPerBar) {
    return static_cast<int>(std::floor((index + kOnBeat) / beatsPerBar)) + 1;
}

int beatsPerBarOf(const PlanDeck& deck) {
    return std::max(1, deck.beatsPerBar);
}

/// The first `x` from `low` to `high` where `isPast` turns true, for an
/// `isPast` that is false and then true
template<typename Predicate>
double firstPast(double low, double high, Predicate isPast) {
    for (int step = 0; step < kBisections; ++step) {
        const double middle = 0.5 * (low + high);
        (isPast(middle) ? high : low) = middle;
    }
    return 0.5 * (low + high);
}

} // namespace

std::optional<double> beatIndexAt(const mixxx::BeatsPointer& pBeats, double trackMs) {
    if (!hasGrid(pBeats) || !std::isfinite(trackMs)) {
        return std::nullopt;
    }
    const mixxx::audio::FramePos position = frameAt(pBeats, trackMs);
    const auto first = pBeats->cfirstmarker();
    auto next = pBeats->iteratorFrom(position);
    const int nextIndex = next - first;
    auto previous = next;
    --previous;
    const double length = *next - *previous;
    if (*next == position || length <= 0) {
        return nextIndex;
    }
    return (nextIndex - 1) + (position - *previous) / length;
}

PlanDeck PlanDeck::fromTrack(const QString& group,
        const TrackPointer& pTrack,
        double durationMs,
        double rate) {
    PlanDeck deck;
    deck.group = group;
    deck.durationMs = durationMs;
    deck.rate = rate;
    if (!pTrack) {
        return deck;
    }
    deck.title = pTrack->getTitle();
    if (deck.title.isEmpty()) {
        deck.title = pTrack->getFileInfo().fileName();
    }
    deck.pBeats = pTrack->getBeats();
    deck.bpm = pTrack->getBpm();
    mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    if (!sampleRate.isValid() && hasGrid(deck.pBeats)) {
        sampleRate = deck.pBeats->getSampleRate();
    }
    if (!sampleRate.isValid()) {
        return deck;
    }
    const double perMs = sampleRate.toDouble() / 1000.0;
    const auto msOf = [perMs](mixxx::audio::FramePos position) -> std::optional<double> {
        if (!position.isValid()) {
            return std::nullopt;
        }
        return position.value() / perMs;
    };
    const QList<CuePointer> cues = pTrack->getCuePoints();
    for (const CuePointer& pCue : cues) {
        PlanTrackCue cue;
        switch (pCue->getType()) {
        case mixxx::CueType::HotCue:
            if (pCue->getHotCue() < 0) {
                continue;
            }
            cue.type = PlanTrackCue::Type::HotCue;
            cue.number = pCue->getHotCue() + 1;
            break;
        case mixxx::CueType::Intro:
            cue.type = PlanTrackCue::Type::Intro;
            break;
        case mixxx::CueType::Outro:
            cue.type = PlanTrackCue::Type::Outro;
            break;
        default:
            continue;
        }
        cue.startMs = msOf(pCue->getPosition());
        cue.endMs = msOf(pCue->getEndPosition());
        if (cue.startMs || cue.endMs) {
            deck.cues.append(cue);
        }
    }
    return deck;
}

bool TransitionPlanViewModel::Tempo::changes() const {
    return std::abs(fromBpm - toBpm) >= 0.005;
}

TransitionPlanViewModel::TransitionPlanViewModel(QObject* pParent)
        : QObject(pParent) {
}

void TransitionPlanViewModel::setPlan(const TransitionPlan& plan,
        const PlanDeck& outgoing,
        const PlanDeck& incoming) {
    m_plan = plan;
    m_outgoing = outgoing;
    m_incoming = incoming;
    layOut();
    emit changed();
}

void TransitionPlanViewModel::clear() {
    if (!m_plan) {
        return;
    }
    m_plan.reset();
    m_outgoing = PlanDeck();
    m_incoming = PlanDeck();
    m_outgoingTicks.clear();
    m_incomingTicks.clear();
    m_moments.clear();
    m_bassSwap.reset();
    m_tempo.reset();
    m_levels.reset();
    m_windowBars.reset();
    emit changed();
}

void TransitionPlanViewModel::setOutgoingPositionMs(double positionMs) {
    if (positionMs == m_nowMs) {
        return;
    }
    m_nowMs = positionMs;
    emit nowChanged();
}

QString TransitionPlanViewModel::deckLetter(Side side) const {
    const QString& group = deck(side).group;
    const QString number = group.mid(8, group.size() - 9);
    bool ok = false;
    const int index = number.toInt(&ok);
    return ok && index > 0 ? QString(QChar(static_cast<char16_t>(u'A' + index - 1))) : group;
}

QString TransitionPlanViewModel::roleLabel(Side side) const {
    return side == Side::Outgoing ? tr("Outgoing") : tr("Incoming");
}

double TransitionPlanViewModel::outgoingRate() const {
    return m_outgoing.rate > 0.0 ? m_outgoing.rate : 1.0;
}

double TransitionPlanViewModel::incomingRunRate() const {
    // As the director: the plan's ratio times the outgoing deck's rate in a
    // beatmatch, and the incoming deck's own otherwise
    if (m_plan && m_plan->isBeatmatch()) {
        return m_plan->incomingTempoRatio * outgoingRate();
    }
    return m_incoming.rate > 0.0 ? m_incoming.rate : 1.0;
}

double TransitionPlanViewModel::beatMs(Side side, double trackMs) const {
    const PlanDeck& planDeck = deck(side);
    if (hasGrid(planDeck.pBeats)) {
        const auto index = beatIndexAt(planDeck.pBeats, trackMs);
        if (index) {
            const int whole = static_cast<int>(std::floor(*index));
            return msOfBeat(planDeck.pBeats, whole + 1) - msOfBeat(planDeck.pBeats, whole);
        }
    }
    return planDeck.bpm > 0.0 ? 60000.0 / planDeck.bpm : 0.0;
}

double TransitionPlanViewModel::incomingPositionAt(double atMs) const {
    if (!m_plan) {
        return 0.0;
    }
    const double perAxisMs = incomingRunRate() / outgoingRate();
    if (atMs <= m_windowEndMs || !m_plan->isBeatmatch()) {
        return m_entryMs + (atMs - m_windowStartMs) * perAxisMs;
    }
    // After a beatmatch the director eases the incoming deck back to its own
    // rate along curves::tempoRestore(), a straight line in real time: its
    // position is that line's integral
    const double atExit = m_entryMs + (m_windowEndMs - m_windowStartMs) * perAxisMs;
    const double fromRate = incomingRunRate();
    const double toRate = m_incoming.rate > 0.0 ? m_incoming.rate : 1.0;
    const double restoreMs = curves::kTempoRestoreMs;
    const double realMs = (atMs - m_windowEndMs) / outgoingRate();
    const double within = std::min(realMs, restoreMs);
    double advance = fromRate * within + (toRate - fromRate) * within * within / (2.0 * restoreMs);
    if (realMs > restoreMs) {
        advance += toRate * (realMs - restoreMs);
    }
    return atExit + advance;
}

double TransitionPlanViewModel::atMsOfIncoming(double incomingMs) const {
    if (!m_plan) {
        return 0.0;
    }
    const double perAxisMs = incomingRunRate() / outgoingRate();
    if (!m_plan->isBeatmatch() || incomingMs <= incomingPositionAt(m_windowEndMs)) {
        return m_windowStartMs + (incomingMs - m_entryMs) / perAxisMs;
    }
    double high = m_windowEndMs + 1000.0;
    for (int step = 0; step < 64 && incomingPositionAt(high) < incomingMs; ++step) {
        high = m_windowEndMs + 2.0 * (high - m_windowEndMs);
    }
    return firstPast(m_windowEndMs, high, [this, incomingMs](double atMs) {
        return incomingPositionAt(atMs) >= incomingMs;
    });
}

std::optional<int> TransitionPlanViewModel::barAt(Side side, double atMs) const {
    const PlanDeck& planDeck = deck(side);
    const double trackMs = side == Side::Outgoing ? atMs : incomingPositionAt(atMs);
    const auto index = beatIndexAt(planDeck.pBeats, trackMs);
    if (!index) {
        return std::nullopt;
    }
    return barOfBeat(*index, beatsPerBarOf(planDeck));
}

double TransitionPlanViewModel::trackStartMs(Side side) const {
    return side == Side::Outgoing ? 0.0 : m_windowStartMs;
}

double TransitionPlanViewModel::trackEndMs(Side side) const {
    return side == Side::Outgoing ? m_outgoing.durationMs : atMsOfIncoming(m_incoming.durationMs);
}

void TransitionPlanViewModel::layOut() {
    const TransitionPlan& plan = *m_plan;
    m_windowStartMs = SmartMixDirector::startAtMs(plan, m_outgoing.durationMs);
    m_windowEndMs = m_windowStartMs + static_cast<double>(plan.durationMs);
    m_entryMs = static_cast<double>(plan.startIncomingCueMs());

    const mixxx::BeatsPointer& pBeats = m_outgoing.pBeats;
    const int beatsPerBar = beatsPerBarOf(m_outgoing);
    const auto startBeat = beatIndexAt(pBeats, m_windowStartMs);
    const auto endBeat = beatIndexAt(pBeats, m_windowEndMs);
    m_windowBars.reset();
    if (startBeat && endBeat) {
        m_windowBars = static_cast<int>(std::lround((*endBeat - *startBeat) / beatsPerBar));
    }

    // The range: the window and what leads to it and follows, on bar lines
    double barMs = beatMs(Side::Outgoing, m_windowStartMs) * beatsPerBar;
    if (barMs <= 0.0) {
        barMs = kFallbackBarMs;
    }
    const double windowMs = m_windowEndMs - m_windowStartMs;
    double rangeStart = m_windowStartMs - std::max(windowMs * kLeadShare, kMinLeadBars * barMs);
    double rangeEnd = m_windowEndMs + std::max(windowMs * kTailShare, kMinTailBars * barMs);
    if (const auto first = beatIndexAt(pBeats, rangeStart)) {
        rangeStart = msOfBeat(pBeats,
                static_cast<int>(std::floor((*first + kOnBeat) / beatsPerBar)) *
                        beatsPerBar);
    }
    if (const auto last = beatIndexAt(pBeats, rangeEnd)) {
        rangeEnd = msOfBeat(
                pBeats, static_cast<int>(std::ceil((*last - kOnBeat) / beatsPerBar)) * beatsPerBar);
    }
    m_rangeStartMs = std::max(0.0, rangeStart);
    m_rangeEndMs = std::max(m_rangeStartMs + barMs, rangeEnd);

    placeTicks(Side::Outgoing);
    placeTicks(Side::Incoming);
    placeMoments();
    placeCurves();
}

void TransitionPlanViewModel::placeTicks(Side side) {
    QVector<Tick>& ticks = side == Side::Outgoing ? m_outgoingTicks : m_incomingTicks;
    ticks.clear();
    const PlanDeck& planDeck = deck(side);
    const mixxx::BeatsPointer& pBeats = planDeck.pBeats;
    if (!hasGrid(pBeats)) {
        return;
    }
    // The part of the track the range shows, in its own time
    double fromMs = std::max(m_rangeStartMs, 0.0);
    double toMs = std::min(m_rangeEndMs, m_outgoing.durationMs);
    if (side == Side::Incoming) {
        fromMs = m_entryMs;
        toMs = std::min(incomingPositionAt(m_rangeEndMs), m_incoming.durationMs);
    }
    if (toMs < fromMs) {
        return;
    }
    const int beatsPerBar = beatsPerBarOf(planDeck);
    const int beatsPerPhrase = beatsPerBar * kPhraseBars;
    const auto first = pBeats->cfirstmarker();
    auto beat = pBeats->iteratorFrom(frameAt(pBeats, fromMs));
    for (int count = 0; count < kMaxTicks; ++count, ++beat) {
        const double trackMs = msAt(pBeats, *beat);
        if (trackMs > toMs) {
            break;
        }
        const int index = beat - first;
        Tick tick;
        tick.atMs = side == Side::Outgoing ? trackMs : atMsOfIncoming(trackMs);
        tick.bar = floorDiv(index, beatsPerBar) + 1;
        if (index - floorDiv(index, beatsPerPhrase) * beatsPerPhrase == 0) {
            tick.kind = TickKind::Phrase;
        } else if (index - floorDiv(index, beatsPerBar) * beatsPerBar == 0) {
            tick.kind = TickKind::Bar;
        }
        ticks.append(tick);
    }
}

void TransitionPlanViewModel::placeMoments() {
    const TransitionPlan& plan = *m_plan;
    m_moments.clear();
    const double perAxisMs = incomingRunRate() / outgoingRate();
    const auto tolerance = [this](Side side, double trackMs) {
        const double beat = beatMs(side, trackMs);
        return beat > 0.0 ? beat * kJoinBeats : kJoinMs;
    };

    // The plan's moments. The incoming deck enters on its cue; the plan
    // cued it on the track's Intro when the cue is on one of its ends
    Moment entry;
    entry.side = Side::Incoming;
    entry.kind = MomentKind::Entry;
    entry.atMs = m_windowStartMs;
    entry.label = tr("Entry");
    const double cueMs = static_cast<double>(plan.incomingCueMs);
    std::optional<double> introEdgeOnCue;
    double closest = tolerance(Side::Incoming, cueMs);
    for (const PlanTrackCue& cue : std::as_const(m_incoming.cues)) {
        if (cue.type != PlanTrackCue::Type::Intro) {
            continue;
        }
        for (const auto& edge : {cue.startMs, cue.endMs}) {
            if (edge && std::abs(*edge - cueMs) <= closest) {
                closest = std::abs(*edge - cueMs);
                introEdgeOnCue = *edge;
            }
        }
    }
    if (introEdgeOnCue) {
        entry.label += kSeparator + tr("intro cue");
    }
    Moment exit;
    exit.side = Side::Outgoing;
    exit.kind = MomentKind::Plan;
    exit.atMs = m_windowEndMs;
    exit.label = tr("Exit");
    QVector<Moment> planMoments{entry, exit};

    // The tracks' cues in range; one on a plan moment joins it
    QVector<Moment> trackMoments;
    const auto addCue = [&](Side side, double trackMs, const QString& label) {
        const PlanDeck& planDeck = deck(side);
        if (trackMs > planDeck.durationMs) {
            return;
        }
        double atMs = trackMs;
        double joinMs = tolerance(side, trackMs);
        if (side == Side::Incoming) {
            // The incoming track plays from the entry on
            if (trackMs < m_entryMs - joinMs) {
                return;
            }
            atMs = atMsOfIncoming(trackMs);
            joinMs /= perAxisMs;
        }
        if (atMs < m_rangeStartMs || atMs > m_rangeEndMs) {
            return;
        }
        for (Moment& moment : planMoments) {
            if (moment.side == side && std::abs(moment.atMs - atMs) <= joinMs) {
                moment.label += kSeparator + label.toLower();
                return;
            }
        }
        Moment moment;
        moment.side = side;
        moment.kind = MomentKind::Track;
        moment.atMs = atMs;
        moment.label = label;
        trackMoments.append(moment);
    };
    for (const Side side : {Side::Outgoing, Side::Incoming}) {
        for (const PlanTrackCue& cue : std::as_const(deck(side).cues)) {
            switch (cue.type) {
            case PlanTrackCue::Type::Intro:
                // Unless the entry already names it
                if (cue.endMs && !(side == Side::Incoming && introEdgeOnCue == cue.endMs)) {
                    addCue(side, *cue.endMs, tr("Intro end"));
                }
                break;
            case PlanTrackCue::Type::Outro:
                if (cue.startMs) {
                    addCue(side, *cue.startMs, tr("Outro in"));
                }
                break;
            case PlanTrackCue::Type::HotCue:
                if (cue.startMs) {
                    addCue(side, *cue.startMs, tr("Hotcue %1").arg(cue.number));
                }
                break;
            }
        }
    }
    m_moments = planMoments + trackMoments;
    for (Moment& moment : m_moments) {
        moment.bar = barAt(moment.side, moment.atMs);
    }
    std::stable_sort(m_moments.begin(), m_moments.end(), [](const Moment& a, const Moment& b) {
        return a.side != b.side ? a.side == Side::Outgoing : a.atMs < b.atMs;
    });
}

void TransitionPlanViewModel::placeCurves() {
    const TransitionPlan& plan = *m_plan;
    m_bassSwap.reset();
    m_tempo.reset();
    m_levels.reset();
    const double windowMs = m_windowEndMs - m_windowStartMs;
    if (windowMs <= 0.0) {
        // Gapless: the tracks follow each other, nothing blends
        return;
    }
    const auto atProgress = [this, windowMs](double progress) {
        return m_windowStartMs + progress * windowMs;
    };
    const bool djBlend = plan.curve == TransitionPlan::Curve::DjBlend;

    // What the director plays: each deck's level along the plan's curve
    Levels levels;
    for (int point = 0; point <= kCurvePoints; ++point) {
        const double progress = static_cast<double>(point) / kCurvePoints;
        levels.outgoing.append(Point{atProgress(progress),
                curves::transitionGain(
                        curves::Role::Outgoing, progress, plan.outgoingGainDb, djBlend)});
        levels.incoming.append(Point{atProgress(progress),
                curves::transitionGain(
                        curves::Role::Incoming, progress, plan.incomingGainDb, djBlend)});
    }
    m_levels = levels;

    // The bass swap of curves::bassCutDb(): where the basses cross, between
    // the incoming deck's lows leaving the cut and the outgoing deck's
    // reaching it
    if (plan.hasBassHandoff()) {
        const double handoff = plan.handoffProgress;
        const auto incomingCut = [handoff](double progress) {
            return curves::bassCutDb(curves::Role::Incoming, progress, handoff);
        };
        const auto outgoingCut = [handoff](double progress) {
            return curves::bassCutDb(curves::Role::Outgoing, progress, handoff);
        };
        BassSwap swap;
        swap.atMs = atProgress(firstPast(0.0, 1.0, [&](double progress) {
            return incomingCut(progress) >= outgoingCut(progress);
        }));
        swap.startMs = atProgress(firstPast(0.0, 1.0, [&](double progress) {
            return incomingCut(progress) > curves::kBassCutDb;
        }));
        swap.endMs = atProgress(firstPast(0.0, 1.0, [&](double progress) {
            return outgoingCut(progress) <= curves::kBassCutDb;
        }));
        swap.bar = barAt(Side::Outgoing, swap.atMs);
        m_bassSwap = swap;
    }

    // The incoming deck's tempo: the director's rate during the transition,
    // then, after a beatmatch, its tempo restore
    double bpm = m_incoming.bpm;
    if (hasGrid(m_incoming.pBeats)) {
        const mixxx::Bpm around =
                m_incoming.pBeats->getBpmAroundPosition(frameAt(m_incoming.pBeats, m_entryMs), 4);
        if (around.isValid()) {
            bpm = around.value();
        }
    }
    if (bpm > 0.0) {
        Tempo tempo;
        const double fromRate = incomingRunRate();
        const double toRate = plan.isBeatmatch() ? (m_incoming.rate > 0.0 ? m_incoming.rate : 1.0)
                                                 : fromRate;
        tempo.fromBpm = bpm * fromRate;
        tempo.toBpm = bpm * toRate;
        tempo.points.append(Point{m_windowStartMs, tempo.fromBpm});
        tempo.points.append(Point{m_windowEndMs, tempo.fromBpm});
        if (plan.isBeatmatch() && tempo.changes()) {
            for (int point = 1; point <= kCurvePoints; ++point) {
                const double realMs = curves::kTempoRestoreMs * point / kCurvePoints;
                tempo.points.append(Point{m_windowEndMs + realMs * outgoingRate(),
                        bpm * curves::tempoRestore(fromRate, toRate, realMs)});
            }
        }
        m_tempo = tempo;
    }
}

QString TransitionPlanViewModel::windowLabel() const {
    if (!m_plan) {
        return {};
    }
    const double windowMs = m_windowEndMs - m_windowStartMs;
    if (windowMs <= 0.0) {
        return tr("Gapless");
    }
    QString length;
    if (m_windowBars) {
        length = *m_windowBars == 1 ? tr("1 bar") : tr("%1 bars").arg(*m_windowBars);
    } else {
        length = tr("%1 s").arg(QString::number(windowMs / 1000.0, 'f', 1));
    }
    return tr("Transition") + kSeparator + length;
}

QString TransitionPlanViewModel::bassSwapLabel() const {
    if (!m_bassSwap) {
        return {};
    }
    QString label = tr("Bass swap");
    if (m_bassSwap->bar) {
        label += kSeparator + tr("bar %1").arg(*m_bassSwap->bar);
    }
    return label;
}

QString TransitionPlanViewModel::gainLabel(Side side) const {
    if (!m_plan || !m_levels) {
        return {};
    }
    const double gainDb = std::min(
            0.0, side == Side::Outgoing ? m_plan->outgoingGainDb : m_plan->incomingGainDb);
    if (!std::isfinite(gainDb) || gainDb > -kNoGainDb) {
        return {};
    }
    return QStringLiteral("%1 %2%3 dB")
            .arg(deckLetter(side), kMinus, QString::number(-gainDb, 'f', 1));
}

std::optional<std::pair<double, double>> TransitionPlanViewModel::beatmatch() const {
    if (!m_plan || !m_plan->isBeatmatch() || m_windowEndMs <= m_windowStartMs) {
        return std::nullopt;
    }
    return std::pair{m_windowStartMs, m_windowEndMs};
}

bool TransitionPlanViewModel::fellBack() const {
    return m_plan && m_plan->fallbackReason.has_value();
}

std::optional<int> TransitionPlanViewModel::nowBar() const {
    if (!m_plan || m_nowMs < 0.0 || m_nowMs > m_outgoing.durationMs) {
        return std::nullopt;
    }
    return barAt(Side::Outgoing, m_nowMs);
}

QString TransitionPlanViewModel::nowLabel() const {
    QString label = tr("Now");
    if (const auto bar = nowBar()) {
        label += kSeparator + tr("bar %1").arg(*bar);
    }
    return label;
}

} // namespace crate::widgets
