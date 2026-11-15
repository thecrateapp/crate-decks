#pragma once

#include <QPixmap>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>
#include <array>

#include "crate/ui/widgets/instrumentpainter.h"
#include "crate/ui/widgets/transitionplanviewmodel.h"

namespace crate::widgets {

/// The Smart Mix plan's timeline (design 14.3, plan V3.PR1b) with the
/// grammar of StackMap, after the GU1 mockup smart-mix-assistant (decks.js
/// planView): a thin view over a TransitionPlanViewModel.
///
/// The outgoing deck's bar ruler above ("A · <title> · outgoing") and the
/// incoming deck's below, from its entry; the plan's moments as nodes
/// (entry cue filled, plan cues with an accentHi stroke, track cues
/// hollow) with their names, the transition window between dashed axes
/// ("Transition · 32 bars"), the bass swap as a "3 4" connection with an
/// arrow, the entry as a connection with an arrow into the incoming deck,
/// three aspect lanes (the tempo solid, the levels "9 4", the beatmatch
/// "1 5") and the legend of the stroke patterns; then "Now · bar N", the
/// outgoing deck's playhead.
///
/// The library pane is shorter than the mockup's: the lanes need
/// planLaneMinGap between them and the legend comes after them, so a
/// shorter view leaves out the legend, then the lanes (the compact
/// timeline: rulers, moments, window, bass swap, entry and playhead), and
/// below minimumSizeHint() it draws nothing.
///
/// Everything but the playhead is painted into a pixmap when the model or
/// the size change (design 9); when the playhead moves, only its old and
/// new strips are repainted, and only when it moves by a pixel or its bar
/// changes. The caller decides how often it moves (AssistPanel: a few
/// times a second).
class TransitionPlanView : public QWidget {
    Q_OBJECT
  public:
    /// `pModel` must outlive the view.
    explicit TransitionPlanView(TransitionPlanViewModel* pModel,
            const InstrumentStyle& style = InstrumentStyle::installed(),
            QWidget* pParent = nullptr);

    /// The mockup's stage: the lanes and the legend.
    QSize sizeHint() const override;
    /// The compact timeline.
    QSize minimumSizeHint() const override;

    /// What the view shows at its height.
    bool showsTimeline() const {
        return stage().timeline;
    }
    bool showsLanes() const {
        return stage().lanes;
    }
    bool showsLegend() const {
        return stage().legend;
    }

    /// Where `atMs` of the model's axis is, in widget pixels.
    double xOf(double atMs) const;
    /// The rulers' heights.
    double outgoingRulerY() const {
        return stage().outgoingY;
    }
    double incomingRulerY() const {
        return stage().incomingY;
    }
    /// The aspect lanes' heights (tempo, levels, beatmatch).
    const std::array<double, 3>& laneYs() const {
        return stage().lanes3;
    }
    /// What a move of the playhead repaints.
    QRect playheadRect() const;

    /// The cache key of the pixmap with everything but the playhead; it
    /// changes only when that is painted again.
    qint64 staticCacheKey() const {
        return m_static.cacheKey();
    }

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void resizeEvent(QResizeEvent* pEvent) override;

  private:
    struct Layout {
        bool timeline = false;
        bool lanes = false;
        bool legend = false;
        bool legendTitle = false;
        double plotLeft = 0;
        double plotRight = 0;
        double outgoingY = 0;
        double incomingY = 0;
        std::array<double, 3> lanes3{};
        double legendTop = 0;
    };
    struct Playhead {
        int x = 0;
        QString label;
        bool inRange = false;
        QRect rect;
    };

    /// The layout of the current size: a hidden widget is resized without
    /// a resize event
    const Layout& stage() const;
    Layout layoutFor(QSize size) const;
    double legendHeight(double width, bool title) const;
    int compactHeight() const;
    int lanesHeight() const;
    Playhead playhead() const;
    void invalidate();
    void onNowChanged();
    void paintStatic();
    void paintRulers(QPainter* pPainter) const;
    void paintLanes(QPainter* pPainter) const;
    void paintConnections(QPainter* pPainter) const;
    void paintMoments(QPainter* pPainter) const;
    void paintLegend(QPainter* pPainter) const;
    void paintPlayhead(QPainter* pPainter, const Playhead& playhead) const;

    TransitionPlanViewModel* const m_pModel;
    const InstrumentStyle m_style;
    mutable Layout m_layout;
    mutable QSize m_layoutSize;
    QPixmap m_static;
    /// The playhead as painted last
    Playhead m_playhead;
};

} // namespace crate::widgets
