#pragma once

#include <QWidget>

#include "crate/ui/widgets/instrumentpainter.h"

namespace crate::widgets {

/// StackMap's level ring (stack-map.css .stack-map__level) as Decks uses it
/// for download progress and the coverage of an offline set (design 14.3,
/// GU1 mockups decks.js levelRing): a hairline track of levelRingRadius in
/// border and, from 12 o'clock clockwise, an arc in accentHi of
/// levelRingWidth with round caps, proportional to the value and never
/// shorter than levelRingMinimum. It repaints only when its value changes.
class LevelRing : public QWidget {
    Q_OBJECT
  public:
    explicit LevelRing(const InstrumentStyle& style = InstrumentStyle::installed(),
            QWidget* pParent = nullptr);

    /// From 0 (nothing, no arc) to 1 (full).
    void setValue(double value);
    double value() const {
        return m_value;
    }

    /// The arc, in Qt's sixteenths of a degree (negative is clockwise).
    int spanAngle() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override {
        return sizeHint();
    }

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    const InstrumentStyle m_style;
    double m_value = 0;
};

} // namespace crate::widgets
