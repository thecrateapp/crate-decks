#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>
#include <optional>

#include "crate/smartmix/transitionplan.h"
#include "preferences/configobject.h"

class ControlProxy;
class PlayerInfo;

namespace crate {

class ControlAutomation;

/// What the Smart Mix director reads that is not a control: the time and the
/// decks' engine positions. The engine one is the app's; tests give a fake.
class SmartMixEnvironment {
  public:
    virtual ~SmartMixEnvironment() = default;

    /// A monotonic clock, in milliseconds.
    virtual double nowMs() const = 0;
    /// Where the deck was at the last audio callback, from 0 to 1 of the
    /// track: `VisualPlayPosition::getEnginePlayPos()`, not `playposition`,
    /// which the engine only updates about 15 times a second (spike review).
    virtual double enginePlayPosition(const QString& group) const = 0;
    /// Something that changes when the deck loads another track, and is
    /// empty without one.
    virtual QString trackKey(const QString& group) const = 0;

    /// The app's: a steady clock, VisualPlayPosition and the decks' tracks
    /// of `playerInfo`, which must outlive it.
    static std::unique_ptr<SmartMixEnvironment> engine(PlayerInfo& playerInfo);
};

/// Options of SmartMixDirector::arm().
struct SmartMixDirectorOptions {
    /// Keylock on the incoming deck while it plays at another tempo;
    /// empty leaves the deck's setting alone (the DJ's preference).
    std::optional<bool> keylock;
    /// Prepares the incoming deck as soon as the plan comes, and holds
    /// it there: the start waits for SmartMixDirector::release(). The assist mode's
    /// "Prepare" (plan F6.C-PR4). A held transition still ends at its
    /// deadline if its cue passes.
    bool holdStart = false;
};

/// Plays one Smart Mix transition (design D8) between two decks, as the
/// server planned it (TransitionPlan), with the deck and mixer controls and
/// the curves of the Android player (curves.h). It runs on the main thread,
/// from a 60 Hz Qt::PreciseTimer, and writes each control at most once per
/// tick, through a ControlAutomation that stops at the first manual change.
///
/// States, in order: Idle → Armed → Preparing → Running → Completed, or
/// Cancelled or FellBack from any of the middle three.
///
/// - Armed: the director watches the outgoing deck and waits for the plan
///   (setPlan()); without one after kPlanDeadlineMs it falls back
///   (`missing_plan`) and leaves the transition to its caller.
/// - Preparing, from kPrepareLeadMs before the start, or as soon as a late
///   plan arrives: the incoming deck waits at the plan's start
///   (TransitionPlan::startIncomingCueMs()), with quantize and sync lock off
///   until the end, a neutral EQ and pregain, keylock as the caller says, and
///   in beatmatch the plan's tempo ratio times the outgoing deck's. The
///   crossfader goes to the outgoing deck's end; if it was elsewhere, the
///   outgoing deck's pregain keeps the level it had.
/// - Running: the incoming deck starts when the outgoing one reaches the
///   plan's cue, on the outgoing deck's beat clock when the cue is on its
///   next beat; a late start jumps the incoming deck to where it would be.
///   The transition follows the wall clock; the crossfader, the pregains and
///   the bass EQs follow the plan's curve, gains and bass handoff.
/// - Completed: the outgoing deck is stopped and the mixer is left on the
///   incoming one, which then eases back to its own tempo (Android's tempo
///   restore). A plan that already was the server's fallback (it has a
///   fallbackReason) is played the same way and ends in FellBack.
/// - Cancelled: a deck stopped or loaded another track, the outgoing deck
///   looped or jumped back, the cue never came, or cancel() was called. The
///   director puts the EQs and pregains it moved back and leaves the rest.
class SmartMixDirector : public QObject {
    Q_OBJECT
  public:
    enum class State {
        Idle,
        Armed,
        Preparing,
        Running,
        Completed,
        Cancelled,
        FellBack,
    };
    Q_ENUM(State)

    using Options = SmartMixDirectorOptions;

    /// What happened, for the caller and for the G4 metrics.
    struct Report {
        /// Why the transition ended: `completed`, the plan's fallback reason,
        /// `missing_plan`, `invalid_plan`, `insufficient_window`,
        /// `outgoing_stopped`, `incoming_stopped`, `incoming_started`,
        /// `track_changed`, `outgoing_jumped_back`, `deadline` or `cancelled`.
        QString reason;
        /// The validation problem of an invalid plan.
        QString detail;
        /// The incoming deck was started: the director played a transition.
        bool mixed = false;
        bool usedBeatClock = false;
        /// Where the outgoing deck was planned to be, and was, when the
        /// incoming deck started (outgoing track time).
        double plannedStartOutgoingMs = 0.0;
        double startOutgoingMs = 0.0;
        /// Where the incoming deck started (incoming track time).
        double incomingStartMs = 0.0;
        /// How late the start was (real time) and the transition's duration
        /// after shortening it to the outgoing track (outgoing track time).
        double lateMs = 0.0;
        double durationMs = 0.0;
        QList<ConfigKey> overridden;
    };

    /// The tick of the update timer: 60 Hz.
    static constexpr int kTickMs = 16;
    /// How long the director waits for the plan after arm().
    static constexpr double kPlanDeadlineMs = 3000.0;
    /// How long before the start the incoming deck is prepared, at least.
    static constexpr double kPrepareLeadMs = 8000.0;
    /// As Android: the transition ends this long before the outgoing track
    /// does, and is not played if less than kMinimumTransitionMs is left
    /// (`NativeMixTrigger`).
    static constexpr double kStartSafetyLeadMs = 50.0;
    static constexpr double kMinimumTransitionMs = 1000.0;
    /// The start goes by the beat clock when the outgoing deck's next beat is
    /// this close to the cue.
    static constexpr double kBeatSnapToleranceMs = 15.0;
    /// A start later than this jumps the incoming deck ahead.
    static constexpr double kLateStartToleranceMs = 5.0;
    /// The outgoing deck going back more than this is a loop or a seek.
    static constexpr double kBackwardJumpToleranceMs = 250.0;
    /// The cue may come this much later than the outgoing deck's position
    /// predicted when the plan arrived.
    static constexpr double kCueDeadlineMarginMs = 5000.0;

    /// The app's director: engine positions, a steady clock, its timers and
    /// the decks' tracks of `playerInfo`, which must outlive it.
    explicit SmartMixDirector(PlayerInfo& playerInfo, QObject* pParent = nullptr);
    /// A director on another environment; it starts no timers, so the caller
    /// calls tick() (tests).
    explicit SmartMixDirector(
            std::unique_ptr<SmartMixEnvironment> pEnvironment, QObject* pParent = nullptr);
    ~SmartMixDirector() override;

    /// Starts watching a transition from the outgoing deck, which must be
    /// playing, to the incoming one; both must hold a track. Returns false,
    /// without touching anything, if a transition is already under way or
    /// the decks cannot take one.
    bool arm(const QString& outgoingGroup,
            const QString& incomingGroup,
            const Options& options = Options());
    /// Gives the armed transition its plan. A plan that does not fit the
    /// decks' tracks (TransitionPlan::validate()) falls back; one that comes
    /// later than kPrepareLeadMs before its start is prepared at once.
    /// Returns whether the plan was taken.
    bool setPlan(const TransitionPlan& plan);
    /// Lets a transition armed with Options::holdStart start at its cue.
    /// Returns false, and does nothing, if no transition is held.
    bool release();
    /// Stops the transition, and the tempo restore after one.
    void cancel();

    /// Where the outgoing deck starts `plan` on a track of
    /// `outgoingDurationMs`: its cue, or earlier so that the transition ends
    /// kStartSafetyLeadMs before the track does (as Android's
    /// NativeMixTrigger.effectiveCueMs()).
    static double startAtMs(const TransitionPlan& plan, double outgoingDurationMs);

    State state() const {
        return m_state;
    }
    /// Armed, Preparing or Running.
    bool isActive() const;
    /// Armed or Preparing with Options::holdStart, not released yet.
    bool isHeld() const {
        return isActive() && m_held;
    }
    /// The incoming deck is still easing back to its tempo after a
    /// completed transition.
    bool isRestoringTempo() const;
    /// The incoming deck's `rate_ratio` before the director prepared it,
    /// which it eases back to after a beatmatch; none before Preparing.
    std::optional<double> incomingRateBefore() const;
    const Report& report() const {
        return m_report;
    }

  public slots:
    /// One update. The timers call it; a director without them is driven by
    /// its caller.
    void tick();

  signals:
    void stateChanged(crate::SmartMixDirector::State state);
    /// The transition ended in Completed, Cancelled or FellBack.
    void finished(crate::SmartMixDirector::State state, const QString& reason);
    /// The DJ (or something else) moved a control the director was moving.
    void controlOverridden(const ConfigKey& key);

  private:
    struct Deck;
    struct Mixer;
    struct TempoRestore;

    std::unique_ptr<Deck> connectDeck(const QString& group);
    void setState(State state);
    double positionMs(const Deck& deck) const;
    double durationMs(const Deck& deck) const;
    double outgoingRate() const;
    std::optional<QString> interruption();
    void stepWaiting(double nowMs);
    void prepare();
    void startIncoming(double nowMs);
    void stepRunning(double nowMs);
    void applyMix(double progress);
    void complete();
    void finish(State state, const QString& reason);
    void restoreDecks();
    void commit();
    void stepTempoRestore(double nowMs);
    void updateTimer();
    void watchOverrides(ControlAutomation* pAutomation);

    const std::unique_ptr<SmartMixEnvironment> m_pEnvironment;
    const bool m_ownTimers;
    QTimer m_timer;
    QTimer m_startTimer;
    State m_state = State::Idle;
    Options m_options;
    Report m_report;
    std::optional<TransitionPlan> m_plan;
    std::unique_ptr<Deck> m_pOutgoing;
    std::unique_ptr<Deck> m_pIncoming;
    std::unique_ptr<Mixer> m_pMixer;
    std::unique_ptr<TempoRestore> m_pTempoRestore;
    double m_armedAtMs = 0.0;
    double m_previousOutgoingMs = 0.0;
    double m_startAtMs = 0.0;
    double m_cueDeadlineMs = 0.0;
    std::optional<double> m_startDueMs;
    double m_runStartMs = 0.0;
    double m_runRate = 1.0;
    bool m_prepared = false;
    bool m_held = false;
    /// Diagnostics: the last running progress and the tenth of it last logged
    double m_lastProgress = -1.0;
    int m_loggedStep = -1;
};

} // namespace crate
