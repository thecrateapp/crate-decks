#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVector>
#include <optional>

#include "crate/smartmix/transitionplan.h"
#include "track/beats.h"
#include "track/track_decl.h"

namespace crate::widgets {

/// A cue of a deck's track that the plan's timeline can name: the Intro and
/// Outro ranges (AnalysisImporter places them from Crate's mix profile) and
/// the hotcues. Milliseconds of the track's own audio.
struct PlanTrackCue {
    enum class Type {
        Intro,
        Outro,
        HotCue,
    };

    Type type = Type::HotCue;
    std::optional<double> startMs;
    std::optional<double> endMs;
    /// A hotcue's number, from 1, as the deck's buttons show it.
    int number = 0;
};

/// One deck of a Smart Mix transition, as the plan's timeline reads it.
struct PlanDeck {
    /// "[Channel1]"
    QString group;
    QString title;
    double durationMs = 0.0;
    /// The loaded track's beat grid; none until the track is analysed. Bars
    /// count from its first marker, which Decks puts on a downbeat
    /// (AnalysisImporter) and upstream on the first beat.
    mixxx::BeatsPointer pBeats;
    int beatsPerBar = 4;
    /// The track's BPM at rate 1, for the tempo without a grid.
    double bpm = 0.0;
    /// The deck's `rate_ratio`: what the outgoing deck plays at, and the
    /// incoming deck's own, before the director moves it.
    double rate = 1.0;
    QList<PlanTrackCue> cues;

    /// The deck's track: its title (the file's name without one), grid, BPM,
    /// Intro, Outro and hotcues.
    static PlanDeck fromTrack(const QString& group,
            const TrackPointer& pTrack,
            double durationMs,
            double rate);
};

/// The Smart Mix plan's timeline (design 14.3, plan V3.PR1b) without
/// widgets: where every part of a TransitionPlan falls, for
/// TransitionPlanView to draw after the GU1 mockup smart-mix-assistant.
///
/// The axis is the outgoing track's own time, the plan's output time. The
/// outgoing deck's bar ruler runs along it; the incoming deck's starts at
/// the entry, at its own tempo against the outgoing one (the plan's ratio in
/// a beatmatch, then Android's tempo restore). The range is the transition
/// window with half its length before it and a quarter after (at least 8
/// and 4 bars), on bar lines, as the mockup's 16 + 32 + 8 bars.
///
/// - Moments: the incoming deck's entry (the entry cue), the outgoing
///   deck's exit (a plan cue), and the tracks' own cues in range (track
///   cues): "Outro in" (Outro's start), "Intro end" (Intro's end) and
///   "Hotcue N". A track cue on a plan moment joins it ("Entry · intro
///   cue" when the plan cues the incoming track on its Intro).
/// - The window, its length in the outgoing deck's bars, the bass swap (at
///   handoffProgress, over the swap of curves::bassCutDb()), the incoming
///   deck's tempo (the director's rate in a beatmatch, then
///   curves::tempoRestore()), both decks' levels (curves::transitionGain(),
///   what the director plays), and the beatmatched span.
/// - Now: the outgoing deck's position, and its bar.
///
/// Times are where the director plays them: the window starts at
/// SmartMixDirector::startAtMs(). Without a grid a deck has no bars, and
/// labels leave them out.
class TransitionPlanViewModel : public QObject {
    Q_OBJECT
  public:
    enum class Side {
        Outgoing,
        Incoming,
    };
    /// The cues of the legend.
    enum class MomentKind {
        /// Where the incoming deck enters: filled.
        Entry,
        /// A moment of the plan.
        Plan,
        /// A cue of the track.
        Track,
    };
    struct Moment {
        Side side = Side::Outgoing;
        MomentKind kind = MomentKind::Track;
        /// On the axis
        double atMs = 0.0;
        QString label;
        std::optional<int> bar;
    };
    enum class TickKind {
        Beat,
        /// The first beat of a bar
        Bar,
    };
    struct Tick {
        double atMs = 0.0;
        TickKind kind = TickKind::Beat;
        /// The bar of the beat, from 1 at the grid's first marker
        int bar = 0;
    };
    struct Point {
        double atMs = 0.0;
        double value = 0.0;
    };
    struct BassSwap {
        /// Where the basses cross, and where the swap starts and ends
        double atMs = 0.0;
        double startMs = 0.0;
        double endMs = 0.0;
        std::optional<int> bar;
    };
    /// The incoming deck's tempo, in BPM.
    struct Tempo {
        QVector<Point> points;
        double fromBpm = 0.0;
        double toBpm = 0.0;
        bool changes() const;
    };
    /// Both decks' levels, from 0 to 1, over the transition.
    struct Levels {
        QVector<Point> outgoing;
        QVector<Point> incoming;
    };

    /// How many points a curve has over the transition
    static constexpr int kCurvePoints = 64;

    explicit TransitionPlanViewModel(QObject* pParent = nullptr);

    /// Shows `plan` from `outgoing` to `incoming`.
    void setPlan(const TransitionPlan& plan, const PlanDeck& outgoing, const PlanDeck& incoming);
    void clear();
    /// The outgoing deck's position, in its track's milliseconds.
    void setOutgoingPositionMs(double positionMs);

    bool hasPlan() const {
        return m_plan.has_value();
    }
    const std::optional<TransitionPlan>& plan() const {
        return m_plan;
    }
    const PlanDeck& deck(Side side) const {
        return side == Side::Outgoing ? m_outgoing : m_incoming;
    }
    /// "A" for [Channel1]
    QString deckLetter(Side side) const;
    QString roleLabel(Side side) const;

    double rangeStartMs() const {
        return m_rangeStartMs;
    }
    double rangeEndMs() const {
        return m_rangeEndMs;
    }
    /// The transition: the incoming deck enters at its start
    double windowStartMs() const {
        return m_windowStartMs;
    }
    double windowEndMs() const {
        return m_windowEndMs;
    }
    /// The window's length in the outgoing deck's bars
    std::optional<int> windowBars() const {
        return m_windowBars;
    }
    /// "Transition · 32 bars"
    QString windowLabel() const;
    /// Where each deck's track plays on the axis: the outgoing one to its
    /// end, the incoming one from the entry to its end.
    double trackStartMs(Side side) const;
    double trackEndMs(Side side) const;

    const QVector<Tick>& ticks(Side side) const {
        return side == Side::Outgoing ? m_outgoingTicks : m_incomingTicks;
    }
    const QVector<Moment>& moments() const {
        return m_moments;
    }
    const std::optional<BassSwap>& bassSwap() const {
        return m_bassSwap;
    }
    /// "Bass swap · bar 65"
    QString bassSwapLabel() const;
    const std::optional<Tempo>& tempo() const {
        return m_tempo;
    }
    const std::optional<Levels>& levels() const {
        return m_levels;
    }
    /// "B −1.5 dB": a deck's staging gain; empty at 0 dB (Smart Mix only
    /// lowers a track).
    QString gainLabel(Side side) const;
    /// The beatmatched span, from the entry to the exit.
    std::optional<std::pair<double, double>> beatmatch() const;
    /// The server played it safe (TransitionPlan::fallbackReason).
    bool fellBack() const;

    double nowMs() const {
        return m_nowMs;
    }
    std::optional<int> nowBar() const;
    /// "Now · bar 45"
    QString nowLabel() const;

    /// The bar of `side`'s track at `atMs` of the axis.
    std::optional<int> barAt(Side side, double atMs) const;
    /// Where the incoming track is at `atMs` of the axis, in its own time.
    double incomingPositionAt(double atMs) const;
    /// Where on the axis the incoming track reaches `incomingMs`.
    double atMsOfIncoming(double incomingMs) const;

  signals:
    /// Everything but the playhead changed.
    void changed();
    void nowChanged();

  private:
    void layOut();
    void placeTicks(Side side);
    void placeMoments();
    void placeCurves();
    double outgoingRate() const;
    double incomingRunRate() const;
    double beatMs(Side side, double trackMs) const;

    std::optional<TransitionPlan> m_plan;
    PlanDeck m_outgoing;
    PlanDeck m_incoming;
    double m_rangeStartMs = 0.0;
    double m_rangeEndMs = 0.0;
    double m_windowStartMs = 0.0;
    double m_windowEndMs = 0.0;
    double m_entryMs = 0.0;
    std::optional<int> m_windowBars;
    QVector<Tick> m_outgoingTicks;
    QVector<Tick> m_incomingTicks;
    QVector<Moment> m_moments;
    std::optional<BassSwap> m_bassSwap;
    std::optional<Tempo> m_tempo;
    std::optional<Levels> m_levels;
    double m_nowMs = 0.0;
};

/// Where `trackMs` is in `pBeats`, in beats from its first marker; nothing
/// without a grid. Exposed for the tests.
std::optional<double> beatIndexAt(const mixxx::BeatsPointer& pBeats, double trackMs);

} // namespace crate::widgets
