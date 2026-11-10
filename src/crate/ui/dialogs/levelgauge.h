#pragma once

#include <QWidget>

namespace crate {

/// The cache's level ring of the Crate page (GU1 mockups preferences-crate):
/// StackMap's level (stack-map.css .stack-map__level), a hairline ring with
/// an arc in accentHi from the top, clockwise, for the value, inside a scale
/// of 48 ticks with a long one every four. Sizes and colours come from the
/// dialog tokens. V3.PR1's LevelRing widget can take its place.
class LevelGauge : public QWidget {
    Q_OBJECT
  public:
    explicit LevelGauge(QWidget* pParent = nullptr);

    /// From 0 (empty) to 1 (full).
    void setValue(double value);
    double value() const {
        return m_value;
    }

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    double m_value = 0;
};

} // namespace crate
