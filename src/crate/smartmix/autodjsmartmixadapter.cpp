#include "crate/smartmix/autodjsmartmixadapter.h"

#include <QFutureWatcher>
#include <QJsonDocument>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "crate/cratepreferences.h"
#include "crate/crateservers.h"
#include "crate/library/crateprefetcher.h"
#include "library/dao/playlistdao.h"
#include "mixer/playerinfo.h"
#include "moc_autodjsmartmixadapter.cpp"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::AutoDJSmartMixAdapter");

// Auto DJ's settings and controls (autodjprocessor.cpp)
const QString kAutoDjPreferenceGroup = QStringLiteral("[Auto DJ]");
const QString kAutoDjControlGroup = QStringLiteral("[AutoDJ]");
constexpr int kAutoDjDefaultTransitionSeconds = 10;
// EngineChannel::ChannelOrientation, as [ChannelN],orientation holds it
constexpr int kOrientationLeft = 0;
constexpr int kOrientationRight = 2;
// The decks Auto DJ may use
constexpr int kMaxDecks = 8;
// The track on air and the next two in the queue: two edges
constexpr int kQueuePlaces = 2;

QString deckGroup(int index) {
    return QStringLiteral("[Channel%1]").arg(index);
}

std::optional<double> deckControl(const QString& group, const QString& item) {
    const ControlObject* pControl = ControlObject::getControl(
            ConfigKey(group, item), ControlFlag::NoWarnIfMissing);
    if (!pControl) {
        return std::nullopt;
    }
    return pControl->get();
}

} // namespace

bool smartMixOffered(const QList<QByteArray>& capabilitiesJsons) {
    return std::any_of(capabilitiesJsons.cbegin(),
            capabilitiesJsons.cend(),
            [](const QByteArray& json) {
                const Result<Capabilities> capabilities =
                        Capabilities::fromJson(QJsonDocument::fromJson(json));
                return capabilities.ok() && capabilities.value().has(DecksFeature::Beatmatch);
            });
}

bool autoDjSmartMixOffered() {
    const CrateServers* pServers = CrateServers::instance();
    if (!pServers) {
        return false;
    }
    QList<QByteArray> answers;
    const QList<Session*> sessions = pServers->sessions();
    for (const Session* pSession : sessions) {
        answers.append(pServers->lastCapabilities(pSession->profile().id));
    }
    return smartMixOffered(answers);
}

AutoDJSmartMixAdapter::AutoDJSmartMixAdapter(Options options, QObject* pParent)
        : QObject(pParent),
          m_options(std::move(options)),
          m_pPlayerInfo(m_options.pPlayerInfo ? m_options.pPlayerInfo : &PlayerInfo::instance()),
          m_pDirector(m_options.pDirector
                          ? std::move(m_options.pDirector)
                          : std::make_unique<SmartMixDirector>(*m_pPlayerInfo)) {
    m_pool.setMaxThreadCount(1);
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(m_options.settleTime);
    connect(&m_settleTimer, &QTimer::timeout, this, &AutoDJSmartMixAdapter::refresh);
    // Queued: the director emits it from inside its own tick or cancel().
    connect(m_pDirector.get(),
            &SmartMixDirector::finished,
            this,
            &AutoDJSmartMixAdapter::onDirectorFinished,
            Qt::QueuedConnection);
    if (PlaylistDAO* pDao = m_options.pPlaylistDao) {
        connect(pDao, &PlaylistDAO::tracksAdded, this, &AutoDJSmartMixAdapter::scheduleRefresh);
        connect(pDao, &PlaylistDAO::tracksMoved, this, &AutoDJSmartMixAdapter::scheduleRefresh);
        connect(pDao, &PlaylistDAO::tracksRemoved, this, &AutoDJSmartMixAdapter::scheduleRefresh);
        connect(pDao,
                &PlaylistDAO::playlistContentChanged,
                this,
                &AutoDJSmartMixAdapter::scheduleRefresh);
    }
    connect(m_pPlayerInfo,
            &PlayerInfo::trackChanged,
            this,
            &AutoDJSmartMixAdapter::scheduleRefresh);
    if (m_options.install) {
        m_installed = true;
        AutoDJProcessor::setTransitionDirector(this);
    }
}

AutoDJSmartMixAdapter::~AutoDJSmartMixAdapter() {
    if (m_installed) {
        AutoDJProcessor::setTransitionDirector(nullptr);
    }
    m_pDirector->disconnect(this);
    m_pDirector->cancel();
    m_pool.waitForDone();
}

bool AutoDJSmartMixAdapter::autoDjInSmartMix() const {
    const auto enabled = deckControl(kAutoDjControlGroup, QStringLiteral("enabled"));
    return enabled.value_or(0.0) > 0.0 && m_options.pConfig &&
            m_options.pConfig->getValue(
                    ConfigKey(kAutoDjPreferenceGroup, QStringLiteral("TransitionMode")),
                    static_cast<int>(AutoDJProcessor::TransitionMode::FullIntroOutro)) ==
            static_cast<int>(AutoDJProcessor::TransitionMode::CrateSmartMix);
}

QStringList AutoDJSmartMixAdapter::autoDjDecks() const {
    QString left;
    QString right;
    for (int index = 1; index <= kMaxDecks; ++index) {
        const QString group = deckGroup(index);
        const auto orientation = deckControl(group, QStringLiteral("orientation"));
        if (!orientation) {
            break;
        }
        const int side = static_cast<int>(*orientation);
        if (left.isEmpty() && side == kOrientationLeft) {
            left = group;
        } else if (right.isEmpty() && side == kOrientationRight) {
            right = group;
        }
    }
    if (left.isEmpty() || right.isEmpty()) {
        return {};
    }
    return {left, right};
}

TrackPointer AutoDJSmartMixAdapter::trackOn(const QString& group) const {
    return m_pPlayerInfo->getTrackInfo(group);
}

std::optional<cache::TrackKey> AutoDJSmartMixAdapter::crateTrackOn(const QString& group) const {
    const TrackPointer pTrack = trackOn(group);
    if (!pTrack || !m_options.trackOfFile) {
        return std::nullopt;
    }
    return m_options.trackOfFile(pTrack->getLocation());
}

PlanContext AutoDJSmartMixAdapter::contextFor(const QString& serverId) const {
    PlanContext context;
    context.plannerVersion = kPlannerVersion;
    context.automatic = true;
    const std::optional<Capabilities> capabilities =
            m_options.capabilitiesOf ? m_options.capabilitiesOf(serverId) : std::nullopt;
    const auto has = [&capabilities](DecksFeature feature) {
        return capabilities && capabilities->has(feature);
    };
    context.allowBeatmatch = has(DecksFeature::Beatmatch);
    context.allowTempoAdjustment = true;
    // The handoff cuts the lows with the default EQ's measured knob
    const QStringList decks = autoDjDecks();
    const bool defaultEq = m_options.defaultEqOn && m_options.defaultEqOn(decks);
    context.allowBassHandoff = has(DecksFeature::BassHandoff) && defaultEq;
    kLogger.info() << "Plan context: beatmatch" << has(DecksFeature::Beatmatch)
                   << "- bass handoff" << context.allowBassHandoff << "(server"
                   << has(DecksFeature::BassHandoff) << ", default EQ on" << decks << defaultEq
                   << ") - long beatmatch" << has(DecksFeature::LongBeatmatch);
    context.allowLongBeatmatch = has(DecksFeature::LongBeatmatch);
    // Auto DJ's own transition time; 0 (the server's own) for the pause of
    // a negative one
    const int seconds = m_options.pConfig
            ? m_options.pConfig->getValue(
                      ConfigKey(kAutoDjPreferenceGroup, QStringLiteral("Transition")),
                      kAutoDjDefaultTransitionSeconds)
            : kAutoDjDefaultTransitionSeconds;
    const TransitionPlanLimits limits = m_limits.value(serverId);
    const qint64 maxMs = std::min<qint64>(limits.maxTransitionMs, kMaxPreferredDurationMs);
    context.preferredDurationMs =
            static_cast<int>(std::clamp<qint64>(qint64{std::max(seconds, 0)} * 1000, 0, maxMs));
    return context;
}

SmartMixDirector::Options AutoDJSmartMixAdapter::directorOptions() const {
    SmartMixDirector::Options options;
    // On by default (Diego, 2026-11-15); off leaves the deck's own
    if (!m_options.pPreferences || m_options.pPreferences->smartMixKeylock()) {
        options.keylock = true;
    }
    return options;
}

bool AutoDJSmartMixAdapter::startTransition(AutoDJProcessor* pProcessor,
        const QString& fromGroup,
        const QString& toGroup) {
    m_pProcessor = pProcessor;
    ++m_transition;
    const auto outgoing = crateTrackOn(fromGroup);
    const auto incoming = crateTrackOn(toGroup);
    if (!outgoing || !incoming || outgoing->serverId != incoming->serverId) {
        kLogger.info() << "Not two tracks of one Crate server:" << fromGroup << "->" << toGroup
                       << "is Auto DJ's own";
        return false;
    }
    const QString serverId = outgoing->serverId;
    PlanClient* pPlans = m_options.planClientFor ? m_options.planClientFor(serverId) : nullptr;
    if (!pPlans) {
        kLogger.info() << "No plans from" << serverId << "- the transition is Auto DJ's own";
        return false;
    }
    // Armed before the plan is asked for: its deadline counts from here.
    if (!m_pDirector->arm(fromGroup, toGroup, directorOptions())) {
        kLogger.info() << "The director cannot take" << fromGroup << "->" << toGroup;
        return false;
    }
    const PlanEdge edge{outgoing->entityUid, incoming->entityUid, QString(), QString()};
    const PlanContext context = contextFor(serverId);
    if (const auto plan = pPlans->cachedPlan(edge, context)) {
        noteLimits(serverId, *plan);
        m_pDirector->setPlan(*plan);
        return true;
    }
    const quint64 transition = m_transition;
    PlanFetch* pFetch = pPlans->requestPlans({edge}, context);
    connect(pFetch,
            &PlanFetch::finished,
            this,
            [this, transition, serverId](const PlanResults& results) {
                if (transition != m_transition ||
                        m_pDirector->state() != SmartMixDirector::State::Armed) {
                    return;
                }
                if (results.plans.empty() || !results.plans.front().ok()) {
                    // The director falls back at its deadline
                    kLogger.warning() << "No plan for the transition";
                    return;
                }
                const TransitionPlan& plan = results.plans.front().value();
                noteLimits(serverId, plan);
                m_pDirector->setPlan(plan);
            });
    return true;
}

bool AutoDJSmartMixAdapter::fadeNow() {
    if (m_pDirector->state() == SmartMixDirector::State::Running ||
            (!m_pDirector->isActive() && m_pDirector->report().mixed)) {
        // It plays already: "Fade now" changes nothing
        return true;
    }
    // Never started early (Diego, 2026-11-15): Auto DJ fades now itself
    ++m_transition;
    m_pDirector->cancel();
    return false;
}

void AutoDJSmartMixAdapter::cancelTransition() {
    ++m_transition;
    m_pDirector->cancel();
}

void AutoDJSmartMixAdapter::onDirectorFinished() {
    if (m_pDirector->isActive()) {
        // Another transition began meanwhile
        return;
    }
    const SmartMixDirector::Report& report = m_pDirector->report();
    kLogger.info() << "Transition ended:" << report.reason << (report.mixed ? "mixed" : "not mixed")
                   << "late" << report.lateMs << "ms";
    if (!report.mixed && m_pProcessor) {
        // Not played: Auto DJ makes its own
        m_pProcessor->handBackTransition();
    }
    scheduleRefresh();
}

void AutoDJSmartMixAdapter::noteLimits(const QString& serverId, const TransitionPlan& plan) {
    if (plan.policyLimits) {
        m_limits.insert(serverId, *plan.policyLimits);
    }
}

void AutoDJSmartMixAdapter::scheduleRefresh() {
    if (!m_settleTimer.isActive()) {
        m_settleTimer.start();
    }
}

void AutoDJSmartMixAdapter::refresh() {
    if (!autoDjInSmartMix() || !m_options.pDbConnectionPool) {
        emit refreshed();
        return;
    }
    auto* pWatcher = new QFutureWatcher<QStringList>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher] {
        pWatcher->deleteLater();
        requestAhead(pWatcher->result());
    });
    pWatcher->setFuture(
            QtConcurrent::run(&m_pool, [pPool = m_options.pDbConnectionPool]() {
                const mixxx::DbConnectionPooler pooler(pPool);
                return library::readAutoDjQueue(mixxx::DbConnectionPooled(pPool), kQueuePlaces);
            }));
}

void AutoDJSmartMixAdapter::requestAhead(const QStringList& queue) {
    // The track on air, then the queue; the deck that waits holds the first
    // queued track
    QStringList tracks;
    QString onAir;
    int playing = 0;
    const QStringList decks = autoDjDecks();
    for (const QString& group : decks) {
        const TrackPointer pTrack = trackOn(group);
        if (pTrack && deckControl(group, QStringLiteral("play")).value_or(0.0) > 0.0) {
            ++playing;
            onAir = pTrack->getLocation();
        }
    }
    if (playing == 1) {
        tracks.append(onAir);
    }
    for (const QString& location : queue) {
        if (tracks.isEmpty() || tracks.last() != location) {
            tracks.append(location);
        }
    }
    QHash<QString, QList<PlanEdge>> edges;
    for (int i = 0; i + 1 < tracks.size() && i < 2; ++i) {
        const auto outgoing = m_options.trackOfFile ? m_options.trackOfFile(tracks[i])
                                                    : std::nullopt;
        const auto incoming = m_options.trackOfFile ? m_options.trackOfFile(tracks[i + 1])
                                                    : std::nullopt;
        if (outgoing && incoming && outgoing->serverId == incoming->serverId) {
            edges[outgoing->serverId].append(
                    PlanEdge{outgoing->entityUid, incoming->entityUid, QString(), QString()});
        }
    }
    for (auto it = edges.cbegin(); it != edges.cend(); ++it) {
        PlanClient* pPlans =
                m_options.planClientFor ? m_options.planClientFor(it.key()) : nullptr;
        if (!pPlans) {
            continue;
        }
        const QString serverId = it.key();
        PlanFetch* pFetch = pPlans->requestPlans(it.value(), contextFor(serverId));
        connect(pFetch, &PlanFetch::finished, this, [this, serverId](const PlanResults& results) {
            for (const Result<TransitionPlan>& plan : results.plans) {
                if (plan.ok()) {
                    noteLimits(serverId, plan.value());
                }
            }
        });
    }
    emit refreshed();
}

} // namespace crate
