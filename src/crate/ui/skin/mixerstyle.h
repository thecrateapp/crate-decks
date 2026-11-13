#pragma once

#include <QColor>
#include <QSize>

#include "crate/ui/skin/crateskin.h"

class QPainter;
class QWidget;

namespace crate {

/// The faders of the CrateDecks skin (design 14.3, GU1 mockups faderSvg): a
/// 1 px rail and a tick every faderTickStep percent on both sides of the
/// handle, the centre one longer, and an outline handle that fills with the
/// fader's colour while it moves or syncs. WSliderComposed draws the idle
/// handle from fader-handle-*.svg; the skin stretches its faders, so the
/// scale is painted here at the real length, on whole pixels, and the ticks
/// stay as crisp as in the mockups. All values come from generated/vars.xml.
struct FaderStyle {
    QColor rail;
    QColor tick;
    QColor centreTick;
    QColor activeLine;
    double tickGap = 0;
    double tickLong = 0;
    double tickShort = 0;
    int tickSteps = 0;
    double handleWidth = 0;
    double handleHeight = 0;
    double handleLength = 0;
    double handleLine = 0;
    double handleRadius = 0;
    int activeHoldMs = 0;

    bool isValid() const {
        return tickSteps > 0 && handleLength > 0 && rail.isValid();
    }
    static FaderStyle fromVariables(const SkinVariables& variables);
};

/// Where WSliderComposed draws the handle of `parameter` on a fader
/// `length` long: the handle's top (vertical) or left edge, in whole pixels.
/// A parameter of 1 is the top of a vertical fader and the right of a
/// horizontal one.
int faderHandleEdge(int length, double handleLength, double parameter, bool horizontal);

/// Paints the rail and ticks of a fader of `size`. The ticks sit where the
/// middle of the handle is at 0, 10, ... 100 %.
void paintFaderScale(QPainter* pPainter, QSize size, bool horizontal, const FaderStyle& style);

/// Paints the handle at `parameter` filled with `colour`, as a fader that
/// moves or syncs shows it, over the idle handle.
void paintActiveFaderHandle(QPainter* pPainter,
        QSize size,
        bool horizontal,
        double parameter,
        const QColor& colour,
        const FaderStyle& style);

/// The level meters of the CrateDecks skin (design 14.3, GU1 mockups vuSvg):
/// a bar in the meter's colour up to the warning level, `warning` above it,
/// a 1 px rule of the background at the warning level and a 1 px peak hold.
/// The clip segment above the bar is a skin group lit by peak_indicator;
/// full scale sits topGap under it.
struct LevelMeterStyle {
    QColor track;
    QColor warning;
    QColor warningRule;
    QColor laneTrack;
    double warningLevel = 1;
    int topGap = 0;
    int peakHold = 1;
    int peakHoldMs = 0;
    int peakFallMs = 0;

    bool isValid() const {
        return track.isValid() && warning.isValid() && peakFallMs > 0;
    }
    static LevelMeterStyle fromVariables(const SkinVariables& variables);
};

/// Paints a vertical level meter of `size` at `level` with its peak hold at
/// `peak` (both on upstream's meter scale, from 0 to 1) in `colour`.
void paintLevelMeter(QPainter* pPainter,
        QSize size,
        double level,
        double peak,
        const QColor& colour,
        const LevelMeterStyle& style);

/// Paints a horizontal 1 px lane at `level`, as the toolbar readings.
void paintLevelLane(QPainter* pPainter,
        QSize size,
        double level,
        const QColor& colour,
        const LevelMeterStyle& style);

/// Gives the faders and meters of a loaded CrateDecks skin their painters.
/// Every WSliderComposed whose object name starts with "CrateFader" gets the
/// scale and the filled handle; it shows as synced while its parent group's
/// highlight is 1. Every one that starts with "CrateMeterVu" is drawn as a
/// level meter, and "CrateMeterLane" as a toolbar lane; meters ignore the
/// mouse. The colour of each is its QSS color.
void styleMixer(QWidget* pSkinRoot, const SkinVariables& variables);

} // namespace crate
