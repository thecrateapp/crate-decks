#pragma once

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QString>

#include "crate/ui/skin/crateskin.h"

class QPainter;
class QRect;

namespace crate::delegates {

/// The look of the library's cells and sidebar in the CrateDecks skin
/// (design 14.3, plan V2.PR4, GU1 mockups decks.css .tracks and .side). Every
/// value comes from the skin's generated/vars.xml, that is, from
/// res/crate-design/tokens.json: the delegates write no colour or size of
/// their own.
struct DelegateStyle {
    QColor background;
    // Text
    QColor textPrimary;
    QColor textSecondary;
    QColor textMuted;
    QColor textSubtle;
    QColor textFaint;
    QColor navIdle;
    // States
    QColor accentHi;
    QColor accentHiBorder;
    QColor warning;
    QColor warningText;
    // Rules and rows
    QColor border;
    QColor borderSubtle;
    QColor rowHover;
    QColor rowSelected;
    /// Deck colours, A to D (design 14.3)
    QColor deck[4];

    QString fontBody;
    QString fontMono;
    double fontSizeText = 0;   // titles and names in rows
    double fontSizeMeta = 0;   // BPM, key and duration in rows
    double fontSizeHeader = 0; // MonoLabel column headers and chips
    double fontSizeMark = 0;   // deck letters, counts, "NO PROFILE"
    double fontSizeNav = 0;    // MonoLabel of the sidebar
    double trackingCaps = 0;   // em
    double lineHeightLabel = 0;

    int rowHeight = 0;
    int headerHeight = 0;
    int rule = 0;
    int smallIcon = 0;
    int cellPadding = 0;
    int deckMarkWidth = 0;
    int activeBar = 0;
    int dot = 0;
    int stateGap = 0;
    int chipHeight = 0;
    int chipPadding = 0;
    double ringRadius = 0;
    double ringWidth = 0;
    double ringTrackWidth = 0;
    double ringMinimum = 0;

    // Sidebar (decks.css .side)
    int sidebarItemInset = 0;
    int sidebarItemPadding = 0;
    int sidebarIndent = 0;
    int sidebarIcon = 0;
    int sidebarIconGap = 0;
    int sidebarBarInset = 0;
    int sidebarSectionTop = 0;
    int sidebarSectionX = 0;
    int sidebarSectionBottom = 0;

    bool isValid() const {
        return rowHeight > 0 && textPrimary.isValid() && !fontMono.isEmpty();
    }
    static DelegateStyle fromVariables(const SkinVariables& variables);

    /// The colour of deck `letter` ("A" to "D"); invalid for anything else.
    QColor deckColour(const QString& letter) const;

    /// The body font (Roboto) at `pixelSize`.
    QFont bodyFont(double pixelSize) const;
    /// The mono font (JetBrains Mono) at `pixelSize`, with tabular figures.
    QFont monoFont(double pixelSize) const;
    /// A MonoLabel: mono, capitals and the caps tracking.
    QFont monoLabelFont(double pixelSize) const;
    /// Height of a MonoLabel line of `pixelSize`, as MonoLabel's line-height.
    int monoLabelLineHeight(double pixelSize) const;
};

/// A dot of `diameter` centred on `centre` (decks.css .dot).
void paintDot(QPainter* pPainter, QPointF centre, double diameter, const QColor& colour);

/// StackMap's level ring (stack-map.css .stack-map__level): a 1 px track of
/// `radius` and, from the top clockwise, an arc of `value` (0 to 1, never
/// shorter than style.ringMinimum) with round caps.
void paintLevelRing(QPainter* pPainter,
        QPointF centre,
        double value,
        const QColor& track,
        const QColor& level,
        const DelegateStyle& style);

/// `text` in `font` and `colour` inside `rect`, elided at the right.
void paintText(QPainter* pPainter,
        const QRect& rect,
        const QString& text,
        const QFont& font,
        const QColor& colour,
        Qt::Alignment alignment);

/// `digits` with its thousands apart, as the admin writes counts ("136 512").
QString groupThousands(const QString& digits);

/// The Solar icon at `svgPath` (one of the skin's generated icons) in
/// `colour`, fitted in `rect`. The tinted pixmaps are cached.
void paintIcon(QPainter* pPainter, const QRect& rect, const QString& svgPath, const QColor& colour);

} // namespace crate::delegates
