#pragma once

#include <QColor>
#include <QFont>
#include <QLineF>
#include <QPen>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

#include "crate/ui/skin/crateskin.h"

class QPainter;

namespace crate::widgets {

/// The look of the instrument widgets of crate/ui/widgets/ (design 14.1 and
/// 14.5, plan V3.PR1): the grammar of the admin's GenreWheel and StackMap.
/// Every value comes from the CrateDecks skin's generated/vars.xml, that is,
/// from res/crate-design/tokens.json (CrateInst<Token> and the skin's
/// colours): the widgets write no colour or size of their own.
struct InstrumentStyle {
    // Colours
    QColor background;
    QColor textPrimary;
    QColor textMuted;
    QColor textSubtle;
    QColor textSecondary;
    QColor textFaint;
    QColor border;
    QColor borderSubtle;
    QColor accent;
    QColor accentHi;
    /// Decks A and C, and B and D
    QColor deckA;
    QColor deckB;

    // Type
    QString fontMono;
    QString fontDisplay;
    double fontSizeLabel = 0;      // MonoLabel
    double fontSizeMeta = 0;       // the subtitle under a display figure
    double fontSizeWheelLabel = 0; // node labels
    double fontSizeDisplay = 0;    // the figure in the centre
    int fontWeightDisplay = 0;
    double trackingCaps = 0; // em
    double trackingWheelLabel = 0;
    double trackingWheelFigure = 0;
    double separatorPadding = 0; // around the " · " of a MonoLabel

    // Strokes
    double strokeHairline = 0;
    double strokeNode = 0;
    double strokeActive = 0;
    double strokeNodeActive = 0;
    double strokeSelected = 0;
    double strokeConnection = 0;
    // Nodes
    double nodeSibling = 0;
    double nodeRoot = 0;
    double nodeSelected = 0;
    double glowSelectedRadius = 0;
    double glowSelectedAlpha = 0;
    double glowHoverRadius = 0;
    double glowHoverAlpha = 0;
    double glowPulseRadius = 0;
    double glowPulseAlpha = 0;
    // Scales
    int tickCount = 0;
    int tickEvery = 0;
    double tickLong = 0;
    double tickShort = 0;
    double tickPhrase = 0;
    // Stroke patterns, in pixels
    QVector<qreal> dashData;
    QVector<qreal> dashQueue;
    QVector<qreal> dashTelemetry;
    QVector<qreal> dashAxis;
    QVector<qreal> dashSpoke;
    // Opacities
    double opacityContext = 0;
    double opacityRest = 0;
    double opacityGuide = 0;
    // Arrows: SVG markers in units of the stroke width
    double arrowMarkerSize = 0;
    double arrowMarkerBox = 0;
    double arrowMarkerTip = 0;
    double arrowMarkerBack = 0;
    double arrowMarkerSpread = 0;

    // Camelot wheel, on GenreWheel's canvas of wheelCanvas pixels
    double wheelCanvas = 0;
    double camelotRingA = 0;
    double camelotRingB = 0;
    int camelotWheelSize = 0;
    double camelotLabelGap = 0;
    double camelotNameGap = 0;
    double camelotLabelShift = 0; // em
    double camelotEdgeBow = 0;
    double camelotFigureBaseline = 0;
    double camelotSubtitleBaseline = 0;

    // Level ring (stack-map.css .stack-map__level)
    double levelRingRadius = 0;
    double levelRingWidth = 0;
    double levelRingTrackWidth = 0;
    double levelRingMinimum = 0;
    double levelRingInset = 0;

    /// The Smart Mix plan's timeline (GU1 mockup smart-mix-assistant,
    /// decks.js planView; CrateInstPlan<Token>)
    struct Plan {
        // The stage
        double gutter = 0;
        double paddingRight = 0;
        double rulerInset = 0;
        double laneInset = 0;
        double laneMinGap = 0;
        // Heights of the window, the labels and the curves, from the rulers
        // and lanes
        double windowRise = 0;
        double windowDrop = 0;
        double windowLabelRise = 0;
        double cueLabelRise = 0;
        double cueLabelDrop = 0;
        double barLabelRise = 0;
        double barLabelDrop = 0;
        double barLabelGap = 0;
        double deckLabelRise = 0;
        double deckRoleDrop = 0;
        double laneLabelDrop = 0;
        double tempoSwing = 0;
        double gainSwing = 0;
        double valueGap = 0;
        double tempoLabelRise = 0;
        double tempoLabelDrop = 0;
        double gainLabelDrop = 0;
        double bassLabelGap = 0;
        // Connections and the playhead
        double entryBow = 0;
        double arrowRise = 0;
        double arrowGap = 0;
        double playheadOvershoot = 0;
        double playheadOpacity = 0;
        double pulseRadius = 0;
        double nowLabelGap = 0;
        double activeOpacity = 0;
        double dimOpacity = 0;
        /// A long tick and a bar number every this many bars
        int phraseBars = 0;
        // Type
        double fontSizeCue = 0;
        double fontSizeValue = 0;
        double fontSizeDeck = 0;
        double fontSizeBar = 0;
        double trackingCue = 0;
        // The legend (stack-map.css .stack-map__legend)
        double legendSample = 0;
        double legendGap = 0;
        double legendRowGap = 0;
        double legendItemGap = 0;
        double legendPaddingY = 0;
        double legendNode = 0;
    };
    Plan plan;

    bool isValid() const {
        return strokeHairline > 0 && tickCount > 0 && wheelCanvas > 0 &&
                levelRingRadius > 0 && plan.gutter > 0 && plan.phraseBars > 0 &&
                textPrimary.isValid() && !fontMono.isEmpty();
    }
    static InstrumentStyle fromVariables(const SkinVariables& variables);
    /// The style of the installed CrateDecks skin, whatever skin is loaded:
    /// the instrument widgets belong to Crate's views. Main thread only.
    static const InstrumentStyle& installed();

    /// A MonoLabel's font (design 14.1): mono, capitals, tabular figures and
    /// the caps tracking, at `pixelSize`.
    QFont monoLabelFont(double pixelSize) const;
    /// The mono font at `pixelSize` with tabular figures and `trackingEm`.
    QFont monoFont(double pixelSize, double trackingEm) const;
    /// The display font (Bricolage Grotesque) for figures: `pixelSize`, the
    /// display weight on its axis, its optical size and tabular figures.
    QFont displayFont(double pixelSize, double trackingEm) const;
};

/// The stroke patterns of StackMap (design 14.1): the type of a line is its
/// pattern, never its colour. Spoke is the related-pairs spoke of the
/// Camelot wheel.
enum class StrokePattern {
    Solid,     // request; tempo in the Smart Mix plan
    Data,      // "9 4"; gain
    Queue,     // "3 4"; bass swap
    Telemetry, // "1 5"; long beatmatch
    Axis,      // "1 7"; the dashed axes of a scale
    Spoke,     // "1.5 4"; relative keys at rest
};

/// The nodes of GenreWheel (design 14.1).
enum class NodeState {
    /// Hollow: background fill, stroke in textPrimary.
    Rest,
    /// Linked to the selection: hollow with an accentHi stroke.
    Active,
    /// The selection: filled in accent with an accentHi stroke and a glow.
    Selected,
    /// Unrelated: a resting node at opacityContext.
    Context,
};

/// `pattern` in pixels; empty for a solid line.
QVector<qreal> dashPattern(StrokePattern pattern, const InstrumentStyle& style);

/// A cosmetic hairline pen (QPen::setCosmetic, SVG's non-scaling-stroke) of
/// strokeHairline pixels in `colour`.
QPen hairlinePen(const QColor& colour, const InstrumentStyle& style);

/// A pen of `width` pixels in `colour` drawing `pattern`. Qt measures dashes
/// in pen widths; the pattern is divided by the width so that the dashes
/// have the tokens' pixels at any width.
QPen patternPen(StrokePattern pattern,
        const QColor& colour,
        double width,
        const InstrumentStyle& style);

/// `line` with the coordinate of a horizontal or vertical line moved to the
/// middle of a device pixel under `pPainter`'s transform, so that a 1 px
/// hairline covers one pixel row or column instead of two half-lit ones.
/// Other lines are returned as they are.
QLineF alignToPixels(const QPainter* pPainter, const QLineF& line);

/// A hairline from `line`, aligned to the pixel grid.
void drawHairline(QPainter* pPainter,
        const QLineF& line,
        const QColor& colour,
        const InstrumentStyle& style);

/// One tick of a scale.
struct Tick {
    QLineF line;
    bool isLong = false;
};

/// The ticks of a ring scale (StackMap's field scale; the knobs of GU1):
/// `count` ticks outwards from `radius`, the first at 12 o'clock and then
/// clockwise, with a long one every `every`.
QVector<Tick> tickRing(QPointF centre,
        double radius,
        int count,
        int every,
        double longLength,
        double shortLength);

/// Paints tickRing() in hairlines: long ticks in `longColour`, short ones in
/// `shortColour`.
void paintTickRing(QPainter* pPainter,
        QPointF centre,
        double radius,
        int count,
        int every,
        const QColor& longColour,
        const QColor& shortColour,
        const InstrumentStyle& style);

/// A ring guide of GenreWheel: a hairline circle in border at opacityGuide.
void paintGuideRing(QPainter* pPainter,
        QPointF centre,
        double radius,
        const InstrumentStyle& style);

/// A node of `radius` in `state`; `hovered` adds the hover glow.
void paintNode(QPainter* pPainter,
        QPointF centre,
        double radius,
        NodeState state,
        const InstrumentStyle& style,
        bool hovered = false);

/// A soft glow around a node of `radius`: a second, wide and translucent
/// shape painted with QPainter, never a QGraphicsEffect (design 14.5).
void paintGlow(QPainter* pPainter,
        QPointF centre,
        double radius,
        double glowRadius,
        const QColor& colour,
        double alpha);

/// GenreWheel's open arrow head (an SVG marker with markerUnits
/// strokeWidth) for an edge of `strokeWidth` that ends at `tip` coming from
/// `direction` (the tangent at the end of the edge).
void paintArrow(QPainter* pPainter,
        QPointF tip,
        QPointF direction,
        double strokeWidth,
        const QColor& colour,
        const InstrumentStyle& style);

/// The MonoLabel text of `parts`: capitals, joined by " · " with the
/// separator's padding.
QString monoLabelText(const QStringList& parts);

/// The width paintMonoLabel() gives `parts`.
double monoLabelWidth(const QStringList& parts, double pixelSize, const InstrumentStyle& style);

/// Paints a MonoLabel (design 14.1) of `parts` with its baseline starting at
/// `baseline`, and returns its width.
double paintMonoLabel(QPainter* pPainter,
        QPointF baseline,
        const QStringList& parts,
        double pixelSize,
        const QColor& colour,
        const InstrumentStyle& style);

/// `text` in `font` and `colour`, centred on `baseline.x()` with its
/// baseline at `baseline.y()` (SVG's text-anchor middle): the display figure
/// in the centre of GenreWheel and StackMap, and the MonoLabel under it.
void paintCentredText(QPainter* pPainter,
        QPointF baseline,
        const QString& text,
        const QFont& font,
        const QColor& colour);

/// The level arc of StackMap (stack-map.css .stack-map__level): the span of
/// `value` (0 to 1) from 12 o'clock clockwise, in Qt's sixteenths of a
/// degree (negative is clockwise); never less than `minimum`, and 0 for no
/// value at all.
int levelSpan(double value, double minimum);

/// StackMap's level ring: a hairline track of `radius` in `track` and
/// levelSpan() of `value` in `level`, levelRingWidth wide with round caps.
void paintLevelRing(QPainter* pPainter,
        QPointF centre,
        double radius,
        double value,
        const QColor& track,
        const QColor& level,
        const InstrumentStyle& style);

} // namespace crate::widgets
