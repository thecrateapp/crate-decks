#include "crate/smartmix/smartmixdirector.h"

#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

#include "control/controlproxy.h"
#include "crate/smartmix/controlautomation.h"
#include "crate/smartmix/curves.h"
#include "crate/smartmix/eqmap.h"
#include "engine/enginexfader.h"
#include "mixer/playerinfo.h"
#include "moc_smartmixdirector.cpp"
#include "track/track.h"
#include "util/logger.h"
#include "waveform/visualplayposition.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("SmartMixDirector");

const QString kMixerGroup = QStringLiteral("[Master]");
const QString kReplayGainGroup = QStringLiteral("[ReplayGain]");
// EngineChannel::ChannelOrientation, as [ChannelN],orientation holds it.
constexpr int kOrientationLeft = 0;
constexpr int kOrientationRight = 2;
// The most the pregain knob takes.
constexpr double kMaxPregain = 4.0;
// The crossfader's position is found by bisection to this precision.
constexpr int kCrossfaderBisections = 32;

class EngineEnvironment : public SmartMixEnvironment {
  public:
    explicit EngineEnvironment(PlayerInfo& playerInfo)
            : m_playerInfo(playerInfo) {
        m_clock.start();
    }

    double nowMs() const override {
        return static_cast<double>(m_clock.nsecsElapsed()) / 1e6;
    }

    double enginePlayPosition(const QString& group) const override {
        return VisualPlayPosition::getVisualPlayPosition(group)->getEnginePlayPos();
    }

    QString trackKey(const QString& group) const override {
        const TrackPointer pTrack = m_playerInfo.getTrackInfo(group);
        if (!pTrack) {
            return {};
        }
        // The object, not only the id: a track without one is still told
        // apart from the next.
        return QStringLiteral("%1:%2").arg(
                QString::number(reinterpret_cast<quintptr>(pTrack.get()), 16),
                pTrack->getId().toString());
    }

  private:
    PlayerInfo& m_playerInfo;
    QElapsedTimer m_clock;
};

QString reasonOf(const TransitionPlan& plan) {
    return plan.fallbackReason ? toString(*plan.fallbackReason) : QStringLiteral("completed");
}

} // namespace

std::unique_ptr<SmartMixEnvironment> SmartMixEnvironment::engine(PlayerInfo& playerInfo) {
    return std::make_unique<EngineEnvironment>(playerInfo);
}

/// The controls of one deck. The automated ones (pregain, the bass EQ and,
/// on the incoming deck, the tempo) remember their value from before the
/// transition, to put it back.
struct SmartMixDirector::Deck {
    explicit Deck(const QString& deckGroup)
            : group(deckGroup),
              play(group,
                      QStringLiteral("play"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              playPosition(group,
                      QStringLiteral("playposition"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              duration(group,
                      QStringLiteral("duration"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              bpm(group,
                      QStringLiteral("bpm"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              beatDistance(group,
                      QStringLiteral("beat_distance"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              quantize(group,
                      QStringLiteral("quantize"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              syncEnabled(group,
                      QStringLiteral("sync_enabled"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              keylock(group,
                      QStringLiteral("keylock"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              orientation(group,
                      QStringLiteral("orientation"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              replayGain(group,
                      QStringLiteral("replaygain"),
                      nullptr,
                      ControlFlag::NoAssertIfMissing),
              midGain(eqmap::effectGroup(group),
                      eqmap::kMidGain,
                      nullptr,
                      ControlFlag::NoWarnIfMissing),
              highGain(eqmap::effectGroup(group),
                      eqmap::kHighGain,
                      nullptr,
                      ControlFlag::NoWarnIfMissing),
              lowKill(eqmap::effectGroup(group),
                      eqmap::kLowKill,
                      nullptr,
                      ControlFlag::NoWarnIfMissing),
              midKill(eqmap::effectGroup(group),
                      eqmap::kMidKill,
                      nullptr,
                      ControlFlag::NoWarnIfMissing),
              highKill(eqmap::effectGroup(group),
                      eqmap::kHighKill,
                      nullptr,
                      ControlFlag::NoWarnIfMissing),
              pregain(std::make_unique<ControlAutomation>(
                      ConfigKey(group, QStringLiteral("pregain")))),
              lowGain(std::make_unique<ControlAutomation>(
                      ConfigKey(eqmap::effectGroup(group), eqmap::kLowGain))),
              rate(std::make_unique<ControlAutomation>(
                      ConfigKey(group, QStringLiteral("rate_ratio")))) {
    }

    QString group;
    QString trackKey;
    ControlProxy play;
    ControlProxy playPosition;
    ControlProxy duration;
    ControlProxy bpm;
    ControlProxy beatDistance;
    ControlProxy quantize;
    ControlProxy syncEnabled;
    ControlProxy keylock;
    ControlProxy orientation;
    /// The track's ReplayGain ratio, 0 without data
    ControlProxy replayGain;
    ControlProxy midGain;
    ControlProxy highGain;
    ControlProxy lowKill;
    ControlProxy midKill;
    ControlProxy highKill;
    std::unique_ptr<ControlAutomation> pregain;
    std::unique_ptr<ControlAutomation> lowGain;
    std::unique_ptr<ControlAutomation> rate;
    /// The values from before the transition, which a cancelled one puts
    /// back.
    double pregainBefore = 1.0;
    double lowGainBefore = eqmap::kNeutralGain;
    double rateBefore = 1.0;
    double quantizeBefore = 0.0;
    double syncBefore = 0.0;
    /// What the transition's levels and cuts multiply: the outgoing deck's
    /// own setting, a neutral one on the incoming deck.
    double pregainBase = 1.0;
    double lowCutDbBase = 0.0;
    /// Written at the end of the tick, the seek before the play button.
    std::optional<double> pendingSeekMs;
    std::optional<double> pendingPlay;

    bool isPlaying() const {
        return play.toBool();
    }
};

/// The crossfader and its settings (Preferences > Mixer).
struct SmartMixDirector::Mixer {
    Mixer()
            : crossfader(std::make_unique<ControlAutomation>(
                      ConfigKey(kMixerGroup, QStringLiteral("crossfader")))),
              mode(QString(EngineXfader::kXfaderConfigKey),
                      QStringLiteral("xFaderMode"),
                      nullptr,
                      ControlFlag::NoWarnIfMissing),
              curve(QString(EngineXfader::kXfaderConfigKey),
                      QStringLiteral("xFaderCurve"),
                      nullptr,
                      ControlFlag::NoWarnIfMissing),
              calibration(QString(EngineXfader::kXfaderConfigKey),
                      QStringLiteral("xFaderCalibration"),
                      nullptr,
                      ControlFlag::NoWarnIfMissing),
              reverse(QString(EngineXfader::kXfaderConfigKey),
                      QStringLiteral("xFaderReverse"),
                      nullptr,
                      ControlFlag::NoWarnIfMissing) {
    }

    /// The crossfader's gains for the outgoing and the incoming deck at
    /// `position`, with the user's curve, as the engine computes them.
    std::pair<double, double> gainsAt(double position) const {
        const double transform =
                curve.valid() ? curve.get() : EngineXfader::kTransformDefault;
        const double powerCalibration = calibration.valid()
                ? calibration.get()
                : EngineXfader::getPowerCalibration(transform);
        CSAMPLE_GAIN left = 1.0f;
        CSAMPLE_GAIN right = 1.0f;
        EngineXfader::getXfadeGains(position,
                transform,
                powerCalibration,
                mode.valid() ? mode.get() : MIXXX_XFADER_ADDITIVE,
                reverse.valid() && reverse.toBool(),
                &left,
                &right);
        return outgoingOnLeft ? std::pair<double, double>{left, right}
                              : std::pair<double, double>{right, left};
    }

    /// Where the crossfader is all on one deck: `towardsIncoming` 0 for the
    /// outgoing deck's end, 1 for the incoming one's.
    double position(double towardsIncoming) const {
        // xFaderReverse swaps the crossfader's ends.
        const bool reversed = reverse.valid() && reverse.toBool();
        const double outgoingEnd = outgoingOnLeft != reversed ? -1.0 : 1.0;
        return outgoingEnd * (1.0 - 2.0 * towardsIncoming);
    }

    std::unique_ptr<ControlAutomation> crossfader;
    ControlProxy mode;
    ControlProxy curve;
    ControlProxy calibration;
    ControlProxy reverse;
    /// The decks are on opposite sides of the crossfader, so the director
    /// crossfades with it; otherwise the pregains carry the whole crossfade.
    bool crossfades = false;
    bool outgoingOnLeft = true;
};

/// The incoming deck easing back to its tempo after a beatmatch.
struct SmartMixDirector::TempoRestore {
    std::unique_ptr<ControlAutomation> rate;
    double fromRate = 1.0;
    double toRate = 1.0;
    double startMs = 0.0;
};

SmartMixDirector::SmartMixDirector(PlayerInfo& playerInfo, QObject* pParent)
        : QObject(pParent),
          m_pEnvironment(SmartMixEnvironment::engine(playerInfo)),
          m_ownTimers(true) {
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &SmartMixDirector::tick);
    m_startTimer.setTimerType(Qt::PreciseTimer);
    m_startTimer.setSingleShot(true);
    connect(&m_startTimer, &QTimer::timeout, this, &SmartMixDirector::tick);
}

SmartMixDirector::SmartMixDirector(
        std::unique_ptr<SmartMixEnvironment> pEnvironment, QObject* pParent)
        : QObject(pParent),
          m_pEnvironment(std::move(pEnvironment)),
          m_ownTimers(false) {
}

SmartMixDirector::~SmartMixDirector() = default;

bool SmartMixDirector::isActive() const {
    return m_state == State::Armed || m_state == State::Preparing || m_state == State::Running;
}

bool SmartMixDirector::isRestoringTempo() const {
    return m_pTempoRestore != nullptr;
}

std::optional<double> SmartMixDirector::incomingRateBefore() const {
    if (!m_prepared || !m_pIncoming || !isActive()) {
        return std::nullopt;
    }
    return m_pIncoming->rateBefore;
}

std::unique_ptr<SmartMixDirector::Deck> SmartMixDirector::connectDeck(const QString& group) {
    auto pDeck = std::make_unique<Deck>(group);
    pDeck->trackKey = m_pEnvironment->trackKey(group);
    watchOverrides(pDeck->pregain.get());
    watchOverrides(pDeck->lowGain.get());
    watchOverrides(pDeck->rate.get());
    return pDeck;
}

void SmartMixDirector::watchOverrides(ControlAutomation* pAutomation) {
    connect(pAutomation, &ControlAutomation::overridden, this, [this](const ConfigKey& key) {
        kLogger.info() << "Manual change of" << key << "- no longer automated";
        m_report.overridden.append(key);
        emit controlOverridden(key);
    });
}

bool SmartMixDirector::arm(const QString& outgoingGroup,
        const QString& incomingGroup,
        const Options& options) {
    if (isActive() || outgoingGroup == incomingGroup) {
        return false;
    }
    auto pOutgoing = connectDeck(outgoingGroup);
    auto pIncoming = connectDeck(incomingGroup);
    if (pOutgoing->trackKey.isEmpty() || pIncoming->trackKey.isEmpty() ||
            !pOutgoing->isPlaying() || pIncoming->isPlaying()) {
        return false;
    }
    m_pOutgoing = std::move(pOutgoing);
    m_pIncoming = std::move(pIncoming);
    m_pMixer = std::make_unique<Mixer>();
    watchOverrides(m_pMixer->crossfader.get());
    m_options = options;
    m_report = Report();
    m_plan.reset();
    m_startDueMs.reset();
    m_prepared = false;
    m_held = options.holdStart;
    m_armedAtMs = m_pEnvironment->nowMs();
    m_previousOutgoingMs = positionMs(*m_pOutgoing);
    kLogger.info() << "Armed" << outgoingGroup << "->" << incomingGroup;
    setState(State::Armed);
    updateTimer();
    return true;
}

bool SmartMixDirector::setPlan(const TransitionPlan& plan) {
    if (m_state != State::Armed || m_plan) {
        return false;
    }
    // The server's pick, or the next entry of its menu that fits (plan
    // F6b.PR1)
    QString problem;
    const std::optional<TransitionPlan> playable =
            plan.playable(static_cast<qint64>(durationMs(*m_pOutgoing)),
                    static_cast<qint64>(durationMs(*m_pIncoming)),
                    &problem);
    if (!playable) {
        kLogger.warning() << "Plan rejected:" << problem;
        m_report.detail = problem;
        finish(State::FellBack, QStringLiteral("invalid_plan"));
        return false;
    }
    if (playable->candidateIndex != plan.candidateIndex) {
        kLogger.info() << "Candidate" << playable->candidateIndex
                       << "of the server's menu instead of its pick:" << problem;
        m_report.detail = problem;
    }
    m_report.candidateIndex = playable->candidateIndex;
    m_plan = playable;
    m_startAtMs = startAtMs(*m_plan, durationMs(*m_pOutgoing));
    m_report.plannedStartOutgoingMs = m_startAtMs;
    const double waitMs = (m_startAtMs - positionMs(*m_pOutgoing)) / outgoingRate();
    m_cueDeadlineMs = m_pEnvironment->nowMs() + std::max(0.0, waitMs) + kCueDeadlineMarginMs;
    kLogger.info() << "Plan" << toString(m_plan->mode) << m_plan->durationMs << "ms from"
                   << m_startAtMs << "ms of the outgoing track"
                   << (m_plan->fallbackReason ? toString(*m_plan->fallbackReason) : QString())
                   << "- bass handoff" << (m_plan->hasBassHandoff() ? "balanced" : "none") << "at"
                   << m_plan->handoffProgress;
    tick();
    return true;
}

double SmartMixDirector::startAtMs(const TransitionPlan& plan, double outgoingDurationMs) {
    // As Android's NativeMixTrigger.effectiveCueMs(): the transition ends
    // before the outgoing track does.
    const double latestCueMs = std::max(0.0,
            outgoingDurationMs - static_cast<double>(plan.durationMs) - kStartSafetyLeadMs);
    return plan.outgoingCueMs > 0 ? std::min(static_cast<double>(plan.outgoingCueMs), latestCueMs)
                                  : latestCueMs;
}

bool SmartMixDirector::release() {
    if (!isHeld()) {
        return false;
    }
    kLogger.info() << "Released: starts at its cue";
    m_held = false;
    tick();
    return true;
}

void SmartMixDirector::cancel() {
    m_pTempoRestore.reset();
    if (isActive()) {
        finish(State::Cancelled, QStringLiteral("cancelled"));
    }
    updateTimer();
}

void SmartMixDirector::setState(State state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state);
}

double SmartMixDirector::durationMs(const Deck& deck) const {
    return deck.duration.get() * 1000.0;
}

double SmartMixDirector::positionMs(const Deck& deck) const {
    return m_pEnvironment->enginePlayPosition(deck.group) * durationMs(deck);
}

double SmartMixDirector::outgoingRate() const {
    const double rate = m_pOutgoing->rate->get();
    // A deck playing backwards or not at all has no tempo to follow.
    return rate > 0.0 ? rate : 1.0;
}

void SmartMixDirector::tick() {
    const double nowMs = m_pEnvironment->nowMs();
    if (m_pTempoRestore) {
        stepTempoRestore(nowMs);
    }
    switch (m_state) {
    case State::Armed:
    case State::Preparing:
        stepWaiting(nowMs);
        break;
    case State::Running:
        stepRunning(nowMs);
        break;
    case State::Idle:
    case State::Completed:
    case State::Cancelled:
    case State::FellBack:
        break;
    }
    commit();
    updateTimer();
}

std::optional<QString> SmartMixDirector::interruption() {
    if (m_pEnvironment->trackKey(m_pOutgoing->group) != m_pOutgoing->trackKey ||
            m_pEnvironment->trackKey(m_pIncoming->group) != m_pIncoming->trackKey) {
        return QStringLiteral("track_changed");
    }
    if (!m_pOutgoing->isPlaying()) {
        return QStringLiteral("outgoing_stopped");
    }
    const double outgoingMs = positionMs(*m_pOutgoing);
    const double previousMs = m_previousOutgoingMs;
    m_previousOutgoingMs = outgoingMs;
    if (outgoingMs < previousMs - kBackwardJumpToleranceMs) {
        return QStringLiteral("outgoing_jumped_back");
    }
    if (m_state == State::Running) {
        if (!m_pIncoming->isPlaying()) {
            return QStringLiteral("incoming_stopped");
        }
    } else if (m_pIncoming->isPlaying()) {
        // Someone else started the deck the director was preparing.
        return QStringLiteral("incoming_started");
    }
    return std::nullopt;
}

void SmartMixDirector::stepWaiting(double nowMs) {
    if (const auto reason = interruption()) {
        finish(State::Cancelled, *reason);
        return;
    }
    if (!m_plan) {
        if (nowMs - m_armedAtMs >= kPlanDeadlineMs) {
            finish(State::FellBack, QStringLiteral("missing_plan"));
        }
        return;
    }
    if (nowMs > m_cueDeadlineMs) {
        finish(State::Cancelled, QStringLiteral("deadline"));
        return;
    }
    const double rate = outgoingRate();
    const double outgoingMs = positionMs(*m_pOutgoing);
    const double remainingMs = (m_startAtMs - outgoingMs) / rate;
    if (m_state == State::Armed && (remainingMs <= kPrepareLeadMs || m_held)) {
        prepare();
    }
    if (m_state != State::Preparing) {
        return;
    }
    if (m_plan->isBeatmatch()) {
        // Follows the outgoing deck's tempo until the start.
        m_pIncoming->rate->set(m_plan->incomingTempoRatio * rate);
    }
    if (m_held) {
        return;
    }
    if (!m_startDueMs && remainingMs <= 2.0 * kTickMs) {
        // The start is within the next tick: fix its time once, from the
        // outgoing deck's beat clock if the cue is on its next beat.
        double delayMs = std::max(0.0, remainingMs);
        const double bpm = m_pOutgoing->bpm.get();
        const double beatDistance = m_pOutgoing->beatDistance.get();
        if (bpm > 0.0 && beatDistance >= 0.0 && beatDistance < 1.0) {
            const double beatDelayMs = (1.0 - beatDistance) * 60000.0 / bpm;
            const double nextBeatMs = outgoingMs + beatDelayMs * rate;
            if (std::abs(nextBeatMs - m_startAtMs) <= kBeatSnapToleranceMs) {
                delayMs = beatDelayMs;
                m_report.usedBeatClock = true;
            }
        }
        m_startDueMs = nowMs + delayMs;
        if (m_ownTimers && delayMs > 0.0) {
            m_startTimer.start(static_cast<int>(std::lround(delayMs)));
        }
    }
    if (!m_startDueMs) {
        return;
    }
    if (nowMs >= *m_startDueMs) {
        startIncoming(nowMs);
    } else if (m_ownTimers && !m_startTimer.isActive()) {
        // The timer came a little early: wait for the rest.
        m_startTimer.start(static_cast<int>(std::ceil(*m_startDueMs - nowMs)));
    }
}

void SmartMixDirector::prepare() {
    Deck& incoming = *m_pIncoming;
    Deck& outgoing = *m_pOutgoing;
    m_prepared = true;
    for (Deck* pDeck : {&incoming, &outgoing}) {
        pDeck->pregainBefore = pDeck->pregain->get();
        pDeck->lowGainBefore = pDeck->lowGain->get();
        pDeck->rateBefore = pDeck->rate->get();
        pDeck->quantizeBefore = pDeck->quantize.get();
        pDeck->syncBefore = pDeck->syncEnabled.get();
    }
    outgoing.pregainBase = outgoing.pregainBefore;
    outgoing.lowCutDbBase = outgoing.lowGain->isValid()
            ? eqmap::cutDbForLowGain(outgoing.lowGainBefore)
            : 0.0;
    incoming.pregainBase = 1.0;
    incoming.lowCutDbBase = 0.0;
    // ReplayGain already matched the decks' loudness: the incoming deck
    // stages only the residual of the plan's gain (G4 finding 3)
    const ControlProxy replayGainOn(kReplayGainGroup,
            QStringLiteral("ReplayGainEnabled"),
            nullptr,
            ControlFlag::NoAssertIfMissing);
    m_report.replayGainDifferenceDb = curves::replayGainDifferenceDb(replayGainOn.valid() &&
                    replayGainOn.toBool(),
            outgoing.replayGain.valid() ? outgoing.replayGain.get() : 0.0,
            incoming.replayGain.valid() ? incoming.replayGain.get() : 0.0);
    m_report.incomingStagingDb =
            curves::incomingStagingDb(m_plan->incomingGainDb, m_report.replayGainDifferenceDb);
    m_lastProgress = -1.0;
    m_loggedStep = -1;
    for (const Deck* pDeck : {&outgoing, &incoming}) {
        kLogger.debug() << "Lows before" << pDeck->group << "valid" << pDeck->lowGain->isValid()
                        << "key" << pDeck->lowGain->key().group << pDeck->lowGain->key().item
                        << "value" << pDeck->lowGain->get();
    }

    // Quantize and sync lock would move the incoming deck's start or tempo
    // away from the plan (spike review); they come back at the end.
    if (incoming.quantize.toBool()) {
        incoming.quantize.set(0.0);
    }
    if (incoming.syncEnabled.toBool()) {
        incoming.syncEnabled.set(0.0);
    }
    if (m_options.keylock && incoming.keylock.toBool() != *m_options.keylock) {
        incoming.keylock.set(*m_options.keylock ? 1.0 : 0.0);
    }
    // A neutral EQ: the plan's levels and bass handoff assume one.
    for (ControlProxy* pGain : {&incoming.midGain, &incoming.highGain}) {
        if (pGain->valid() && pGain->get() != eqmap::kNeutralGain) {
            pGain->set(eqmap::kNeutralGain);
        }
    }
    for (ControlProxy* pKill : {&incoming.lowKill, &incoming.midKill, &incoming.highKill}) {
        if (pKill->valid() && pKill->toBool()) {
            pKill->set(0.0);
        }
    }
    incoming.pendingSeekMs = static_cast<double>(m_plan->startIncomingCueMs());

    m_pMixer->outgoingOnLeft =
            static_cast<int>(outgoing.orientation.get()) != kOrientationRight;
    const int outgoingSide = static_cast<int>(outgoing.orientation.get());
    const int incomingSide = static_cast<int>(incoming.orientation.get());
    m_pMixer->crossfades = (outgoingSide == kOrientationLeft &&
                                   incomingSide == kOrientationRight) ||
            (outgoingSide == kOrientationRight &&
                    incomingSide == kOrientationLeft);
    if (m_pMixer->crossfades && !m_pMixer->crossfader->isOverridden()) {
        // The transition starts with the crossfader on the outgoing deck's
        // end. Where the DJ left it elsewhere (centred, say), the outgoing
        // deck keeps the level it has there instead of jumping to the end's:
        // +3 dB for a centred constant-power crossfader.
        const double hereGain = m_pMixer->gainsAt(m_pMixer->crossfader->get()).first;
        const double endGain = m_pMixer->gainsAt(m_pMixer->position(0.0)).first;
        if (endGain > 0.0 && hereGain < endGain) {
            outgoing.pregainBase *= hereGain / endGain;
        }
    }
    // The incoming deck at the start of the transition: silent on the
    // crossfader, or through its pregain.
    applyMix(0.0);
    kLogger.info() << "Preparing" << incoming.group << "at" << *incoming.pendingSeekMs << "ms"
                   << (m_pMixer->crossfades ? "" : "(no crossfader: decks on the same side)");
    setState(State::Preparing);
}

void SmartMixDirector::startIncoming(double nowMs) {
    Deck& incoming = *m_pIncoming;
    const double rate = outgoingRate();
    const double outgoingMs = positionMs(*m_pOutgoing);
    const double startMs = std::max(m_startAtMs, outgoingMs);
    // As Android's NativeMixTrigger: shorten the transition to what is left
    // of the outgoing track, and give up if too little is.
    const double availableMs = durationMs(*m_pOutgoing) - startMs - kStartSafetyLeadMs;
    const double planMs = static_cast<double>(m_plan->durationMs);
    // A gapless plan needs no room: it is a cut.
    const double boundedMs = planMs > 0.0 ? std::min(planMs, availableMs) : 0.0;
    if (boundedMs < std::min(planMs, kMinimumTransitionMs)) {
        finish(State::FellBack, QStringLiteral("insufficient_window"));
        return;
    }
    // The incoming deck starts at the next audio callback, which plays the
    // outgoing deck from its engine position: off the cue by this much, in
    // output time, early or late (G4 finding 2)
    const double offsetMs = (outgoingMs - m_startAtMs) / rate;
    const double lateMs = std::max(0.0, offsetMs);
    double incomingStartMs = static_cast<double>(m_plan->startIncomingCueMs());
    if (std::abs(offsetMs) > kStartAlignToleranceMs) {
        // Start where the incoming deck would be, so the beats stay aligned
        // (spike review): ahead on a late start (a long outro, a seek
        // forward), back on an early one
        const double incomingRate = m_plan->isBeatmatch()
                ? m_plan->incomingTempoRatio * rate
                : std::max(incoming.rate->get(), 0.0);
        incomingStartMs = std::max(0.0, incomingStartMs + offsetMs * incomingRate);
        incoming.pendingSeekMs = incomingStartMs;
    }
    incoming.pendingPlay = 1.0;
    m_report.mixed = true;
    m_report.lateMs = lateMs;
    m_report.startOffsetMs = offsetMs;
    m_report.startOutgoingMs = outgoingMs;
    m_report.incomingStartMs = incomingStartMs;
    m_report.durationMs = boundedMs;
    m_runStartMs = nowMs;
    m_runRate = rate;
    kLogger.info() << "Incoming" << incoming.group << "started at" << incomingStartMs
                   << "ms with the outgoing deck at" << outgoingMs << "ms (planned"
                   << m_startAtMs << "ms, off by" << offsetMs << "ms, beat clock"
                   << m_report.usedBeatClock << ")";
    setState(State::Running);
    if (boundedMs <= 0.0) {
        // Gapless: a cut to the incoming deck.
        complete();
        return;
    }
    applyMix(0.0);
}

void SmartMixDirector::stepRunning(double nowMs) {
    if (const auto reason = interruption()) {
        finish(State::Cancelled, *reason);
        return;
    }
    // Wall clock at the outgoing deck's tempo, so a paused or looping deck
    // cannot freeze the transition (spike review).
    const double progress = (nowMs - m_runStartMs) * m_runRate / m_report.durationMs;
    if (progress >= 1.0) {
        complete();
        return;
    }
    m_lastProgress = progress;
    applyMix(progress);
}

void SmartMixDirector::applyMix(double progress) {
    const TransitionPlan& plan = *m_plan;
    Deck& outgoing = *m_pOutgoing;
    Deck& incoming = *m_pIncoming;
    Mixer& mixer = *m_pMixer;
    const bool djBlend = plan.curve == TransitionPlan::Curve::DjBlend;
    const auto gains = curves::equalPower(curves::crossfade(progress, djBlend));
    const double outgoingStaging = curves::stagingFactor(
            curves::Role::Outgoing, progress, curves::stagingGain(plan.outgoingGainDb));
    const double incomingStaging = curves::stagingFactor(
            curves::Role::Incoming, progress, curves::stagingGain(m_report.incomingStagingDb));

    // The levels Android gives each deck: the crossfade's equal-power gains
    // times the plan's staging. The crossfader sets their balance with the
    // user's crossfader curve, and the pregains bring both to the level the
    // curve misses (none with a constant-power curve).
    double outgoingLevel = gains.outgoing * outgoingStaging;
    double incomingLevel = gains.incoming * incomingStaging;
    if (mixer.crossfades && !mixer.crossfader->isOverridden()) {
        // The position whose gains have the curve's ratio, by bisection: the
        // incoming deck's share grows along the crossfader.
        const auto mismatch = [&](double towardsIncoming) {
            const auto [outgoingGain, incomingGain] =
                    mixer.gainsAt(mixer.position(towardsIncoming));
            return incomingGain * gains.outgoing - outgoingGain * gains.incoming;
        };
        double low = 0.0;
        double high = 1.0;
        if (mismatch(low) >= 0.0) {
            high = low;
        } else if (mismatch(high) > 0.0) {
            for (int i = 0; i < kCrossfaderBisections; ++i) {
                const double middle = 0.5 * (low + high);
                (mismatch(middle) >= 0.0 ? high : low) = middle;
            }
        }
        const double position = mixer.position(high);
        mixer.crossfader->set(position);
        const auto [outgoingGain, incomingGain] = mixer.gainsAt(position);
        const double scale = outgoingGain >= incomingGain ? gains.outgoing / outgoingGain
                                                          : gains.incoming / incomingGain;
        outgoingLevel = outgoingStaging * scale;
        incomingLevel = incomingStaging * scale;
    } else if (mixer.crossfades) {
        // The DJ has the crossfader: only the plan's staging.
        outgoingLevel = outgoingStaging;
        incomingLevel = incomingStaging;
    }
    outgoing.pregain->set(std::clamp(outgoing.pregainBase * outgoingLevel, 0.0, kMaxPregain));
    incoming.pregain->set(std::clamp(incoming.pregainBase * incomingLevel, 0.0, kMaxPregain));

    if (plan.hasBassHandoff()) {
        outgoing.lowGain->set(eqmap::lowGainForCutDb(outgoing.lowCutDbBase +
                curves::bassCutDb(curves::Role::Outgoing, progress, plan.handoffProgress)));
        incoming.lowGain->set(eqmap::lowGainForCutDb(
                curves::bassCutDb(curves::Role::Incoming, progress, plan.handoffProgress)));
    } else {
        incoming.lowGain->set(eqmap::kNeutralGain);
    }
    if (plan.isBeatmatch()) {
        incoming.rate->set(plan.incomingTempoRatio * outgoingRate());
    }
}

void SmartMixDirector::complete() {
    Deck& outgoing = *m_pOutgoing;
    Deck& incoming = *m_pIncoming;
    if (m_pMixer->crossfades) {
        m_pMixer->crossfader->set(m_pMixer->position(1.0));
    }
    // As Android: the outgoing deck stops; its caller (AutoDJ) loads the
    // next track into it.
    outgoing.play.set(0.0);
    if (m_plan->isBeatmatch() && !incoming.rate->isOverridden() &&
            incoming.syncBefore == 0.0) {
        // Android's tempo restore, back to the deck's own tempo. Sync lock,
        // if it was on, sets the tempo itself instead.
        auto pRestore = std::make_unique<TempoRestore>();
        pRestore->fromRate = m_plan->incomingTempoRatio * outgoingRate();
        pRestore->toRate = incoming.rateBefore;
        pRestore->startMs = m_pEnvironment->nowMs();
        pRestore->rate = std::move(incoming.rate);
        pRestore->rate->set(pRestore->fromRate);
        incoming.rate = std::make_unique<ControlAutomation>(pRestore->rate->key());
        m_pTempoRestore = std::move(pRestore);
    }
    finish(m_plan->fallbackReason ? State::FellBack : State::Completed, reasonOf(*m_plan));
}

void SmartMixDirector::finish(State state, const QString& reason) {
    m_startTimer.stop();
    m_report.reason = reason;
    if (m_prepared) {
        restoreDecks();
        commit();
    }
    m_prepared = false;
    m_startDueMs.reset();
    if (state == State::Completed) {
        kLogger.info() << "Transition completed";
    } else {
        kLogger.info() << "Transition ended:" << reason;
    }
    setState(state);
    emit finished(state, reason);
}

void SmartMixDirector::restoreDecks() {
    Deck& outgoing = *m_pOutgoing;
    Deck& incoming = *m_pIncoming;
    if (incoming.quantizeBefore != incoming.quantize.get()) {
        incoming.quantize.set(incoming.quantizeBefore);
    }
    if (incoming.syncBefore != incoming.syncEnabled.get()) {
        incoming.syncEnabled.set(incoming.syncBefore);
    }
    if (m_state == State::Running) {
        // The incoming deck plays on (the transition completed, or was
        // cancelled midway): at its level, without the plan's cut.
        incoming.pregain->set(incoming.pregainBase);
        incoming.lowGain->set(eqmap::kNeutralGain);
    } else {
        // Never started: the incoming deck as it was.
        incoming.pendingPlay.reset();
        incoming.pregain->set(incoming.pregainBefore);
        incoming.lowGain->set(incoming.lowGainBefore);
        if (m_plan && m_plan->isBeatmatch()) {
            incoming.rate->set(incoming.rateBefore);
        }
    }
    outgoing.pregain->set(outgoing.pregainBefore);
    outgoing.lowGain->set(outgoing.lowGainBefore);
}

void SmartMixDirector::commit() {
    for (Deck* pDeck : {m_pOutgoing.get(), m_pIncoming.get()}) {
        if (pDeck == nullptr) {
            continue;
        }
        if (pDeck->pendingSeekMs) {
            const double trackMs = durationMs(*pDeck);
            if (trackMs > 0.0) {
                pDeck->playPosition.set(std::clamp(*pDeck->pendingSeekMs / trackMs, 0.0, 1.0));
            }
            pDeck->pendingSeekMs.reset();
        }
        pDeck->rate->commit();
        pDeck->pregain->commit();
        pDeck->lowGain->commit();
    }
    if (m_pMixer) {
        m_pMixer->crossfader->commit();
    }
    // Diagnostics of the lows each tenth of the transition (bass handoff)
    if (m_lastProgress >= 0.0 && static_cast<int>(m_lastProgress * 10) != m_loggedStep) {
        m_loggedStep = static_cast<int>(m_lastProgress * 10);
        for (const Deck* pDeck : {m_pOutgoing.get(), m_pIncoming.get()}) {
            if (pDeck != nullptr) {
                kLogger.debug() << "Lows at" << m_lastProgress << pDeck->group << "valid"
                                << pDeck->lowGain->isValid() << "value" << pDeck->lowGain->get()
                                << "overridden" << pDeck->lowGain->isOverridden() << "writes"
                                << pDeck->lowGain->writeCount() << "pregain"
                                << pDeck->pregain->get();
            }
        }
    }
    if (m_pTempoRestore) {
        m_pTempoRestore->rate->commit();
    }
    for (Deck* pDeck : {m_pOutgoing.get(), m_pIncoming.get()}) {
        if (pDeck != nullptr && pDeck->pendingPlay) {
            pDeck->play.set(*pDeck->pendingPlay);
            pDeck->pendingPlay.reset();
        }
    }
}

void SmartMixDirector::stepTempoRestore(double nowMs) {
    TempoRestore& restore = *m_pTempoRestore;
    const double elapsedMs = nowMs - restore.startMs;
    if (restore.rate->checkOverride()) {
        m_pTempoRestore.reset();
        return;
    }
    restore.rate->set(curves::tempoRestore(restore.fromRate, restore.toRate, elapsedMs));
    if (elapsedMs >= curves::kTempoRestoreMs) {
        restore.rate->commit();
        m_pTempoRestore.reset();
    }
}

void SmartMixDirector::updateTimer() {
    if (!m_ownTimers) {
        return;
    }
    const bool needed = isActive() || m_pTempoRestore;
    if (needed && !m_timer.isActive()) {
        m_timer.start();
    } else if (!needed && m_timer.isActive()) {
        m_timer.stop();
    }
}

} // namespace crate
