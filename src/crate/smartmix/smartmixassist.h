#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "crate/cache/audiocache.h"
#include "crate/client/cratecapabilities.h"
#include "crate/smartmix/planclient.h"
#include "crate/smartmix/sethistory.h"
#include "crate/smartmix/smartmixdirector.h"
#include "crate/smartmix/transitionplan.h"
#include "track/track_decl.h"

class PlayerInfo;

namespace crate {

class CratePreferences;

/// The Smart Mix assist mode (plan F6.C-PR4, design D8): for a DJ who mixes
/// by hand, the server's plan from the master deck to the other one, which
/// the DJ prepares, executes or cancels from the assistant (AssistPanel) or
/// from a controller. The model under the panel; it has no widgets.
///
/// - The master deck is the one on air (PlayerInfo::getCurrentPlayingDeck()).
///   The other deck is its partner (deck 1 with 2, 3 with 4) if it holds a
///   stopped track, or else the first other deck that does.
/// - Whenever a deck loads a track, starts or stops, or the master deck
///   changes, the plan of that edge is asked of the tracks' Crate server
///   (`smart-mix-v3`, `assist=true`: long blends up to the server's
///   `maximumLongBeatmatchMs`, 96 s; Diego, 2026-11-15), unless a
///   transition is under way. It prefers the transition length of the
///   preferences (CratePreferences::smartMixAssistTransitionSeconds(), 8 s
///   by default) and asks again when it changes.
/// - Prepare arms the director with the plan and holds it: the other deck is
///   cued, its tempo matched and its EQ neutral, and the transition waits.
///   Execute lets it start at the plan's entry; without Prepare, it arms the
///   director and the incoming deck is prepared 8 s before the entry, as in
///   Auto DJ. Cancel stops the transition and puts back what the director
///   moved.
/// - `[CrateSmartMix],prepare`, `execute` and `cancel` are push buttons that
///   controllers can map.
/// - Auto DJ comes first: while it is on, in any mode, the assistant plans
///   nothing and its buttons do nothing, and turning Auto DJ on cancels the
///   assistant's transition. Both would move the same decks and crossfader.
///
/// Its director is its own, not Auto DJ's (AutoDJSmartMixAdapter): the rule
/// above keeps them from playing at the same time.
class SmartMixAssist : public QObject {
    Q_OBJECT
  public:
    enum class Status {
        /// No Crate server plans the decks' tracks.
        NoServer,
        /// A deck must play and another hold a stopped track.
        NoTransition,
        /// The decks' tracks are not of one Crate server.
        NotCrate,
        Requesting,
        /// The server gave no plan that fits the tracks (problem()).
        NoPlan,
        Ready,
        /// Prepared, waiting for Execute.
        Prepared,
        /// Executed: the director waits for the plan's entry.
        Executing,
        Running,
        /// Auto DJ is on: its transitions come first.
        AutoDj,
    };
    Q_ENUM(Status)

    /// The group of its controls.
    static inline const QString kControlGroup = QStringLiteral("[CrateSmartMix]");

    struct Options {
        /// The decks' tracks. Defaults to PlayerInfo::instance().
        PlayerInfo* pPlayerInfo = nullptr;
        /// Keylock during the transition and the preferred transition
        /// length; on and 8 s without one.
        CratePreferences* pPreferences = nullptr;
        /// The Crate track whose cached file `location` is, if any.
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
        /// The master deck's group, or empty; the deck on air by default.
        std::function<QString()> masterDeck;
        /// The decks' positions; the engine's by default.
        std::unique_ptr<SmartMixEnvironment> pEnvironment;
        /// The director; the app's one without it.
        std::unique_ptr<SmartMixDirector> pDirector;
        /// Changes closer than this ask for one plan.
        std::chrono::milliseconds settleTime{150};
        /// Creates the controls controllers map; false for a second
        /// assistant beside the app's (the screenshot harness).
        bool mappable = true;
    };

    explicit SmartMixAssist(Options options, QObject* pParent = nullptr);
    ~SmartMixAssist() override;

    Status status() const;
    /// The master deck and the other one; empty without a transition.
    QString outgoingGroup() const {
        return m_outgoing;
    }
    QString incomingGroup() const {
        return m_incoming;
    }
    TrackPointer outgoingTrack() const;
    TrackPointer incomingTrack() const;
    /// The plan of the decks' edge, once the server gave one that fits: its
    /// pick, or the next entry of its menu that fits the decks' tracks
    /// (TransitionPlan::playable()).
    const std::optional<TransitionPlan>& plan() const {
        return m_plan;
    }
    /// The planner the plan was asked of (PlanContext::forServer()).
    QString plannerVersion() const {
        return m_plannerVersion;
    }
    /// Why there is no plan (Status::NoPlan).
    QString problem() const {
        return m_problem;
    }
    /// How long the plan's request took, in milliseconds; 0 from the cache.
    qint64 planLatencyMs() const {
        return m_latencyMs;
    }
    /// Real time until the incoming deck enters, at the outgoing deck's
    /// tempo; negative once the entry has passed.
    std::optional<double> msToEntry() const;
    /// Where the outgoing deck is in its track, in milliseconds.
    std::optional<double> outgoingPositionMs() const;
    /// The incoming deck's own `rate_ratio`: its rate now, or the one it had
    /// before the director prepared it, which it goes back to after a
    /// beatmatch.
    double incomingOwnRate() const;

    bool canPrepare() const;
    bool canExecute() const;
    bool canCancel() const;

    /// What the buttons and the controls do. False if it cannot now.
    bool prepare();
    bool execute();
    void cancel();

    /// The planner context of the server's edges.
    PlanContext contextFor(const QString& serverId, const QStringList& decks) const;

    SmartMixDirector* director() const {
        return m_pDirector.get();
    }

  public slots:
    /// Reads the decks again and asks for their plan.
    void refresh();
    /// refresh() once the changes settle.
    void scheduleRefresh();
    /// The plans of the server's track changed (its correction, plan
    /// F8.SM), and its PlanClient dropped them: the plan of the decks' edge,
    /// if it has the track, is asked for again. A transition under way
    /// (prepared, executing or running) keeps its plan.
    void trackPlansChanged(const QString& serverId, const QString& entityUid);

  signals:
    /// The status, the decks or the plan changed.
    void changed();

  private:
    void watchControls();
    bool autoDjOn() const;
    QString masterDeck() const;
    QString incomingFor(const QString& master) const;
    std::optional<cache::TrackKey> crateTrackOn(const QString& group) const;
    bool deckPlays(const QString& group) const;
    double durationMs(const QString& group) const;
    void setPlan(const TransitionPlan& plan);
    void onAutoDjChanged(double value);
    void onDirectorFinished();
    SmartMixDirector::Options directorOptions() const;

    Options m_options;
    PlayerInfo* const m_pPlayerInfo;
    const std::unique_ptr<SmartMixEnvironment> m_pEnvironment;
    const std::unique_ptr<SmartMixDirector> m_pDirector;
    std::unique_ptr<ControlPushButton> m_pPrepare;
    std::unique_ptr<ControlPushButton> m_pExecute;
    std::unique_ptr<ControlPushButton> m_pCancel;
    std::unique_ptr<ControlProxy> m_pAutoDjEnabled;
    QList<ControlProxy*> m_deckPlays;
    QTimer m_settleTimer;

    Status m_base = Status::NoTransition;
    QString m_outgoing;
    QString m_incoming;
    PlanEdge m_edge;
    QString m_serverId;
    /// The planner and context the plan was asked with: another one asks
    /// again.
    QString m_plannerVersion;
    QJsonObject m_context;
    std::optional<TransitionPlan> m_plan;
    QString m_problem;
    qint64 m_latencyMs = 0;
    /// Counts the requests, so an old answer is not taken for a new edge.
    quint64 m_request = 0;
};

} // namespace crate
