#include "crate/ui/widgets/transitionplanview.h"

#include <QFontMetricsF>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>
#include <optional>

#include "moc_transitionplanview.cpp"

namespace crate::widgets {

namespace {

using Side = TransitionPlanViewModel::Side;
using MomentKind = TransitionPlanViewModel::MomentKind;
using TickKind = TransitionPlanViewModel::TickKind;

// Ticks closer than this run into one stroke: the ruler leaves out the
// beats, then the bars
constexpr double kMinTickSpacingPx = 3.0;

enum class Anchor {
    Start,
    Middle,
    End,
};

/// The middle of the device pixel `coordinate` falls in, for a crisp
/// hairline
double crisp(double coordinate) {
    return std::floor(coordinate) + 0.5;
}

/// Where `text` goes in `font` with its baseline at `baseline`, anchored as
/// SVG's text-anchor
QRectF textRect(QPointF baseline, const QString& text, const QFont& font, Anchor anchor) {
    const QFontMetricsF metrics(font);
    const double width = metrics.horizontalAdvance(text);
    double left = baseline.x();
    if (anchor == Anchor::Middle) {
        left -= width / 2;
    } else if (anchor == Anchor::End) {
        left -= width;
    }
    return QRectF(left,
            baseline.y() - metrics.ascent(),
            width,
            metrics.ascent() + metrics.descent());
}

/// `rect` moved, not shrunk, to lie between `left` and `right`
QRectF within(QRectF rect, double left, double right) {
    if (rect.right() > right) {
        rect.moveRight(right);
    }
    if (rect.left() < left) {
        rect.moveLeft(left);
    }
    return rect;
}

void drawText(QPainter* pPainter,
        const QRectF& rect,
        const QString& text,
        const QFont& font,
        const QColor& colour) {
    const QFontMetricsF metrics(font);
    pPainter->save();
    pPainter->setFont(font);
    pPainter->setPen(colour);
    pPainter->drawText(QPointF(rect.left(), rect.top() + metrics.ascent()), text);
    pPainter->restore();
}

/// The labels of one row, which must not run into each other
class LabelRow {
  public:
    explicit LabelRow(double gap)
            : m_gap(gap) {
    }
    /// Takes `rect` if it is free
    bool take(const QRectF& rect) {
        const QRectF padded = rect.adjusted(-m_gap / 2, 0, m_gap / 2, 0);
        for (const QRectF& taken : std::as_const(m_taken)) {
            if (taken.intersects(padded)) {
                return false;
            }
        }
        m_taken.append(padded);
        return true;
    }

  private:
    const double m_gap;
    QVector<QRectF> m_taken;
};

/// A cosmetic pen of `width` in `colour` with `pattern`
QPen connectionPen(StrokePattern pattern,
        const QColor& colour,
        double width,
        const InstrumentStyle& style) {
    QPen pen = patternPen(pattern, colour, width, style);
    pen.setCosmetic(true);
    if (pattern == StrokePattern::Telemetry) {
        // stack-map.css: telemetry dots are round
        pen.setCapStyle(Qt::RoundCap);
    } else {
        pen.setCapStyle(Qt::FlatCap);
    }
    return pen;
}

/// One item of the legend: a stroke sample or a cue node, and its name
struct LegendItem {
    std::optional<StrokePattern> pattern;
    MomentKind node = MomentKind::Track;
    QString text;
};

QVector<LegendItem> legendItems() {
    return {
            {StrokePattern::Solid, MomentKind::Track, TransitionPlanView::tr("Tempo")},
            {StrokePattern::Queue, MomentKind::Track, TransitionPlanView::tr("Bass swap")},
            {StrokePattern::Data, MomentKind::Track, TransitionPlanView::tr("Gain")},
            {StrokePattern::Telemetry, MomentKind::Track, TransitionPlanView::tr("Long beatmatch")},
            {StrokePattern::Axis, MomentKind::Track, TransitionPlanView::tr("Transition window")},
            {std::nullopt, MomentKind::Entry, TransitionPlanView::tr("Entry cue")},
            {std::nullopt, MomentKind::Plan, TransitionPlanView::tr("Plan cue")},
            {std::nullopt, MomentKind::Track, TransitionPlanView::tr("Track cue")},
    };
}

QString legendTitle() {
    return TransitionPlanView::tr(
            "Plan · each aspect is a stroke pattern · arrows point to the incoming deck");
}

} // namespace

TransitionPlanView::TransitionPlanView(TransitionPlanViewModel* pModel,
        const InstrumentStyle& style,
        QWidget* pParent)
        : QWidget(pParent),
          m_pModel(pModel),
          m_style(style) {
    setObjectName(QStringLiteral("CrateTransitionPlanView"));
    // The height is the pane's to give: the view fits what it shows to it
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    connect(m_pModel, &TransitionPlanViewModel::changed, this, [this] {
        // Whether there is a plan decides what fits
        m_layoutSize = QSize();
        invalidate();
    });
    connect(m_pModel,
            &TransitionPlanViewModel::nowChanged,
            this,
            &TransitionPlanView::onNowChanged);
}

const TransitionPlanView::Layout& TransitionPlanView::stage() const {
    if (m_layoutSize != size()) {
        m_layout = layoutFor(size());
        m_layoutSize = size();
    }
    return m_layout;
}

int TransitionPlanView::compactHeight() const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    const QFontMetricsF cue(m_style.monoFont(plan.fontSizeCue, plan.trackingCue));
    const QFontMetricsF value(m_style.monoFont(plan.fontSizeValue, m_style.trackingCaps));
    // The moments' names above and below the rulers, and the bass swap's
    // between the connections
    const double top = plan.cueLabelRise + cue.ascent();
    const double rulers = plan.arrowRise + plan.arrowGap + value.height();
    const double bottom = plan.cueLabelDrop + cue.descent() + m_style.strokeHairline;
    return static_cast<int>(std::ceil(top + rulers + bottom));
}

int TransitionPlanView::lanesHeight() const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    const QFontMetricsF value(m_style.monoFont(plan.fontSizeValue, m_style.trackingCaps));
    const double top = std::max(plan.rulerInset, plan.windowLabelRise + value.ascent());
    const double rulers = 2 * plan.laneInset + 2 * plan.laneMinGap;
    return static_cast<int>(std::ceil(top + rulers + plan.rulerInset));
}

double TransitionPlanView::legendHeight(double width, bool title) const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    const QFontMetricsF metrics(m_style.monoFont(plan.fontSizeValue, 0));
    const double line = metrics.height();
    int rows = 1;
    double x = 0;
    for (const LegendItem& item : legendItems()) {
        const double mark = item.pattern ? plan.legendSample : plan.legendNode;
        const double itemWidth = mark + plan.legendItemGap + metrics.horizontalAdvance(item.text);
        if (x > 0 && x + itemWidth > width) {
            ++rows;
            x = 0;
        }
        x += itemWidth + plan.legendGap;
    }
    double height = 2 * plan.legendPaddingY + rows * line + (rows - 1) * plan.legendRowGap;
    if (title) {
        height += line + plan.legendRowGap;
    }
    return height;
}

QSize TransitionPlanView::minimumSizeHint() const {
    const int width = static_cast<int>(
            std::ceil(2 * m_style.plan.gutter + m_style.plan.paddingRight));
    return QSize(width, compactHeight());
}

QSize TransitionPlanView::sizeHint() const {
    const QSize minimum = minimumSizeHint();
    const int width = std::max(this->width(), minimum.width());
    return QSize(minimum.width(),
            lanesHeight() + static_cast<int>(std::ceil(legendHeight(width, true))));
}

TransitionPlanView::Layout TransitionPlanView::layoutFor(QSize size) const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    Layout layout;
    layout.plotLeft = plan.gutter;
    layout.plotRight = std::max(plan.gutter + 1, size.width() - plan.paddingRight);
    const double height = size.height();
    if (!m_pModel->hasPlan() || height < compactHeight()) {
        return layout;
    }
    layout.timeline = true;
    const int lanes = lanesHeight();
    if (height < lanes) {
        // Compact: the rulers as far apart as the height lets them
        const QFontMetricsF cue(m_style.monoFont(plan.fontSizeCue, plan.trackingCue));
        layout.outgoingY = plan.cueLabelRise + cue.ascent();
        layout.incomingY = height - (plan.cueLabelDrop + cue.descent() + m_style.strokeHairline);
        const double middle = (layout.outgoingY + layout.incomingY) / 2;
        layout.lanes3 = {middle, middle, middle};
        layout.legendTop = height;
        return layout;
    }
    layout.lanes = true;
    double legend = 0;
    const double withTitle = legendHeight(size.width(), true);
    const double withoutTitle = legendHeight(size.width(), false);
    if (height >= lanes + withTitle) {
        layout.legend = true;
        layout.legendTitle = true;
        legend = withTitle;
    } else if (height >= lanes + withoutTitle) {
        layout.legend = true;
        legend = withoutTitle;
    }
    const QFontMetricsF value(m_style.monoFont(plan.fontSizeValue, m_style.trackingCaps));
    layout.outgoingY = std::max(plan.rulerInset, plan.windowLabelRise + value.ascent());
    layout.legendTop = height - legend;
    layout.incomingY = layout.legendTop - plan.rulerInset;
    const double between = (layout.incomingY - layout.outgoingY - 2 * plan.laneInset) / 2;
    for (int lane = 0; lane < 3; ++lane) {
        layout.lanes3[lane] = layout.outgoingY + plan.laneInset + lane * between;
    }
    return layout;
}

double TransitionPlanView::xOf(double atMs) const {
    stage();
    const double span = m_pModel->rangeEndMs() - m_pModel->rangeStartMs();
    if (span <= 0) {
        return m_layout.plotLeft;
    }
    return m_layout.plotLeft +
            (atMs - m_pModel->rangeStartMs()) / span * (m_layout.plotRight - m_layout.plotLeft);
}

QRegion TransitionPlanView::playheadRegion() const {
    return m_playhead.region;
}

TransitionPlanView::Playhead TransitionPlanView::playhead() const {
    stage();
    Playhead playhead;
    if (!m_layout.timeline) {
        return playhead;
    }
    const InstrumentStyle::Plan& plan = m_style.plan;
    const double x = xOf(m_pModel->nowMs());
    playhead.inRange = x >= m_layout.plotLeft && x <= m_layout.plotRight;
    playhead.x = static_cast<int>(
            std::floor(std::clamp(x, m_layout.plotLeft, m_layout.plotRight)));
    playhead.label = m_pModel->nowLabel().toUpper();

    const double reach = plan.pulseRadius + m_style.glowPulseRadius + m_style.strokeHairline;
    const double top = std::max(0.0, m_layout.outgoingY - plan.playheadOvershoot - reach);
    const double bottom = std::min<double>(
            height(), m_layout.incomingY + plan.playheadOvershoot + m_style.strokeHairline);
    QRectF rect(playhead.x - reach, top, 2 * reach + 1, bottom - top);
    const QFont font = m_style.monoFont(plan.fontSizeCue, m_style.trackingCaps);
    const QPointF baseline(playhead.x - plan.nowLabelGap, m_layout.incomingY + plan.cueLabelDrop);
    // Left of the playhead, as the mockup, clear of the incoming deck's
    // names on its row by the legend's gap between items; right of it when
    // there is no room on the left
    const auto clear = [this, &plan](QRectF label, double step) {
        for (int pass = 0; pass <= m_incomingNames.size(); ++pass) {
            bool moved = false;
            for (const QRectF& name : std::as_const(m_incomingNames)) {
                if (name.adjusted(-plan.legendGap, 0, plan.legendGap, 0).intersects(label)) {
                    if (step < 0) {
                        label.moveRight(name.left() - plan.legendGap);
                    } else {
                        label.moveLeft(name.right() + plan.legendGap);
                    }
                    moved = true;
                }
            }
            if (!moved) {
                break;
            }
        }
        return label;
    };
    QRectF label = clear(textRect(baseline, playhead.label, font, Anchor::End), -1);
    if (label.left() < 0) {
        label = clear(textRect(QPointF(playhead.x + plan.nowLabelGap, baseline.y()),
                              playhead.label,
                              font,
                              Anchor::Start),
                1);
    }
    playhead.labelRect = label;
    playhead.region = QRegion(rect.toAlignedRect().adjusted(-1, -1, 1, 1))
                              .united(label.toAlignedRect().adjusted(-1, -1, 1, 1));
    return playhead;
}

void TransitionPlanView::invalidate() {
    m_static = QPixmap();
    m_playhead = playhead();
    update();
}

void TransitionPlanView::onNowChanged() {
    if (m_layoutSize != size()) {
        // Resized while hidden: all of it again
        invalidate();
        return;
    }
    const Playhead next = playhead();
    if (next.x == m_playhead.x && next.label == m_playhead.label &&
            next.inRange == m_playhead.inRange) {
        return;
    }
    const QRegion dirty = m_playhead.region.united(next.region);
    m_playhead = next;
    update(dirty);
}

void TransitionPlanView::resizeEvent(QResizeEvent* pEvent) {
    invalidate();
    QWidget::resizeEvent(pEvent);
}

void TransitionPlanView::paintEvent(QPaintEvent* /*pEvent*/) {
    stage();
    const qreal ratio = devicePixelRatioF();
    if (m_static.isNull() || m_static.devicePixelRatio() != ratio ||
            m_static.deviceIndependentSize() != QSizeF(size())) {
        paintStatic();
        m_playhead = playhead();
    }
    QPainter painter(this);
    painter.drawPixmap(0, 0, m_static);
    if (m_layout.timeline) {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        paintPlayhead(&painter, m_playhead);
    }
}

void TransitionPlanView::paintStatic() {
    const qreal ratio = devicePixelRatioF();
    m_static = QPixmap(size() * ratio);
    m_static.setDevicePixelRatio(ratio);
    m_static.fill(Qt::transparent);
    m_incomingNames.clear();
    if (!m_layout.timeline) {
        return;
    }
    QPainter painter(&m_static);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    paintRulers(&painter);
    if (m_layout.lanes) {
        paintLanes(&painter);
    }
    paintConnections(&painter);
    paintMoments(&painter);
    if (m_layout.legend) {
        paintLegend(&painter);
    }
}

void TransitionPlanView::paintRulers(QPainter* pPainter) const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    const double left = m_layout.plotLeft;
    const double right = m_layout.plotRight;
    const double yA = m_layout.outgoingY;
    const double yB = m_layout.incomingY;

    // The transition window between dashed axes (StackMap's field axes)
    pPainter->save();
    pPainter->setPen(connectionPen(
            StrokePattern::Axis, m_style.border, m_style.strokeHairline, m_style));
    const double axisTop = std::max(0.0, yA - plan.windowRise);
    const double axisBottom = std::min(m_layout.legendTop, yB + plan.windowDrop);
    for (const double atMs : {m_pModel->windowStartMs(), m_pModel->windowEndMs()}) {
        const double x = crisp(xOf(atMs));
        pPainter->drawLine(QLineF(x, axisTop, x, axisBottom));
    }
    pPainter->restore();

    const double spanMs = m_pModel->rangeEndMs() - m_pModel->rangeStartMs();
    const double pxPerMs = spanMs > 0 ? (right - left) / spanMs : 0;
    const QFont barFont = m_style.monoFont(plan.fontSizeBar, 0);
    const QFont deckFont = m_style.monoFont(plan.fontSizeDeck, m_style.trackingCaps);
    const QFont roleFont = m_style.monoFont(plan.fontSizeCue, m_style.trackingCaps);
    const QPen beatPen = hairlinePen(m_style.border, m_style);
    const QPen barPen = hairlinePen(m_style.textFaint, m_style);
    const QPen phrasePen = hairlinePen(m_style.textMuted, m_style);
    for (const Side side : {Side::Outgoing, Side::Incoming}) {
        const bool outgoing = side == Side::Outgoing;
        const double y = outgoing ? yA : yB;
        const double direction = outgoing ? -1 : 1;
        const double from = std::max(left, xOf(m_pModel->trackStartMs(side)));
        const double to = std::min(right, xOf(m_pModel->trackEndMs(side)));
        pPainter->save();
        if (!outgoing && from > left) {
            // Before its entry the incoming deck is silent
            pPainter->setPen(connectionPen(
                    StrokePattern::Telemetry, m_style.border, m_style.strokeHairline, m_style));
            pPainter->drawLine(QLineF(left, crisp(y), from, crisp(y)));
        }
        if (to > from) {
            pPainter->setPen(hairlinePen(m_style.border, m_style));
            pPainter->drawLine(QLineF(from, crisp(y), to, crisp(y)));
        }

        // Beats, bars and phrases (StackMap's scale: a long tick every 4,
        // and a longer one with its number every phrase)
        const auto& ticks = m_pModel->ticks(side);
        double beatPx = 0;
        if (ticks.size() > 1) {
            beatPx = (ticks.last().atMs - ticks.first().atMs) / (ticks.size() - 1) * pxPerMs;
        }
        const int beatsPerBar = std::max(1, m_pModel->deck(side).beatsPerBar);
        const bool drawBeats = beatPx >= kMinTickSpacingPx;
        const bool drawBars = beatPx * beatsPerBar >= kMinTickSpacingPx;
        LabelRow numbers(plan.barLabelGap);
        pPainter->setRenderHint(QPainter::Antialiasing, false);
        for (const auto& tick : ticks) {
            const double x = xOf(tick.atMs);
            if (x < left || x > right) {
                continue;
            }
            const bool isBar = tick.kind == TickKind::Bar;
            const bool isPhrase = isBar && plan.phraseBars > 0 &&
                    (tick.bar - 1) % plan.phraseBars == 0;
            if ((!isBar && !drawBeats) || (isBar && !isPhrase && !drawBars)) {
                continue;
            }
            const double length = isPhrase ? m_style.tickPhrase
                    : isBar                ? m_style.tickLong
                                           : m_style.tickShort;
            pPainter->setPen(isPhrase ? phrasePen : isBar ? barPen
                                                          : beatPen);
            pPainter->drawLine(QLineF(crisp(x), y, crisp(x), y + direction * length));
            if (isPhrase && tick.bar > 0) {
                const QString number = QString::number(tick.bar);
                const QRectF rect = textRect(
                        QPointF(x + plan.barLabelGap,
                                y +
                                        (outgoing ? -plan.barLabelRise
                                                  : plan.barLabelDrop)),
                        number,
                        barFont,
                        Anchor::Start);
                if (rect.right() <= right + plan.paddingRight && numbers.take(rect)) {
                    pPainter->setRenderHint(QPainter::Antialiasing, true);
                    drawText(pPainter, rect, number, barFont, m_style.textSubtle);
                    pPainter->setRenderHint(QPainter::Antialiasing, false);
                }
            }
        }
        pPainter->restore();

        // The deck in the gutter: "A · LATTICE", then its role. The
        // incoming deck's name may run on over its silent ruler
        const QString letter = m_pModel->deckLetter(side);
        const bool accentDeck = letter == QStringLiteral("A") || letter == QStringLiteral("C");
        const QFontMetricsF metrics(deckFont);
        const QString separator = QStringLiteral(" · ");
        const QPointF baseline(0, y - plan.deckLabelRise);
        const double room = (outgoing ? left : std::max(left, from)) - plan.valueGap;
        const double lead = metrics.horizontalAdvance(letter + separator);
        const QString title = metrics.elidedText(
                m_pModel->deck(side).title.toUpper(), Qt::ElideRight, std::max(0.0, room - lead));
        drawText(pPainter,
                textRect(baseline, letter, deckFont, Anchor::Start),
                letter,
                deckFont,
                accentDeck ? m_style.deckA : m_style.deckB);
        if (!title.isEmpty()) {
            const QString rest = separator + title;
            drawText(pPainter,
                    textRect(QPointF(metrics.horizontalAdvance(letter), baseline.y()),
                            rest,
                            deckFont,
                            Anchor::Start),
                    rest,
                    deckFont,
                    m_style.textSubtle);
        }
        const QString role = m_pModel->roleLabel(side).toUpper();
        drawText(pPainter,
                textRect(QPointF(0, y + plan.deckRoleDrop), role, roleFont, Anchor::Start),
                role,
                roleFont,
                m_style.textSubtle);
    }
}

void TransitionPlanView::paintLanes(QPainter* pPainter) const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    const double left = m_layout.plotLeft;
    const double right = m_layout.plotRight;
    const QFont nameFont = m_style.monoFont(plan.fontSizeCue, m_style.trackingCaps);
    const QFont valueFont = m_style.monoFont(plan.fontSizeValue, 0);
    const QString names[] = {tr("Tempo"), tr("Gain"), tr("Beatmatch")};
    for (int lane = 0; lane < 3; ++lane) {
        const double y = m_layout.lanes3[lane];
        const QString name = names[lane].toUpper();
        drawText(pPainter,
                textRect(QPointF(0, y + plan.laneLabelDrop), name, nameFont, Anchor::Start),
                name,
                nameFont,
                m_style.textMuted);
        drawHairline(pPainter, QLineF(left, y, right, y), m_style.borderSubtle, m_style);
    }

    pPainter->save();
    pPainter->setClipRect(QRectF(left, 0, right - left, height()));
    pPainter->setBrush(Qt::NoBrush);
    const QPen active = connectionPen(
            StrokePattern::Solid, m_style.accentHi, m_style.strokeConnection, m_style);

    // The incoming deck's tempo, solid: its tempo during the transition
    // and, after a beatmatch, back to its own
    if (const auto& tempo = m_pModel->tempo(); tempo && !tempo->points.isEmpty()) {
        const double y = m_layout.lanes3[0];
        const double low = std::min(tempo->fromBpm, tempo->toBpm);
        const double high = std::max(tempo->fromBpm, tempo->toBpm);
        const auto yOf = [&](double bpm) {
            if (!tempo->changes() || high <= low) {
                return y;
            }
            return y + plan.tempoSwing - (bpm - low) / (high - low) * 2 * plan.tempoSwing;
        };
        QPainterPath path(QPointF(xOf(tempo->points.first().atMs),
                yOf(tempo->points.first().value)));
        for (const auto& point : tempo->points) {
            path.lineTo(xOf(point.atMs), yOf(point.value));
        }
        pPainter->setOpacity(plan.activeOpacity);
        pPainter->setPen(active);
        pPainter->drawPath(path);
        pPainter->setOpacity(1);
        const bool falls = tempo->fromBpm >= tempo->toBpm;
        const QString first = QString::number(tempo->fromBpm, 'f', 2);
        const QRectF firstRect = textRect(
                QPointF(xOf(tempo->points.first().atMs) + plan.valueGap,
                        falls ? y - plan.tempoLabelRise : y + plan.tempoLabelDrop),
                first,
                valueFont,
                Anchor::Start);
        drawText(pPainter, firstRect, first, valueFont, m_style.textSecondary);
        if (tempo->changes()) {
            const QString last = QString::number(tempo->toBpm, 'f', 2);
            const QRectF lastRect = textRect(
                    QPointF(xOf(tempo->points.last().atMs) - plan.valueGap,
                            falls ? y + plan.tempoLabelDrop : y - plan.tempoLabelRise),
                    last,
                    valueFont,
                    Anchor::End);
            drawText(pPainter, lastRect, last, valueFont, m_style.textSecondary);
        }
    }

    // Both decks' levels, "9 4": the incoming one active, the outgoing one
    // dimmed
    if (const auto& levels = m_pModel->levels()) {
        const double y = m_layout.lanes3[1];
        const auto path = [&](const QVector<TransitionPlanViewModel::Point>& points) {
            QPainterPath curve;
            for (const auto& point : points) {
                const QPointF at(xOf(point.atMs),
                        y + plan.gainSwing - point.value * 2 * plan.gainSwing);
                if (curve.elementCount() == 0) {
                    curve.moveTo(at);
                } else {
                    curve.lineTo(at);
                }
            }
            return curve;
        };
        pPainter->setOpacity(plan.dimOpacity);
        pPainter->setPen(connectionPen(
                StrokePattern::Data, m_style.textMuted, m_style.strokeHairline, m_style));
        pPainter->drawPath(path(levels->outgoing));
        pPainter->setOpacity(plan.activeOpacity);
        pPainter->setPen(connectionPen(
                StrokePattern::Data, m_style.accentHi, m_style.strokeConnection, m_style));
        pPainter->drawPath(path(levels->incoming));
        pPainter->setOpacity(1);
        LabelRow gains(plan.valueGap);
        const QString incoming = m_pModel->gainLabel(Side::Incoming);
        if (!incoming.isEmpty()) {
            const QRectF rect = textRect(
                    QPointF(xOf(m_pModel->windowStartMs()) + plan.valueGap, y + plan.gainLabelDrop),
                    incoming,
                    valueFont,
                    Anchor::Start);
            gains.take(rect);
            drawText(pPainter, rect, incoming, valueFont, m_style.textSecondary);
        }
        const QString outgoing = m_pModel->gainLabel(Side::Outgoing);
        if (!outgoing.isEmpty()) {
            const QRectF rect = textRect(
                    QPointF(xOf(m_pModel->windowEndMs()) - plan.valueGap, y + plan.gainLabelDrop),
                    outgoing,
                    valueFont,
                    Anchor::End);
            if (gains.take(rect)) {
                drawText(pPainter, rect, outgoing, valueFont, m_style.textSecondary);
            }
        }
    }

    // The beatmatched span, "1 5"
    if (const auto span = m_pModel->beatmatch()) {
        const double y = crisp(m_layout.lanes3[2]);
        pPainter->setOpacity(plan.activeOpacity);
        pPainter->setPen(connectionPen(
                StrokePattern::Telemetry, m_style.accentHi, m_style.strokeConnection, m_style));
        pPainter->drawLine(QLineF(xOf(span->first), y, xOf(span->second), y));
    }
    pPainter->restore();
}

void TransitionPlanView::paintConnections(QPainter* pPainter) const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    const double yA = m_layout.outgoingY;
    const double yB = m_layout.incomingY;
    pPainter->save();
    pPainter->setBrush(Qt::NoBrush);

    // The bass swap: a "3 4" connection from deck to deck, and its name
    if (const auto& swap = m_pModel->bassSwap()) {
        const double x = crisp(xOf(swap->atMs));
        const QPointF tip(x, yB - plan.arrowGap);
        pPainter->setOpacity(plan.activeOpacity);
        pPainter->setPen(connectionPen(
                StrokePattern::Queue, m_style.accentHi, m_style.strokeConnection, m_style));
        pPainter->drawLine(QLineF(QPointF(x, yA + plan.arrowRise), tip));
        paintArrow(pPainter,
                tip,
                QPointF(0, 1),
                m_style.strokeConnection,
                m_style.accentHi,
                m_style);
        pPainter->setOpacity(1);
        const QFont font = m_style.monoFont(plan.fontSizeValue, m_style.trackingCaps);
        const QString label = m_pModel->bassSwapLabel().toUpper();
        const double y = (m_layout.lanes ? (m_layout.lanes3[0] + m_layout.lanes3[1]) / 2
                                         : (yA + yB) / 2) +
                plan.laneLabelDrop;
        QRectF rect = textRect(QPointF(x + plan.bassLabelGap, y), label, font, Anchor::Start);
        if (rect.right() > width()) {
            rect = textRect(QPointF(x - plan.bassLabelGap, y), label, font, Anchor::End);
        }
        drawText(pPainter, rect, label, font, m_style.accentHi);
    }

    // The entry: from the outgoing deck where the transition starts into
    // the incoming deck's entry cue
    const double x = xOf(m_pModel->windowStartMs());
    const QPointF start(x, yA + plan.arrowRise);
    const QPointF end(x, yB - plan.arrowGap);
    const QPointF bow(x - plan.entryBow, (yA + yB) / 2);
    QPainterPath path(start);
    path.cubicTo(bow, bow, end);
    pPainter->setOpacity(plan.activeOpacity);
    pPainter->setPen(connectionPen(
            StrokePattern::Solid, m_style.accentHi, m_style.strokeConnection, m_style));
    pPainter->drawPath(path);
    paintArrow(pPainter, end, end - bow, m_style.strokeConnection, m_style.accentHi, m_style);
    pPainter->restore();
}

void TransitionPlanView::paintMoments(QPainter* pPainter) const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    const double left = m_layout.plotLeft;
    const double right = m_layout.plotRight;
    const QFont font = m_style.monoFont(plan.fontSizeCue, plan.trackingCue);
    const QFont windowFont = m_style.monoFont(plan.fontSizeValue, m_style.trackingCaps);
    // Names on a row keep the legend's gap between items apart
    LabelRow outgoingRow(plan.legendGap);
    LabelRow incomingRow(plan.legendGap);
    m_incomingNames.clear();
    const auto moments = m_pModel->moments();

    // Nodes: track cues first, so that the plan's sit on top
    pPainter->save();
    for (const MomentKind kind : {MomentKind::Track, MomentKind::Plan, MomentKind::Entry}) {
        for (const auto& moment : moments) {
            const double x = xOf(moment.atMs);
            if (moment.kind != kind || x < left || x > right) {
                continue;
            }
            const QPointF centre(x,
                    moment.side == Side::Outgoing ? m_layout.outgoingY : m_layout.incomingY);
            double radius = m_style.nodeSibling;
            QColor fill = m_style.background;
            QColor stroke = m_style.textPrimary;
            double width = m_style.strokeNode;
            if (kind == MomentKind::Entry) {
                radius = m_style.nodeRoot;
                fill = m_style.accent;
                stroke = m_style.accentHi;
                width = m_style.strokeNodeActive;
            } else if (kind == MomentKind::Plan) {
                stroke = m_style.accentHi;
            }
            QPen pen(stroke, width);
            pen.setCosmetic(true);
            pPainter->setPen(pen);
            pPainter->setBrush(fill);
            pPainter->drawEllipse(centre, radius, radius);
        }
    }
    pPainter->restore();

    // Names: the plan's first, then the window's, then the tracks' where
    // there is room
    const auto name = [&](const TransitionPlanViewModel::Moment& moment) {
        const double x = xOf(moment.atMs);
        if (x < left || x > right) {
            return;
        }
        const bool outgoing = moment.side == Side::Outgoing;
        const QString label = moment.label.toUpper();
        const QRectF rect =
                within(textRect(QPointF(x,
                                        outgoing ? m_layout.outgoingY -
                                                        plan.cueLabelRise
                                                 : m_layout.incomingY +
                                                        plan.cueLabelDrop),
                               label,
                               font,
                               Anchor::Middle),
                        0,
                        width());
        if ((outgoing ? outgoingRow : incomingRow).take(rect)) {
            if (!outgoing) {
                m_incomingNames.append(rect);
            }
            drawText(pPainter,
                    rect,
                    label,
                    font,
                    moment.kind == MomentKind::Track ? m_style.textMuted : m_style.textPrimary);
        }
    };
    for (const auto& moment : moments) {
        if (moment.kind != MomentKind::Track) {
            name(moment);
        }
    }
    const QString window = m_pModel->windowLabel().toUpper();
    const double middle = (xOf(m_pModel->windowStartMs()) + xOf(m_pModel->windowEndMs())) / 2;
    if (m_layout.lanes) {
        const QRectF rect = within(
                textRect(QPointF(middle,
                                 m_layout.outgoingY - plan.windowLabelRise),
                        window,
                        windowFont,
                        Anchor::Middle),
                left,
                width());
        drawText(pPainter, rect, window, windowFont, m_style.accentHi);
    } else {
        // Compact: on the outgoing deck's row of names, if it fits
        const QRectF rect = within(textRect(QPointF(middle, m_layout.outgoingY - plan.cueLabelRise),
                                           window,
                                           windowFont,
                                           Anchor::Middle),
                left,
                width());
        if (outgoingRow.take(rect)) {
            drawText(pPainter, rect, window, windowFont, m_style.accentHi);
        }
    }
    for (const auto& moment : moments) {
        if (moment.kind == MomentKind::Track) {
            name(moment);
        }
    }
}

void TransitionPlanView::paintLegend(QPainter* pPainter) const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    const QFont font = m_style.monoFont(plan.fontSizeValue, 0);
    const QFontMetricsF metrics(font);
    const double line = metrics.height();
    drawHairline(pPainter,
            QLineF(0, m_layout.legendTop, width(), m_layout.legendTop),
            m_style.border,
            m_style);
    double y = m_layout.legendTop + plan.legendPaddingY;
    if (m_layout.legendTitle) {
        const QString title = legendTitle();
        drawText(pPainter,
                textRect(QPointF(0, y + metrics.ascent()), title, font, Anchor::Start),
                title,
                font,
                m_style.textSubtle);
        y += line + plan.legendRowGap;
    }
    double x = 0;
    pPainter->save();
    for (const LegendItem& item : legendItems()) {
        const double mark = item.pattern ? plan.legendSample : plan.legendNode;
        const double itemWidth = mark + plan.legendItemGap + metrics.horizontalAdvance(item.text);
        if (x > 0 && x + itemWidth > width()) {
            x = 0;
            y += line + plan.legendRowGap;
        }
        const double middle = y + line / 2;
        if (item.pattern) {
            QPen pen = connectionPen(
                    *item.pattern, m_style.textMuted, m_style.strokeHairline, m_style);
            pen.setCapStyle(Qt::RoundCap);
            pPainter->setPen(pen);
            pPainter->drawLine(QLineF(x, crisp(middle), x + mark, crisp(middle)));
        } else {
            // stack-map.css .stack-map__legend i: a 9 px dot with a 1 px rim
            QColor fill = Qt::transparent;
            QColor rim = m_style.textMuted;
            if (item.node == MomentKind::Entry) {
                fill = m_style.accent;
                rim = m_style.accentHi;
            } else if (item.node == MomentKind::Plan) {
                rim = m_style.accentHi;
            }
            const double radius = (mark - m_style.strokeHairline) / 2;
            pPainter->setPen(hairlinePen(rim, m_style));
            pPainter->setBrush(fill);
            pPainter->drawEllipse(QPointF(x + mark / 2, middle), radius, radius);
        }
        drawText(pPainter,
                textRect(QPointF(x + mark + plan.legendItemGap, y + metrics.ascent()),
                        item.text,
                        font,
                        Anchor::Start),
                item.text,
                font,
                m_style.textMuted);
        x += itemWidth + plan.legendGap;
    }
    pPainter->restore();
}

void TransitionPlanView::paintPlayhead(QPainter* pPainter, const Playhead& playhead) const {
    const InstrumentStyle::Plan& plan = m_style.plan;
    const double x = playhead.x + 0.5;
    const double yA = m_layout.outgoingY;
    pPainter->save();
    // Out of range it waits at the edge, as context
    const double context = playhead.inRange ? 1.0 : m_style.opacityContext;
    pPainter->setOpacity(plan.playheadOpacity * context);
    pPainter->setPen(hairlinePen(m_style.textPrimary, m_style));
    pPainter->drawLine(QLineF(x,
            std::max(0.0, yA - plan.playheadOvershoot),
            x,
            std::min<double>(height(), m_layout.incomingY + plan.playheadOvershoot)));
    // StackMap's live pulse
    pPainter->setOpacity(context);
    paintGlow(pPainter,
            QPointF(x, yA),
            plan.pulseRadius,
            m_style.glowPulseRadius,
            m_style.accentHi,
            m_style.glowPulseAlpha);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(m_style.accentHi);
    pPainter->drawEllipse(QPointF(x, yA), plan.pulseRadius, plan.pulseRadius);
    pPainter->setOpacity(1);
    const QFont font = m_style.monoFont(plan.fontSizeCue, m_style.trackingCaps);
    drawText(pPainter, playhead.labelRect, playhead.label, font, m_style.textPrimary);
    pPainter->restore();
}

} // namespace crate::widgets
