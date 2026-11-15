#include "crate/smartmix/smartmixassist.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <algorithm>

#include "control/controlobject.h"
#include "crate/cratepreferences.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_smartmixassist.cpp"
#include "track/track.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::SmartMixAssist");

const ConfigKey kAutoDjEnabled(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"));
static_assert(CratePreferences::kMaximumAssistTransitionSeconds * 1000 <=
                PlanContext::kMaxPreferredDurationMs,
        "the assistant's longest transition is more than the server takes");
// The decks the assistant looks at
constexpr int kMaxDecks = 4;

double controlValue(const QString& group, const QString& item, double fallback = 0.0) {
    const ControlObject* pControl =
            ControlObject::getControl(ConfigKey(group, item), ControlFlag::NoWarnIfMissing);
    return pControl ? pControl->get() : fallback;
}

int deckCount() {
    return std::clamp(static_cast<int>(controlValue(
                              QStringLiteral("[App]"), QStringLiteral("num_decks"), 2.0)),
            0,
            kMaxDecks);
}

} // namespace

SmartMixAssist::SmartMixAssist(Options options, QObject* pParent)
        : QObject(pParent),
          m_options(std::move(options)),
          m_pPlayerInfo(m_options.pPlayerInfo ? m_options.pPlayerInfo : &PlayerInfo::instance()),
          m_pEnvironment(m_options.pEnvironment ? std::move(m_options.pEnvironment)
                                                : SmartMixEnvironment::engine()),
          m_pDirector(m_options.pDirector ? std::move(m_options.pDirector)
                                          : std::make_unique<SmartMixDirector>()) {
    if (m_options.mappable) {
        const auto button = [this](const QString& item, std::function<void()> action) {
            auto pButton = std::make_unique<ControlPushButton>(ConfigKey(kControlGroup, item));
            connect(pButton.get(),
                    &ControlObject::valueChanged,
                    this,
                    [action = std::move(action)](double value) {
                        if (value > 0) {
                            action();
                        }
                    });
            return pButton;
        };
        m_pPrepare = button(QStringLiteral("prepare"), [this] { prepare(); });
        m_pExecute = button(QStringLiteral("execute"), [this] { execute(); });
        m_pCancel = button(QStringLiteral("cancel"), [this] { cancel(); });
    }
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(m_options.settleTime);
    connect(&m_settleTimer, &QTimer::timeout, this, &SmartMixAssist::refresh);
    connect(m_pDirector.get(), &SmartMixDirector::stateChanged, this, &SmartMixAssist::changed);
    // Queued: the director emits it from inside its own tick or cancel()
    connect(m_pDirector.get(),
            &SmartMixDirector::finished,
            this,
            &SmartMixAssist::onDirectorFinished,
            Qt::QueuedConnection);
    if (m_options.pPreferences) {
        // Its plan's context changes: refresh() asks again
        connect(m_options.pPreferences,
                &CratePreferences::smartMixAssistTransitionChanged,
                this,
                &SmartMixAssist::scheduleRefresh);
    }
    connect(m_pPlayerInfo, &PlayerInfo::trackChanged, this, &SmartMixAssist::scheduleRefresh);
    connect(m_pPlayerInfo,
            &PlayerInfo::currentPlayingDeckChanged,
            this,
            &SmartMixAssist::scheduleRefresh);
    // The decks and Auto DJ may come after it: the first refresh looks again
    scheduleRefresh();
}

SmartMixAssist::~SmartMixAssist() {
    m_pDirector->disconnect(this);
    m_pDirector->cancel();
}

void SmartMixAssist::watchControls() {
    if (!m_pAutoDjEnabled && ControlObject::exists(kAutoDjEnabled)) {
        m_pAutoDjEnabled = std::make_unique<ControlProxy>(kAutoDjEnabled, this);
        m_pAutoDjEnabled->connectValueChanged(this, &SmartMixAssist::onAutoDjChanged);
    }
    for (int index = static_cast<int>(m_deckPlays.size()); index < deckCount(); ++index) {
        const ConfigKey play(PlayerManager::groupForDeck(index), QStringLiteral("play"));
        if (!ControlObject::exists(play)) {
            break;
        }
        auto* pPlay = new ControlProxy(play, this);
        pPlay->connectValueChanged(this, &SmartMixAssist::scheduleRefresh);
        m_deckPlays.append(pPlay);
    }
}

bool SmartMixAssist::autoDjOn() const {
    return controlValue(kAutoDjEnabled.group, kAutoDjEnabled.item) > 0.0;
}

SmartMixAssist::Status SmartMixAssist::status() const {
    switch (m_pDirector->state()) {
    case SmartMixDirector::State::Armed:
    case SmartMixDirector::State::Preparing:
        return m_pDirector->isHeld() ? Status::Prepared : Status::Executing;
    case SmartMixDirector::State::Running:
        return Status::Running;
    case SmartMixDirector::State::Idle:
    case SmartMixDirector::State::Completed:
    case SmartMixDirector::State::Cancelled:
    case SmartMixDirector::State::FellBack:
        break;
    }
    return autoDjOn() ? Status::AutoDj : m_base;
}

TrackPointer SmartMixAssist::outgoingTrack() const {
    return m_outgoing.isEmpty() ? TrackPointer() : m_pPlayerInfo->getTrackInfo(m_outgoing);
}

TrackPointer SmartMixAssist::incomingTrack() const {
    return m_incoming.isEmpty() ? TrackPointer() : m_pPlayerInfo->getTrackInfo(m_incoming);
}

QString SmartMixAssist::masterDeck() const {
    if (m_options.masterDeck) {
        return m_options.masterDeck();
    }
    const int deck = m_pPlayerInfo->getCurrentPlayingDeck();
    return deck >= 0 ? PlayerManager::groupForDeck(deck) : QString();
}

bool SmartMixAssist::deckPlays(const QString& group) const {
    return controlValue(group, QStringLiteral("play")) > 0.0;
}

double SmartMixAssist::durationMs(const QString& group) const {
    return controlValue(group, QStringLiteral("duration")) * 1000.0;
}

QString SmartMixAssist::incomingFor(const QString& master) const {
    const int decks = deckCount();
    int masterIndex = -1;
    for (int index = 0; index < decks; ++index) {
        if (PlayerManager::groupForDeck(index) == master) {
            masterIndex = index;
        }
    }
    if (masterIndex < 0) {
        return {};
    }
    // Its partner first: deck 1 with 2, 3 with 4
    QList<int> order{masterIndex ^ 1};
    for (int index = 0; index < decks; ++index) {
        if (index != masterIndex && index != (masterIndex ^ 1)) {
            order.append(index);
        }
    }
    for (const int index : std::as_const(order)) {
        if (index >= decks) {
            continue;
        }
        const QString group = PlayerManager::groupForDeck(index);
        if (m_pPlayerInfo->getTrackInfo(group) && !deckPlays(group)) {
            return group;
        }
    }
    return {};
}

std::optional<cache::TrackKey> SmartMixAssist::crateTrackOn(const QString& group) const {
    const TrackPointer pTrack = m_pPlayerInfo->getTrackInfo(group);
    if (!pTrack || !m_options.trackOfFile) {
        return std::nullopt;
    }
    return m_options.trackOfFile(pTrack->getLocation());
}

PlanContext SmartMixAssist::contextFor(const QString& serverId, const QStringList& decks) const {
    PlanContext context;
    context.plannerVersion = kPlannerVersion;
    // A planned transition, as Auto DJ's: `automatic=false` is a skip the
    // DJ asks for now, which the server plans as a short crossfade (350 ms
    // on the dev stack)
    context.automatic = true;
    context.assist = true;
    const std::optional<Capabilities> capabilities =
            m_options.capabilitiesOf ? m_options.capabilitiesOf(serverId) : std::nullopt;
    const auto has = [&capabilities](DecksFeature feature) {
        return capabilities && capabilities->has(feature);
    };
    context.allowBeatmatch = has(DecksFeature::Beatmatch);
    context.allowTempoAdjustment = true;
    // The handoff cuts the lows with the default EQ's measured knob
    context.allowBassHandoff = has(DecksFeature::BassHandoff) && m_options.defaultEqOn &&
            m_options.defaultEqOn(decks);
    context.allowLongBeatmatch = has(DecksFeature::LongBeatmatch);
    // A DJ mixing by hand has no Auto DJ time: the assistant's own, 8 s by
    // default; long blends follow allowLongBeatmatch and the assist limits
    const int seconds = m_options.pPreferences
            ? m_options.pPreferences->smartMixAssistTransitionSeconds()
            : CratePreferences::kDefaultAssistTransitionSeconds;
    context.preferredDurationMs = seconds * 1000;
    return context;
}

void SmartMixAssist::scheduleRefresh() {
    if (!m_settleTimer.isActive()) {
        m_settleTimer.start();
    }
}

void SmartMixAssist::refresh() {
    watchControls();
    if (m_pDirector->isActive()) {
        // A transition is under way: its decks stay
        emit changed();
        return;
    }
    const auto keep = [this](Status status) {
        m_base = status;
        m_plan.reset();
        m_problem.clear();
        m_edge = PlanEdge();
        m_serverId.clear();
        m_context = QJsonObject();
        ++m_request;
        emit changed();
    };
    if (autoDjOn()) {
        m_outgoing.clear();
        m_incoming.clear();
        keep(Status::AutoDj);
        return;
    }
    const QString master = masterDeck();
    m_outgoing = master;
    m_incoming = master.isEmpty() ? QString() : incomingFor(master);
    if (m_incoming.isEmpty()) {
        keep(Status::NoTransition);
        return;
    }
    const auto outgoing = crateTrackOn(m_outgoing);
    const auto incoming = crateTrackOn(m_incoming);
    if (!outgoing || !incoming || outgoing->serverId != incoming->serverId) {
        keep(Status::NotCrate);
        return;
    }
    PlanClient* pPlans =
            m_options.planClientFor ? m_options.planClientFor(outgoing->serverId) : nullptr;
    if (!pPlans) {
        keep(Status::NoServer);
        return;
    }
    const PlanEdge edge{outgoing->entityUid, incoming->entityUid, QString(), QString()};
    const PlanContext context = contextFor(outgoing->serverId, {m_outgoing, m_incoming});
    const QJsonObject contextJson = context.toJson();
    if (m_plan && m_serverId == outgoing->serverId &&
            m_edge.outgoingTrackEntityUid == edge.outgoingTrackEntityUid &&
            m_edge.incomingTrackEntityUid == edge.incomingTrackEntityUid &&
            m_context == contextJson) {
        // The same edge and context: its plan holds
        emit changed();
        return;
    }
    keep(Status::Requesting);
    m_edge = edge;
    m_serverId = outgoing->serverId;
    m_context = contextJson;
    if (const auto cached = pPlans->cachedPlan(edge, context)) {
        m_latencyMs = 0;
        setPlan(*cached);
        return;
    }
    const quint64 request = m_request;
    auto pClock = std::make_shared<QElapsedTimer>();
    pClock->start();
    kLogger.info() << "Asking for the plan" << m_outgoing << "->" << m_incoming;
    PlanFetch* pFetch = pPlans->requestPlans({edge}, context);
    connect(pFetch,
            &PlanFetch::finished,
            this,
            [this, request, pClock](const PlanResults& results) {
                if (request != m_request) {
                    return;
                }
                m_latencyMs = pClock->elapsed();
                if (results.plans.empty() || !results.plans.front().ok()) {
                    m_base = Status::NoPlan;
                    m_problem = results.plans.empty()
                            ? tr("The server sent no plan.")
                            : results.plans.front().error().message;
                    kLogger.warning() << "No plan:" << m_problem;
                    emit changed();
                    return;
                }
                setPlan(results.plans.front().value());
            });
}

void SmartMixAssist::setPlan(const TransitionPlan& plan) {
    const auto problem = plan.validate(static_cast<qint64>(durationMs(m_outgoing)),
            static_cast<qint64>(durationMs(m_incoming)));
    if (problem) {
        m_base = Status::NoPlan;
        m_problem = *problem;
        kLogger.warning() << "The plan does not fit the tracks:" << *problem;
    } else {
        m_base = Status::Ready;
        m_plan = plan;
        kLogger.info() << "Plan" << toString(plan.mode) << plan.durationMs << "ms, confidence"
                       << plan.confidence;
    }
    emit changed();
}

std::optional<double> SmartMixAssist::outgoingPositionMs() const {
    if (m_outgoing.isEmpty()) {
        return std::nullopt;
    }
    return m_pEnvironment->enginePlayPosition(m_outgoing) * durationMs(m_outgoing);
}

double SmartMixAssist::incomingOwnRate() const {
    if (const auto before = m_pDirector->incomingRateBefore()) {
        return *before;
    }
    return controlValue(m_incoming, QStringLiteral("rate_ratio"), 1.0);
}

std::optional<double> SmartMixAssist::msToEntry() const {
    if (!m_plan || m_outgoing.isEmpty()) {
        return std::nullopt;
    }
    const double totalMs = durationMs(m_outgoing);
    const double positionMs = m_pEnvironment->enginePlayPosition(m_outgoing) * totalMs;
    const double rate = controlValue(m_outgoing, QStringLiteral("rate_ratio"), 1.0);
    return (SmartMixDirector::startAtMs(*m_plan, totalMs) - positionMs) / (rate > 0.0 ? rate : 1.0);
}

bool SmartMixAssist::canPrepare() const {
    const auto entry = msToEntry();
    return status() == Status::Ready && entry && *entry > 0.0;
}

bool SmartMixAssist::canExecute() const {
    return canPrepare() || status() == Status::Prepared;
}

bool SmartMixAssist::canCancel() const {
    return m_pDirector->isActive();
}

SmartMixDirector::Options SmartMixAssist::directorOptions() const {
    SmartMixDirector::Options options;
    // As in Auto DJ: on by default, off leaves the deck's own
    if (!m_options.pPreferences || m_options.pPreferences->smartMixKeylock()) {
        options.keylock = true;
    }
    return options;
}

bool SmartMixAssist::prepare() {
    if (!canPrepare()) {
        return false;
    }
    SmartMixDirector::Options options = directorOptions();
    options.holdStart = true;
    kLogger.info() << "Prepare" << m_outgoing << "->" << m_incoming;
    const bool prepared =
            m_pDirector->arm(m_outgoing, m_incoming, options) && m_pDirector->setPlan(*m_plan);
    emit changed();
    return prepared;
}

bool SmartMixAssist::execute() {
    bool executed = false;
    if (m_pDirector->isHeld()) {
        kLogger.info() << "Execute the prepared transition";
        executed = m_pDirector->release();
    } else if (canPrepare()) {
        kLogger.info() << "Execute" << m_outgoing << "->" << m_incoming;
        executed = m_pDirector->arm(m_outgoing, m_incoming, directorOptions()) &&
                m_pDirector->setPlan(*m_plan);
    } else {
        return false;
    }
    // Released, the director may stay in Preparing
    emit changed();
    return executed;
}

void SmartMixAssist::cancel() {
    if (m_pDirector->isActive()) {
        kLogger.info() << "Cancel";
        m_pDirector->cancel();
    }
}

void SmartMixAssist::onAutoDjChanged(double value) {
    if (value > 0.0 && m_pDirector->isActive()) {
        // Auto DJ's transitions come first
        kLogger.info() << "Auto DJ is on: the assistant's transition is cancelled";
        m_pDirector->cancel();
    }
    if (value > 0.0) {
        // Its plan goes at once
        refresh();
    } else {
        scheduleRefresh();
    }
}

void SmartMixAssist::onDirectorFinished() {
    const SmartMixDirector::Report& report = m_pDirector->report();
    kLogger.info() << "Transition ended:" << report.reason
                   << (report.mixed ? "mixed" : "not mixed") << "late" << report.lateMs << "ms";
    scheduleRefresh();
    emit changed();
}

} // namespace crate
