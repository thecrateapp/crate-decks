#include "crate/ui/skin/mixerstyle.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "widget/wslidercomposed.h"
#include "widget/wwidgetgroup.h"

namespace crate {

namespace {

const QString kFaderPrefix = QStringLiteral("CrateFader");
const QString kMeterPrefix = QStringLiteral("CrateMeterVu");
const QString kLanePrefix = QStringLiteral("CrateMeterLane");
/// The group around a fader whose highlight says that it syncs
const QString kSyncGroup = QStringLiteral("CrateFaderSync");
/// How often a falling peak hold repaints, about the meters' own rate
constexpr int kPeakFrameMs = 33;

double number(const SkinVariables& variables, const QString& name) {
    bool ok = false;
    const double value = variables.value(name).toDouble(&ok);
    return ok ? value : 0;
}

QColor colour(const SkinVariables& variables, const QString& name) {
    return QColor(variables.value(name));
}

/// The middle of the handle at `parameter`, on a half pixel
double handleMiddle(int length, double parameter, bool horizontal, const FaderStyle& style) {
    return faderHandleEdge(length, style.handleLength, parameter, horizontal) +
            style.handleLength / 2;
}

/// The rail runs on the half pixel at the middle of the fader's width
double railAt(int across) {
    return std::round(across / 2.0) + 0.5;
}

QPen hairline(const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(1);
    pen.setCapStyle(Qt::FlatCap);
    return pen;
}

/// Fills the handle of a fader while it moves, is dragged or syncs. A child
/// over the fader, so that it paints after the idle handle; it never takes
/// the mouse.
class ActiveHandle : public QWidget {
  public:
    ActiveHandle(WSliderComposed* pSlider, const FaderStyle& style)
            : QWidget(pSlider),
              m_pSlider(pSlider),
              m_style(style) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(pSlider->rect());
    }

    void setActive(bool active) {
        if (active != m_active) {
            m_active = active;
            update();
        }
    }

  protected:
    void paintEvent(QPaintEvent* /*pEvent*/) override {
        if (!m_active || !m_pSlider) {
            return;
        }
        QPainter painter(this);
        const QSize size = m_pSlider->size();
        paintActiveFaderHandle(&painter,
                size,
                size.width() > size.height(),
                m_pSlider->getControlParameterDisplay(),
                m_pSlider->palette().color(QPalette::WindowText),
                m_style);
    }

  private:
    QPointer<WSliderComposed> m_pSlider;
    const FaderStyle m_style;
    bool m_active = false;
};

/// Paints the scale under a fader and fills its handle while it is dragged,
/// for activeHoldMs after it moved (from the mouse, a controller or Auto
/// DJ) and while its sync group is highlighted.
class FaderPainter : public QObject {
  public:
    FaderPainter(WSliderComposed* pSlider, const FaderStyle& style)
            : QObject(pSlider),
              m_pSlider(pSlider),
              m_style(style),
              m_pActiveHandle(new ActiveHandle(pSlider, style)) {
        m_holdTimer.setSingleShot(true);
        m_holdTimer.setInterval(style.activeHoldMs);
        connect(&m_holdTimer, &QTimer::timeout, this, &FaderPainter::updateActive);
        if (auto* pGroup = qobject_cast<WWidgetGroup*>(pSlider->parentWidget());
                pGroup && pGroup->objectName() == kSyncGroup) {
            m_pSyncGroup = pGroup;
            connect(pGroup, &WWidgetGroup::highlightChanged, this, &FaderPainter::updateActive);
        }
        pSlider->installEventFilter(this);
        updateActive();
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pWatched != m_pSlider) {
            return QObject::eventFilter(pWatched, pEvent);
        }
        switch (pEvent->type()) {
        case QEvent::Paint: {
            const double parameter = m_pSlider->getControlParameterDisplay();
            if (m_hasParameter && parameter != m_parameter) {
                m_holdTimer.start();
            }
            m_hasParameter = true;
            m_parameter = parameter;
            QPainter painter(m_pSlider);
            const QSize size = m_pSlider->size();
            paintFaderScale(&painter, size, size.width() > size.height(), m_style);
            painter.end();
            updateActive();
            break;
        }
        case QEvent::MouseButtonPress:
            m_dragging = true;
            updateActive();
            break;
        case QEvent::MouseButtonRelease:
            m_dragging = false;
            m_holdTimer.start();
            updateActive();
            break;
        case QEvent::Resize:
            m_pActiveHandle->setGeometry(m_pSlider->rect());
            break;
        default:
            break;
        }
        return QObject::eventFilter(pWatched, pEvent);
    }

  private:
    void updateActive() {
        const bool synced = m_pSyncGroup && m_pSyncGroup->getHighlight() > 0;
        m_pActiveHandle->setActive(m_dragging || m_holdTimer.isActive() || synced);
    }

    QPointer<WSliderComposed> m_pSlider;
    const FaderStyle m_style;
    ActiveHandle* m_pActiveHandle;
    QPointer<WWidgetGroup> m_pSyncGroup;
    QTimer m_holdTimer;
    double m_parameter = 0;
    bool m_hasParameter = false;
    bool m_dragging = false;
};

/// Paints a WSliderComposed connected to a level as a level meter, with the
/// peak hold falling after peakHoldMs. The meter ignores the mouse, so that
/// nobody sets the level by clicking it.
class MeterPainter : public QObject {
  public:
    MeterPainter(WSliderComposed* pSlider, const LevelMeterStyle& style, bool lane)
            : QObject(pSlider),
              m_pSlider(pSlider),
              m_style(style),
              m_lane(lane) {
        pSlider->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_fallTimer.setInterval(kPeakFrameMs);
        connect(&m_fallTimer, &QTimer::timeout, pSlider, qOverload<>(&QWidget::update));
        m_clock.start();
        pSlider->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pWatched != m_pSlider || pEvent->type() != QEvent::Paint) {
            return QObject::eventFilter(pWatched, pEvent);
        }
        const double level = std::clamp(m_pSlider->getControlParameterDisplay(), 0.0, 1.0);
        const QColor colour = m_pSlider->palette().color(QPalette::WindowText);
        QPainter painter(m_pSlider);
        if (m_lane) {
            paintLevelLane(&painter, m_pSlider->size(), level, colour, m_style);
        } else {
            updatePeak(level);
            paintLevelMeter(&painter, m_pSlider->size(), level, m_peak, colour, m_style);
        }
        // The widget would only paint its (empty) style background
        return true;
    }

  private:
    void updatePeak(double level) {
        const qint64 now = m_clock.elapsed();
        if (level >= m_peak) {
            m_peak = level;
            m_holdUntil = now + m_style.peakHoldMs;
        } else if (now > m_holdUntil) {
            const qint64 from = std::max(m_lastPaint, m_holdUntil);
            m_peak = std::max(level,
                    m_peak - static_cast<double>(now - from) / m_style.peakFallMs);
        }
        m_lastPaint = now;
        if (m_peak > level) {
            if (!m_fallTimer.isActive()) {
                m_fallTimer.start();
            }
        } else {
            m_fallTimer.stop();
        }
    }

    QPointer<WSliderComposed> m_pSlider;
    const LevelMeterStyle m_style;
    const bool m_lane;
    QTimer m_fallTimer;
    QElapsedTimer m_clock;
    double m_peak = 0;
    qint64 m_holdUntil = 0;
    qint64 m_lastPaint = 0;
};

} // namespace

FaderStyle FaderStyle::fromVariables(const SkinVariables& variables) {
    FaderStyle style;
    style.rail = colour(variables, QStringLiteral("CrateFaderRailColor"));
    style.tick = colour(variables, QStringLiteral("CrateFaderTickColor"));
    style.centreTick = colour(variables, QStringLiteral("CrateFaderCentreTickColor"));
    style.activeLine = colour(variables, QStringLiteral("CrateFaderHandleLineActive"));
    style.tickGap = number(variables, QStringLiteral("CrateFaderTickGap"));
    style.tickLong = number(variables, QStringLiteral("CrateFaderTickLong"));
    style.tickShort = number(variables, QStringLiteral("CrateFaderTickShort"));
    const double step = number(variables, QStringLiteral("CrateFaderTickStep"));
    style.tickSteps = step > 0 ? static_cast<int>(std::lround(100 / step)) : 0;
    style.handleWidth = number(variables, QStringLiteral("CrateFaderHandleWidth"));
    style.handleHeight = number(variables, QStringLiteral("CrateFaderHandleHeight"));
    style.handleLength = number(variables, QStringLiteral("CrateFaderHandleLength"));
    style.handleLine = number(variables, QStringLiteral("CrateFaderHandleLine"));
    style.handleRadius = number(variables, QStringLiteral("CrateFaderHandleRadius"));
    style.activeHoldMs = static_cast<int>(
            number(variables, QStringLiteral("CrateFaderActiveHold")));
    return style;
}

int faderHandleEdge(int length, double handleLength, double parameter, bool horizontal) {
    // WSliderComposed: round(parameterToPosition()), with 1 at the top of a
    // vertical fader
    const double travel = std::max(0.0, length - handleLength);
    const double position = horizontal ? parameter : 1 - parameter;
    return static_cast<int>(std::lround(std::clamp(position, 0.0, 1.0) * travel));
}

void paintFaderScale(QPainter* pPainter, QSize size, bool horizontal, const FaderStyle& style) {
    if (!style.isValid()) {
        return;
    }
    const int length = horizontal ? size.width() : size.height();
    const double rail = railAt(horizontal ? size.height() : size.width());
    const auto line = [pPainter, horizontal](double along1,
                              double across1,
                              double along2,
                              double across2) {
        if (horizontal) {
            pPainter->drawLine(QPointF(along1, across1), QPointF(along2, across2));
        } else {
            pPainter->drawLine(QPointF(across1, along1), QPointF(across2, along2));
        }
    };
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setPen(hairline(style.rail));
    // From the handle's middle at one end to the other
    line(handleMiddle(length, horizontal ? 0 : 1, horizontal, style),
            rail,
            handleMiddle(length, horizontal ? 1 : 0, horizontal, style),
            rail);
    const double gap = style.handleWidth / 2 + style.tickGap;
    for (int step = 0; step <= style.tickSteps; ++step) {
        const bool centre = 2 * step == style.tickSteps;
        const double tickLength = centre ? style.tickLong : style.tickShort;
        const double along = handleMiddle(
                length, static_cast<double>(step) / style.tickSteps, horizontal, style);
        pPainter->setPen(hairline(centre ? style.centreTick : style.tick));
        line(along, rail - gap - tickLength, along, rail - gap);
        line(along, rail + gap, along, rail + gap + tickLength);
    }
    pPainter->restore();
}

void paintActiveFaderHandle(QPainter* pPainter,
        QSize size,
        bool horizontal,
        double parameter,
        const QColor& colour,
        const FaderStyle& style) {
    if (!style.isValid() || !colour.isValid()) {
        return;
    }
    const int length = horizontal ? size.width() : size.height();
    const double rail = railAt(horizontal ? size.height() : size.width());
    const double edge = faderHandleEdge(length, style.handleLength, parameter, horizontal);
    const double middle = edge + style.handleLength / 2;
    // The outline of the idle handle: a hairline on half pixels
    QRectF handle(edge + 0.5, rail - style.handleWidth / 2, style.handleHeight, style.handleWidth);
    QLineF centre(middle, rail - style.handleLine / 2, middle, rail + style.handleLine / 2);
    if (!horizontal) {
        handle = handle.transposed();
        handle.moveTo(handle.y(), handle.x());
        centre = QLineF(centre.y1(), centre.x1(), centre.y2(), centre.x2());
    }
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setPen(hairline(colour));
    pPainter->setBrush(colour);
    pPainter->drawRoundedRect(handle, style.handleRadius, style.handleRadius);
    pPainter->setPen(hairline(style.activeLine));
    pPainter->drawLine(centre);
    pPainter->restore();
}

LevelMeterStyle LevelMeterStyle::fromVariables(const SkinVariables& variables) {
    LevelMeterStyle style;
    style.track = colour(variables, QStringLiteral("CrateVuTrack"));
    style.warning = colour(variables, QStringLiteral("CrateVuWarning"));
    style.warningRule = colour(variables, QStringLiteral("CrateVuWarningRule"));
    style.laneTrack = colour(variables, QStringLiteral("CrateLaneTrack"));
    style.warningLevel = number(variables, QStringLiteral("CrateVuWarningLevel"));
    style.topGap = static_cast<int>(number(variables, QStringLiteral("CrateVuClipGap")));
    style.peakHold = static_cast<int>(number(variables, QStringLiteral("CrateVuPeakHold")));
    style.peakHoldMs = static_cast<int>(number(variables, QStringLiteral("CrateVuPeakHoldTime")));
    style.peakFallMs = static_cast<int>(number(variables, QStringLiteral("CrateVuPeakFallTime")));
    return style;
}

void paintLevelMeter(QPainter* pPainter,
        QSize size,
        double level,
        double peak,
        const QColor& colour,
        const LevelMeterStyle& style) {
    const int width = size.width();
    const int height = size.height();
    // Full scale is topGap under the top, as the mockups leave room between
    // the bar and the clip segment
    const int scale = std::max(0, height - style.topGap);
    const auto top = [height, scale](double value) {
        return height - static_cast<int>(std::lround(scale * std::clamp(value, 0.0, 1.0)));
    };
    pPainter->fillRect(QRect(0, 0, width, height), style.track);
    const int levelTop = top(level);
    const int warningTop = top(style.warningLevel);
    const int baseTop = std::max(levelTop, warningTop);
    pPainter->fillRect(QRect(0, baseTop, width, height - baseTop), colour);
    if (levelTop < warningTop) {
        pPainter->fillRect(QRect(0, levelTop, width, warningTop - levelTop), style.warning);
    }
    pPainter->fillRect(QRect(0, warningTop, width, 1), style.warningRule);
    if (peak > 0 && style.peakHold > 0) {
        const int peakTop = std::clamp(top(peak), 0, std::max(0, height - style.peakHold));
        pPainter->fillRect(QRect(0, peakTop, width, style.peakHold),
                peak > style.warningLevel ? style.warning : colour);
    }
}

void paintLevelLane(QPainter* pPainter,
        QSize size,
        double level,
        const QColor& colour,
        const LevelMeterStyle& style) {
    pPainter->fillRect(QRect(QPoint(0, 0), size), style.laneTrack);
    const int lit = static_cast<int>(std::lround(size.width() * std::clamp(level, 0.0, 1.0)));
    pPainter->fillRect(QRect(0, 0, lit, size.height()), colour);
}

void styleMixer(QWidget* pSkinRoot, const SkinVariables& variables) {
    if (!pSkinRoot) {
        return;
    }
    const FaderStyle faderStyle = FaderStyle::fromVariables(variables);
    const LevelMeterStyle meterStyle = LevelMeterStyle::fromVariables(variables);
    const QList<WSliderComposed*> sliders = pSkinRoot->findChildren<WSliderComposed*>();
    for (WSliderComposed* pSlider : sliders) {
        const QString name = pSlider->objectName();
        if (name.startsWith(kFaderPrefix) && faderStyle.isValid()) {
            new FaderPainter(pSlider, faderStyle);
        } else if (name.startsWith(kMeterPrefix) && meterStyle.isValid()) {
            new MeterPainter(pSlider, meterStyle, false);
        } else if (name.startsWith(kLanePrefix) && meterStyle.isValid()) {
            new MeterPainter(pSlider, meterStyle, true);
        }
    }
}

} // namespace crate
