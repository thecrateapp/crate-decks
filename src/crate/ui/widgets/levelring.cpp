#include "crate/ui/widgets/levelring.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

#include "moc_levelring.cpp"

namespace crate::widgets {

LevelRing::LevelRing(const InstrumentStyle& style, QWidget* pParent)
        : QWidget(pParent),
          m_style(style) {
    setObjectName(QStringLiteral("CrateLevelRing"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void LevelRing::setValue(double value) {
    const double clamped = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
    if (clamped == m_value) {
        return;
    }
    m_value = clamped;
    update();
}

int LevelRing::spanAngle() const {
    return levelSpan(m_value, m_style.levelRingMinimum);
}

QSize LevelRing::sizeHint() const {
    // The mockup's box: the ring and its inset on each side
    const int side = static_cast<int>(
            std::ceil(2 * (m_style.levelRingRadius + m_style.levelRingInset)));
    return QSize(side, side);
}

void LevelRing::paintEvent(QPaintEvent* /*pEvent*/) {
    QPainter painter(this);
    paintLevelRing(&painter,
            QRectF(rect()).center(),
            m_style.levelRingRadius,
            m_value,
            m_style.border,
            m_style.accentHi,
            m_style);
}

} // namespace crate::widgets
