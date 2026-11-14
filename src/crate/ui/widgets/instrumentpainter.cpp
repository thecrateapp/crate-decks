#include "crate/ui/widgets/instrumentpainter.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <numbers>

#include "crate/ui/dialogs/dialogstyle.h"
#include "crate/ui/skin/crateskin.h"

namespace crate::widgets {

namespace {

// QSS and the generator write fractional font sizes in points, at Qt's
// 96 dpi reference; the widgets do the same so that both agree.
constexpr double kPointsPerPixel = 72.0 / 96.0;

QColor colour(const SkinVariables& variables, const QString& name) {
    return QColor(variables.value(name));
}

double number(const SkinVariables& variables, const QString& name) {
    bool ok = false;
    const double value = variables.value(name).toDouble(&ok);
    return ok ? value : 0;
}

/// CrateInst<Token>, the instrument variables of vars.xml
double token(const SkinVariables& variables, const char* name) {
    return number(variables, QStringLiteral("CrateInst") + QLatin1String(name));
}

int wholeToken(const SkinVariables& variables, const char* name) {
    return static_cast<int>(std::lround(token(variables, name)));
}

QVector<qreal> patternToken(const SkinVariables& variables, const char* name) {
    QVector<qreal> pattern;
    const QStringList parts =
            variables.value(QStringLiteral("CrateInst") + QLatin1String(name))
                    .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        bool ok = false;
        const double length = part.toDouble(&ok);
        if (!ok) {
            return {};
        }
        pattern.append(length);
    }
    return pattern;
}

void setPixelSize(QFont* pFont, double pixelSize) {
    if (pixelSize <= 0) {
        return;
    }
    if (std::abs(pixelSize - std::round(pixelSize)) < 0.01) {
        pFont->setPixelSize(static_cast<int>(std::lround(pixelSize)));
    } else {
        pFont->setPointSizeF(pixelSize * kPointsPerPixel);
    }
}

/// The direction of `degrees` measured clockwise from 12 o'clock
QPointF clockwise(double degrees) {
    const double radians = degrees * std::numbers::pi / 180;
    return QPointF(std::sin(radians), -std::cos(radians));
}

} // namespace

InstrumentStyle InstrumentStyle::fromVariables(const SkinVariables& variables) {
    InstrumentStyle style;
    style.background = colour(variables, QStringLiteral("CrateBgApp"));
    style.textPrimary = colour(variables, QStringLiteral("CrateTextPrimary"));
    style.textMuted = colour(variables, QStringLiteral("CrateTextMuted"));
    style.textSubtle = colour(variables, QStringLiteral("CrateTextSubtle"));
    style.border = colour(variables, QStringLiteral("CrateBorder"));
    style.borderSubtle = colour(variables, QStringLiteral("CrateBorderSubtle"));
    style.accent = colour(variables, QStringLiteral("CrateAccent"));
    style.accentHi = colour(variables, QStringLiteral("CrateAccentHi"));

    style.fontMono = variables.value(QStringLiteral("CrateFontMono"));
    style.fontDisplay = variables.value(QStringLiteral("CrateFontDisplay"));
    style.fontSizeLabel = token(variables, "FontSizeMonoLabel");
    style.fontSizeMeta = token(variables, "FontSizeMeta");
    style.fontSizeWheelLabel = token(variables, "FontSizeWheelLabel");
    style.fontSizeDisplay = token(variables, "FontSizeDisplay");
    style.fontWeightDisplay = wholeToken(variables, "FontWeightDisplay");
    style.trackingCaps = number(variables, QStringLiteral("CrateTrackingCaps"));
    style.trackingWheelLabel = token(variables, "TrackingWheelLabel");
    style.trackingWheelFigure = token(variables, "TrackingWheelFigure");
    style.separatorPadding = token(variables, "MonoSeparatorPadding");

    style.strokeHairline = token(variables, "StrokeHairline");
    style.strokeNode = token(variables, "StrokeNode");
    style.strokeActive = token(variables, "StrokeActive");
    style.strokeNodeActive = token(variables, "StrokeNodeActive");
    style.strokeSelected = token(variables, "StrokeSelected");
    style.strokeConnection = token(variables, "StrokeConnection");
    style.nodeRoot = token(variables, "NodeRoot");
    style.nodeSelected = token(variables, "NodeSelected");
    style.glowSelectedRadius = token(variables, "GlowSelectedRadius");
    style.glowSelectedAlpha = token(variables, "GlowSelectedAlpha");
    style.glowHoverRadius = token(variables, "GlowHoverRadius");
    style.glowHoverAlpha = token(variables, "GlowHoverAlpha");
    style.tickCount = wholeToken(variables, "TickCount");
    style.tickEvery = wholeToken(variables, "TickEvery");
    style.tickLong = token(variables, "TickLong");
    style.tickShort = token(variables, "TickShort");
    style.dashData = patternToken(variables, "DashData");
    style.dashQueue = patternToken(variables, "DashQueue");
    style.dashTelemetry = patternToken(variables, "DashTelemetry");
    style.dashAxis = patternToken(variables, "DashAxis");
    style.dashSpoke = patternToken(variables, "DashSpoke");
    style.opacityContext = token(variables, "OpacityContext");
    style.opacityGuide = token(variables, "OpacityGuide");
    style.arrowMarkerSize = token(variables, "ArrowMarkerSize");
    style.arrowMarkerBox = token(variables, "ArrowMarkerBox");
    style.arrowMarkerTip = token(variables, "ArrowMarkerTip");
    style.arrowMarkerBack = token(variables, "ArrowMarkerBack");
    style.arrowMarkerSpread = token(variables, "ArrowMarkerSpread");

    style.wheelCanvas = token(variables, "WheelCanvas");
    style.camelotRingA = token(variables, "CamelotRingA");
    style.camelotRingB = token(variables, "CamelotRingB");
    style.camelotWheelSize = wholeToken(variables, "CamelotWheelSize");
    style.camelotLabelGap = token(variables, "CamelotLabelGap");
    style.camelotNameGap = token(variables, "CamelotNameGap");
    style.camelotLabelShift = token(variables, "CamelotLabelShift");
    style.camelotEdgeBow = token(variables, "CamelotEdgeBow");
    style.camelotFigureBaseline = token(variables, "CamelotFigureBaseline");
    style.camelotSubtitleBaseline = token(variables, "CamelotSubtitleBaseline");

    style.levelRingRadius = number(variables, QStringLiteral("CrateLevelRingRadius"));
    style.levelRingWidth = number(variables, QStringLiteral("CrateLevelRingWidth"));
    style.levelRingTrackWidth = number(variables, QStringLiteral("CrateLevelRingTrackWidth"));
    style.levelRingMinimum = number(variables, QStringLiteral("CrateLevelRingMinimum"));
    style.levelRingInset = token(variables, "LevelRingInset");
    return style;
}

const InstrumentStyle& InstrumentStyle::installed() {
    static const InstrumentStyle kStyle = fromVariables(DialogMetrics::installed().variables());
    return kStyle;
}

QFont InstrumentStyle::monoFont(double pixelSize, double trackingEm) const {
    QFont font(fontMono);
    setPixelSize(&font, pixelSize);
    font.setLetterSpacing(QFont::AbsoluteSpacing, trackingEm * pixelSize);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    // Font features came with Qt 6.7
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    return font;
}

QFont InstrumentStyle::monoLabelFont(double pixelSize) const {
    QFont font = monoFont(pixelSize, trackingCaps);
    font.setCapitalization(QFont::AllUppercase);
    return font;
}

QFont InstrumentStyle::displayFont(double pixelSize, double trackingEm) const {
    QFont font(fontDisplay);
    setPixelSize(&font, pixelSize);
    font.setWeight(static_cast<QFont::Weight>(fontWeightDisplay));
    font.setLetterSpacing(QFont::AbsoluteSpacing, trackingEm * pixelSize);
    // Tabular figures, and the weight and optical size on their axes
    return deckNumberFont(font);
}

QVector<qreal> dashPattern(StrokePattern pattern, const InstrumentStyle& style) {
    switch (pattern) {
    case StrokePattern::Solid:
        return {};
    case StrokePattern::Data:
        return style.dashData;
    case StrokePattern::Queue:
        return style.dashQueue;
    case StrokePattern::Telemetry:
        return style.dashTelemetry;
    case StrokePattern::Axis:
        return style.dashAxis;
    case StrokePattern::Spoke:
        return style.dashSpoke;
    }
    return {};
}

QPen hairlinePen(const QColor& colour, const InstrumentStyle& style) {
    QPen pen(colour, style.strokeHairline);
    pen.setCosmetic(true);
    return pen;
}

QPen patternPen(StrokePattern pattern,
        const QColor& colour,
        double width,
        const InstrumentStyle& style) {
    QPen pen(colour, width);
    QVector<qreal> dashes = dashPattern(pattern, style);
    if (dashes.isEmpty() || width <= 0) {
        return pen;
    }
    for (qreal& dash : dashes) {
        dash /= width;
    }
    pen.setDashPattern(dashes);
    return pen;
}

QLineF alignToPixels(const QPainter* pPainter, const QLineF& line) {
    const QTransform& transform = pPainter->deviceTransform();
    if (!transform.isInvertible() || transform.isRotating()) {
        return line;
    }
    QLineF device = transform.map(line);
    const auto middle = [](double coordinate) {
        return std::floor(coordinate) + 0.5;
    };
    if (device.y1() == device.y2()) {
        device.setLine(device.x1(), middle(device.y1()), device.x2(), middle(device.y2()));
    } else if (device.x1() == device.x2()) {
        device.setLine(middle(device.x1()), device.y1(), middle(device.x2()), device.y2());
    } else {
        return line;
    }
    return transform.inverted().map(device);
}

void drawHairline(QPainter* pPainter,
        const QLineF& line,
        const QColor& colour,
        const InstrumentStyle& style) {
    pPainter->save();
    pPainter->setPen(hairlinePen(colour, style));
    pPainter->drawLine(alignToPixels(pPainter, line));
    pPainter->restore();
}

QVector<Tick> tickRing(QPointF centre,
        double radius,
        int count,
        int every,
        double longLength,
        double shortLength) {
    QVector<Tick> ticks;
    if (count <= 0) {
        return ticks;
    }
    ticks.reserve(count);
    const int longEvery = std::max(1, every);
    for (int index = 0; index < count; ++index) {
        const QPointF direction = clockwise(360.0 * index / count);
        const bool isLong = index % longEvery == 0;
        const double length = isLong ? longLength : shortLength;
        ticks.append(Tick{QLineF(centre + direction * radius,
                                  centre + direction * (radius + length)),
                isLong});
    }
    return ticks;
}

void paintTickRing(QPainter* pPainter,
        QPointF centre,
        double radius,
        int count,
        int every,
        const QColor& longColour,
        const QColor& shortColour,
        const InstrumentStyle& style) {
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    const QPen longPen = hairlinePen(longColour, style);
    const QPen shortPen = hairlinePen(shortColour, style);
    const QVector<Tick> ticks =
            tickRing(centre, radius, count, every, style.tickLong, style.tickShort);
    for (const Tick& tick : ticks) {
        pPainter->setPen(tick.isLong ? longPen : shortPen);
        pPainter->drawLine(tick.line);
    }
    pPainter->restore();
}

void paintGuideRing(QPainter* pPainter,
        QPointF centre,
        double radius,
        const InstrumentStyle& style) {
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setOpacity(pPainter->opacity() * style.opacityGuide);
    pPainter->setPen(hairlinePen(style.border, style));
    pPainter->setBrush(Qt::NoBrush);
    pPainter->drawEllipse(centre, radius, radius);
    pPainter->restore();
}

void paintGlow(QPainter* pPainter,
        QPointF centre,
        double radius,
        double glowRadius,
        const QColor& colour,
        double alpha) {
    if (glowRadius <= 0 || alpha <= 0) {
        return;
    }
    const double outer = radius + glowRadius;
    QRadialGradient gradient(centre, outer);
    QColor inner = colour;
    inner.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
    QColor clear = colour;
    clear.setAlphaF(0);
    gradient.setColorAt(radius / outer, inner);
    gradient.setColorAt(1, clear);
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(gradient);
    pPainter->drawEllipse(centre, outer, outer);
    pPainter->restore();
}

void paintNode(QPainter* pPainter,
        QPointF centre,
        double radius,
        NodeState state,
        const InstrumentStyle& style,
        bool hovered) {
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    if (state == NodeState::Context) {
        pPainter->setOpacity(pPainter->opacity() * style.opacityContext);
    }
    if (hovered) {
        paintGlow(pPainter,
                centre,
                radius,
                style.glowHoverRadius,
                style.accentHi,
                style.glowHoverAlpha);
    } else if (state == NodeState::Selected) {
        paintGlow(pPainter,
                centre,
                radius,
                style.glowSelectedRadius,
                style.accent,
                style.glowSelectedAlpha);
    }
    QColor fill = style.background;
    QColor stroke = style.textPrimary;
    double width = style.strokeNode;
    switch (state) {
    case NodeState::Rest:
    case NodeState::Context:
        break;
    case NodeState::Active:
        stroke = style.accentHi;
        width = style.strokeNodeActive;
        break;
    case NodeState::Selected:
        fill = style.accent;
        stroke = style.accentHi;
        width = style.strokeSelected;
        break;
    }
    QPen pen(stroke, width);
    pen.setCosmetic(true);
    pPainter->setPen(pen);
    pPainter->setBrush(fill);
    pPainter->drawEllipse(centre, radius, radius);
    pPainter->restore();
}

void paintArrow(QPainter* pPainter,
        QPointF tip,
        QPointF direction,
        double strokeWidth,
        const QColor& colour,
        const InstrumentStyle& style) {
    const double length = std::hypot(direction.x(), direction.y());
    if (length <= 0 || style.arrowMarkerBox <= 0) {
        return;
    }
    // The marker's viewBox, scaled to markerWidth stroke widths
    const double unit = style.arrowMarkerSize * strokeWidth / style.arrowMarkerBox;
    const QPointF along = direction / length;
    const QPointF across(-along.y(), along.x());
    const double back = (style.arrowMarkerTip - style.arrowMarkerBack) * unit;
    const double spread = style.arrowMarkerSpread * unit;
    QPainterPath path(tip - along * back + across * spread);
    path.lineTo(tip);
    path.lineTo(tip - along * back - across * spread);

    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setPen(QPen(colour, style.strokeConnection * unit));
    pPainter->setBrush(Qt::NoBrush);
    pPainter->drawPath(path);
    pPainter->restore();
}

QString monoLabelText(const QStringList& parts) {
    QStringList shown;
    for (const QString& part : parts) {
        if (!part.trimmed().isEmpty()) {
            shown.append(part.trimmed());
        }
    }
    return shown.join(QStringLiteral(" · "));
}

double paintMonoLabel(QPainter* pPainter,
        QPointF baseline,
        const QStringList& parts,
        double pixelSize,
        const QColor& colour,
        const InstrumentStyle& style) {
    const QFont font = style.monoLabelFont(pixelSize);
    const QFontMetricsF metrics(font);
    const QString separator = QStringLiteral("·");
    double x = baseline.x();
    pPainter->save();
    pPainter->setFont(font);
    pPainter->setPen(colour);
    bool first = true;
    for (const QString& part : parts) {
        const QString text = part.trimmed();
        if (text.isEmpty()) {
            continue;
        }
        if (!first) {
            // MonoLabel.tsx: the separator with px-1 on both sides
            x += style.separatorPadding;
            pPainter->drawText(QPointF(x, baseline.y()), separator);
            x += metrics.horizontalAdvance(separator) + style.separatorPadding;
        }
        pPainter->drawText(QPointF(x, baseline.y()), text);
        x += metrics.horizontalAdvance(text);
        first = false;
    }
    pPainter->restore();
    return x - baseline.x();
}

void paintCentredText(QPainter* pPainter,
        QPointF baseline,
        const QString& text,
        const QFont& font,
        const QColor& colour) {
    if (text.isEmpty()) {
        return;
    }
    const QFontMetricsF metrics(font);
    // SVG's text-anchor middle: the advance, tracking included, is centred
    const double width = metrics.horizontalAdvance(text);
    pPainter->save();
    pPainter->setFont(font);
    pPainter->setPen(colour);
    pPainter->drawText(QPointF(baseline.x() - width / 2, baseline.y()), text);
    pPainter->restore();
}

int levelSpan(double value, double minimum) {
    if (!(value > 0)) {
        return 0;
    }
    const double fraction = std::clamp(std::max(value, minimum), 0.0, 1.0);
    // Qt's angles run counter-clockwise: negative is clockwise
    return -static_cast<int>(std::lround(fraction * 360 * 16));
}

void paintLevelRing(QPainter* pPainter,
        QPointF centre,
        double radius,
        double value,
        const QColor& track,
        const QColor& level,
        const InstrumentStyle& style) {
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setBrush(Qt::NoBrush);
    const QRectF circle(centre.x() - radius, centre.y() - radius, 2 * radius, 2 * radius);
    QPen trackPen(track, style.levelRingTrackWidth);
    trackPen.setCosmetic(true);
    pPainter->setPen(trackPen);
    pPainter->drawEllipse(circle);
    const int span = levelSpan(value, style.levelRingMinimum);
    if (span != 0) {
        QPen pen(level, style.levelRingWidth);
        pen.setCosmetic(true);
        pen.setCapStyle(Qt::RoundCap);
        pPainter->setPen(pen);
        // From 12 o'clock, in sixteenths of a degree
        pPainter->drawArc(circle, 90 * 16, span);
    }
    pPainter->restore();
}

} // namespace crate::widgets
