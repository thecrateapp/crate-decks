#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "crate/cache/audiocache.h"
#include "crate/client/cratecapabilities.h"
#include "crate/smartmix/planclient.h"
#include "crate/smartmix/sethistory.h"
#include "crate/smartmix/smartmixdirector.h"
#include "crate/smartmix/transitionplan.h"
#include "library/autodj/autodjprocessor.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class PlayerInfo;
class PlaylistDAO;

namespace crate {

class CratePreferences;

/// Whether Auto DJ offers Crate Smart Mix: one of these answers of
/// `GET /api/capabilities` announces `beatmatch` in
/// `capabilities.decks.features` (S5).
bool smartMixOffered(const QList<QByteArray>& capabilitiesJsons);
/// The same with the last answers of the saved servers of
/// CrateServers::instance(); false without one.
bool autoDjSmartMixOffered();

/// Plays Auto DJ's Crate Smart Mix transitions (plan F6.C-PR3) with the
/// Smart Mix director and the server's plans: the TransitionDirector of
/// AutoDJProcessor, which it sets while it lives.
///
/// - Ahead of time, whenever the Auto DJ queue or a deck's track changes,
///   and while Auto DJ runs in Crate Smart Mix, it asks for the plans of the
///   next two edges (the track on air to the next one, and that one to the
///   one after), so a transition usually finds its plan cached.
/// - Auto DJ hands a transition over as early as the edge's cached plan needs
///   (transitionLeadSeconds(), G4 finding 1): the plan's start, with the
///   director's preparation, before Auto DJ's own fade, at least 30 s; 30 s
///   without a plan in the cache. Plans that arrive later place the handover
///   again (AutoDJProcessor::transitionLeadChanged()).
/// - When Auto DJ hands a transition over, it arms
///   the director at once, so the director's plan deadline counts from the
///   request, and gives it the cached plan or the one the request brings. An
///   edge that is not of two tracks of the same Crate server, or a director
///   that cannot arm, is left to Auto DJ.
/// - The director's finished() comes through a queued connection: a
///   transition the director did not play (no plan in time, a plan that does
///   not fit, cancelled before the start) is handed back to Auto DJ, which
///   makes its own; then the next plans are asked for.
/// - "Fade now" before the director started the incoming deck cancels the
///   director and Auto DJ fades now itself; the director never starts early
///   (Diego, 2026-11-15). Once it plays, "Fade now" changes nothing.
///
/// Plans are asked of `smart-mix-v3` with its candidate menu when the server
/// offers them, and of `smart-mix-v2` otherwise (PlanContext::forServer(),
/// plan F6b.PR1), for Auto DJ's own transition time within the server's
/// limits, with beatmatch, bass handoff and long blends as the server
/// announces them; bass handoff only when Auto DJ's decks have the default
/// mixing EQ on (eqmap::isDefaultEqOn()), and with the set history. The
/// director plays the server's pick or the next entry of its menu that fits
/// (TransitionPlan::playable()). Keylock follows the Smart Mix preference.
class AutoDJSmartMixAdapter : public QObject, public AutoDJProcessor::TransitionDirector {
    Q_OBJECT
  public:
    /// The most `preferredDurationMs` the server takes (its request schema).
    static constexpr int kMaxPreferredDurationMs = PlanContext::kMaxPreferredDurationMs;
    /// The handover lead's floor, the lead without a plan, and its ceiling
    /// (G4 finding 1); and the margin above what a plan needs.
    static constexpr double kMinLeadSeconds = AutoDJProcessor::kTransitionDirectorLeadSeconds;
    static constexpr double kMaxLeadSeconds = 300.0;
    static constexpr double kLeadMarginSeconds = 2.0;

    /// The lead `plan` needs on tracks of these durations when Auto DJ's own
    /// fade begins at `fadeBeginSecond`: from its start
    /// (SmartMixDirector::startAtMs() of the entry it would play) less the
    /// director's kPrepareLeadMs and kLeadMarginSeconds, within
    /// [kMinLeadSeconds, kMaxLeadSeconds]. kMinLeadSeconds for a plan it
    /// could not play.
    static double leadSecondsFor(const TransitionPlan& plan,
            qint64 outgoingDurationMs,
            qint64 incomingDurationMs,
            double fadeBeginSecond);

    struct Options {
        /// Auto DJ's mode and transition time.
        UserSettingsPointer pConfig;
        /// The decks' tracks. Defaults to PlayerInfo::instance().
        PlayerInfo* pPlayerInfo = nullptr;
        /// Its changes to the Auto DJ queue ask for the next plans.
        PlaylistDAO* pPlaylistDao = nullptr;
        /// The Auto DJ queue is read from it, off the main thread.
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// Keylock during Smart Mix; on without one.
        CratePreferences* pPreferences = nullptr;
        /// The Crate track whose cached file `location` is, if any
        /// (cache::AudioCache::trackOfFile()).
        std::function<std::optional<cache::TrackKey>(const QString& location)> trackOfFile;
        /// The plans of a server; nullptr for none.
        std::function<PlanClient*(const QString& serverId)> planClientFor;
        /// What a server announced last.
        std::function<std::optional<Capabilities>(const QString& serverId)> capabilitiesOf;
        /// The transitions of the set, which it records and sends; none
        /// without it.
        SetHistory* pSetHistory = nullptr;
        /// Whether these decks have the default mixing EQ, on.
        std::function<bool(const QStringList& deckGroups)> defaultEqOn;
        /// The director; the app's one without it.
        std::unique_ptr<SmartMixDirector> pDirector;
        /// Changes closer than this ask for plans once.
        std::chrono::milliseconds settleTime{200};
        /// Sets itself as Auto DJ's TransitionDirector while it lives.
        bool install = true;
    };

    explicit AutoDJSmartMixAdapter(Options options, QObject* pParent = nullptr);
    ~AutoDJSmartMixAdapter() override;

    // AutoDJProcessor::TransitionDirector
    bool startTransition(AutoDJProcessor* pProcessor,
            const QString& fromGroup,
            const QString& toGroup) override;
    bool fadeNow() override;
    void cancelTransition() override;
    double transitionLeadSeconds(AutoDJProcessor* pProcessor,
            const QString& fromGroup,
            const QString& toGroup,
            double fadeBeginSecond) override;

    SmartMixDirector* director() const {
        return m_pDirector.get();
    }

    /// The planner context of the server's edges.
    PlanContext contextFor(const QString& serverId) const;
    /// The director's options for an Auto DJ transition.
    SmartMixDirector::Options directorOptions() const;

    /// Asks for the next plans once the changes settle.
    void scheduleRefresh();
    /// The plans of the server's track changed (its correction, plan
    /// F8.SM), and its PlanClient dropped them: the next edges are asked for
    /// again. A transition the director already took keeps its plan.
    void trackPlansChanged(const QString& serverId, const QString& entityUid);

  signals:
    /// The plans of the next edges were asked for (or nothing to ask).
    void refreshed();

  private:
    void refresh();
    void requestAhead(const QStringList& queue);
    void onDirectorFinished();
    void noteLimits(const QString& serverId, const TransitionPlan& plan);
    bool autoDjInSmartMix() const;
    /// Auto DJ's two decks: the first left one and the first right one.
    QStringList autoDjDecks() const;
    TrackPointer trackOn(const QString& group) const;
    std::optional<cache::TrackKey> crateTrackOn(const QString& group) const;
    /// The cached plan of the edge between the tracks on these decks, if any.
    std::optional<TransitionPlan> cachedPlanFor(
            const QString& fromGroup, const QString& toGroup) const;

    Options m_options;
    PlayerInfo* const m_pPlayerInfo;
    const std::unique_ptr<SmartMixDirector> m_pDirector;
    QPointer<AutoDJProcessor> m_pProcessor;
    /// Auto DJ asked for a lead without a plan in the cache: the next plans
    /// place its handover again.
    bool m_leadPending = false;
    /// Counts the transitions, so the plan of an old one is not given to a
    /// new one.
    quint64 m_transition = 0;
    /// The policy limits the last plans of each server brought.
    QHash<QString, TransitionPlanLimits> m_limits;
    QTimer m_settleTimer;
    QThreadPool m_pool;
    bool m_installed = false;
};

} // namespace crate
