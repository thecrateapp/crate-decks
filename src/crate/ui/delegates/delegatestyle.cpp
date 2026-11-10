#include "crate/ui/delegates/delegatestyle.h"

#include <QFontMetrics>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QRect>
#include <QSvgRenderer>
#include <algorithm>
#include <cmath>

namespace crate::delegates {

namespace {

// QSS and the generator write fractional font sizes in points, at Qt's
// 96 dpi reference; the delegates do the same so that both agree.
constexpr double kPointsPerPixel = 72.0 / 96.0;

QColor colour(const SkinVariables& variables, const QString& name) {
    return QColor(variables.value(name));
}

double number(const SkinVariables& variables, const QString& name) {
    bool ok = false;
    const double value = variables.value(name).toDouble(&ok);
    return ok ? value : 0;
}

int whole(const SkinVariables& variables, const QString& name) {
    return static_cast<int>(std::lround(number(variables, name)));
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

} // namespace

DelegateStyle DelegateStyle::fromVariables(const SkinVariables& variables) {
    DelegateStyle style;
    style.background = colour(variables, QStringLiteral("CrateBgApp"));
    style.textPrimary = colour(variables, QStringLiteral("CrateTextPrimary"));
    style.textSecondary = colour(variables, QStringLiteral("CrateTextSecondary"));
    style.textMuted = colour(variables, QStringLiteral("CrateTextMuted"));
    style.textSubtle = colour(variables, QStringLiteral("CrateTextSubtle"));
    style.textFaint = colour(variables, QStringLiteral("CrateTextFaint"));
    style.navIdle = colour(variables, QStringLiteral("CrateNavIdle"));
    style.accentHi = colour(variables, QStringLiteral("CrateAccentHi"));
    style.accentHiBorder = colour(variables, QStringLiteral("CrateAccentHiBorder"));
    style.warning = colour(variables, QStringLiteral("CrateWarning"));
    style.warningText = colour(variables, QStringLiteral("CrateWarningText"));
    style.border = colour(variables, QStringLiteral("CrateBorder"));
    style.borderSubtle = colour(variables, QStringLiteral("CrateBorderSubtle"));
    style.rowHover = colour(variables, QStringLiteral("CrateRowHover"));
    style.rowSelected = colour(variables, QStringLiteral("CrateRowSelected"));
    style.deck[0] = colour(variables, QStringLiteral("CrateDeckA"));
    style.deck[1] = colour(variables, QStringLiteral("CrateDeckB"));
    style.deck[2] = colour(variables, QStringLiteral("CrateDeckC"));
    style.deck[3] = colour(variables, QStringLiteral("CrateDeckD"));

    style.fontBody = variables.value(QStringLiteral("CrateFontBody"));
    style.fontMono = variables.value(QStringLiteral("CrateFontMono"));
    style.fontSizeText = number(variables, QStringLiteral("CrateLibFontSizeText"));
    style.fontSizeMeta = number(variables, QStringLiteral("CrateLibFontSizeMeta"));
    style.fontSizeHeader = number(variables, QStringLiteral("CrateLibFontSizeHeader"));
    style.fontSizeMark = number(variables, QStringLiteral("CrateLibFontSizeMark"));
    style.fontSizeNav = number(variables, QStringLiteral("CrateLibFontSizeNav"));
    style.trackingCaps = number(variables, QStringLiteral("CrateTrackingCaps"));
    style.lineHeightLabel = number(variables, QStringLiteral("CrateLineHeightLabel"));

    style.rowHeight = whole(variables, QStringLiteral("CrateLibRowHeight"));
    style.headerHeight = whole(variables, QStringLiteral("CrateLibHeaderHeight"));
    style.rule = whole(variables, QStringLiteral("CrateRule"));
    style.smallIcon = whole(variables, QStringLiteral("CrateLibSmallIcon"));
    style.cellPadding = whole(variables, QStringLiteral("CrateLibCellPadding"));
    style.deckMarkWidth = whole(variables, QStringLiteral("CrateLibDeckMarkWidth"));
    style.activeBar = whole(variables, QStringLiteral("CrateActiveBarWidth"));
    style.dot = whole(variables, QStringLiteral("CrateStatusDot"));
    style.stateGap = whole(variables, QStringLiteral("CrateLibStateGap"));
    style.chipHeight = whole(variables, QStringLiteral("CrateLibChipHeight"));
    style.chipPadding = whole(variables, QStringLiteral("CrateLibChipPadding"));
    style.ringRadius = number(variables, QStringLiteral("CrateLevelRingRadius"));
    style.ringWidth = number(variables, QStringLiteral("CrateLevelRingWidth"));
    style.ringTrackWidth = number(variables, QStringLiteral("CrateLevelRingTrackWidth"));
    style.ringMinimum = number(variables, QStringLiteral("CrateLevelRingMinimum"));

    style.sidebarItemInset = whole(variables, QStringLiteral("CrateSidebarItemInset"));
    style.sidebarItemPadding = whole(variables, QStringLiteral("CrateSidebarItemPadding"));
    style.sidebarIndent = whole(variables, QStringLiteral("CrateSidebarIndent"));
    style.sidebarIcon = whole(variables, QStringLiteral("CrateSidebarIcon"));
    style.sidebarIconGap = whole(variables, QStringLiteral("CrateSidebarIconGap"));
    style.sidebarBarInset = whole(variables, QStringLiteral("CrateSidebarBarInset"));
    style.sidebarSectionTop = whole(variables, QStringLiteral("CrateSidebarSectionTop"));
    style.sidebarSectionX = whole(variables, QStringLiteral("CrateSidebarSectionX"));
    style.sidebarSectionBottom = whole(variables, QStringLiteral("CrateSidebarSectionBottom"));
    return style;
}

QColor DelegateStyle::deckColour(const QString& letter) const {
    if (letter.size() != 1) {
        return QColor();
    }
    const int deckIndex = letter.at(0).unicode() - u'A';
    if (deckIndex < 0 || deckIndex > 3) {
        return QColor();
    }
    return deck[deckIndex];
}

QFont DelegateStyle::bodyFont(double pixelSize) const {
    QFont font(fontBody);
    setPixelSize(&font, pixelSize);
    return font;
}

QFont DelegateStyle::monoFont(double pixelSize) const {
    QFont font(fontMono);
    setPixelSize(&font, pixelSize);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    // Font features came with Qt 6.7
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    return font;
}

QFont DelegateStyle::monoLabelFont(double pixelSize) const {
    QFont font = monoFont(pixelSize);
    font.setCapitalization(QFont::AllUppercase);
    font.setLetterSpacing(QFont::AbsoluteSpacing, trackingCaps * pixelSize);
    return font;
}

int DelegateStyle::monoLabelLineHeight(double pixelSize) const {
    return static_cast<int>(std::lround(pixelSize * lineHeightLabel));
}

void paintDot(QPainter* pPainter, QPointF centre, double diameter, const QColor& colour) {
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(colour);
    pPainter->drawEllipse(centre, diameter / 2, diameter / 2);
    pPainter->restore();
}

void paintLevelRing(QPainter* pPainter,
        QPointF centre,
        double value,
        const QColor& track,
        const QColor& level,
        const DelegateStyle& style) {
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setBrush(Qt::NoBrush);
    const double radius = style.ringRadius;
    const QRectF circle(centre.x() - radius, centre.y() - radius, 2 * radius, 2 * radius);
    pPainter->setPen(QPen(track, style.ringTrackWidth));
    pPainter->drawEllipse(circle);
    if (value > 0) {
        const double fraction = std::clamp(std::max(value, style.ringMinimum), 0.0, 1.0);
        QPen pen(level, style.ringWidth);
        pen.setCapStyle(Qt::RoundCap);
        pPainter->setPen(pen);
        // From the top, clockwise: Qt's angles run counter-clockwise from 3
        // o'clock, in sixteenths of a degree
        pPainter->drawArc(circle, 90 * 16, -static_cast<int>(std::lround(fraction * 360 * 16)));
    }
    pPainter->restore();
}

void paintText(QPainter* pPainter,
        const QRect& rect,
        const QString& text,
        const QFont& font,
        const QColor& colour,
        Qt::Alignment alignment) {
    if (text.isEmpty() || rect.width() <= 0) {
        return;
    }
    pPainter->save();
    pPainter->setFont(font);
    pPainter->setPen(colour);
    const QFontMetrics metrics(font);
    const QString elided = metrics.elidedText(text, Qt::ElideRight, rect.width());
    pPainter->drawText(rect, static_cast<int>(alignment | Qt::TextSingleLine), elided);
    pPainter->restore();
}

QString groupThousands(const QString& digits) {
    QString grouped;
    for (int i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            grouped.append(u' ');
        }
        grouped.append(digits.at(i));
    }
    return grouped;
}

void paintIcon(QPainter* pPainter,
        const QRect& rect,
        const QString& svgPath,
        const QColor& colour) {
    if (svgPath.isEmpty() || rect.isEmpty()) {
        return;
    }
    // Cached by path, size, colour and pixel ratio: a sidebar paints a dozen
    // of them on every hover
    static QHash<QString, QPixmap> s_cache;
    const qreal ratio = pPainter->device() ? pPainter->device()->devicePixelRatioF() : 1.0;
    const QString key = QStringLiteral("%1|%2x%3|%4|%5")
                                .arg(svgPath)
                                .arg(rect.width())
                                .arg(rect.height())
                                .arg(colour.name(QColor::HexArgb))
                                .arg(ratio);
    const auto cached = s_cache.constFind(key);
    if (cached != s_cache.constEnd()) {
        pPainter->drawPixmap(rect.topLeft(), *cached);
        return;
    }
    QSvgRenderer renderer(svgPath);
    QPixmap pixmap(rect.size() * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    if (renderer.isValid()) {
        QPainter iconPainter(&pixmap);
        iconPainter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&iconPainter, QRectF(QPointF(0, 0), QSizeF(rect.size())));
        // The icon's own colour becomes `colour`, its alpha stays
        iconPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        iconPainter.fillRect(QRectF(QPointF(0, 0), QSizeF(rect.size())), colour);
    }
    s_cache.insert(key, pixmap);
    pPainter->drawPixmap(rect.topLeft(), pixmap);
}

} // namespace crate::delegates
