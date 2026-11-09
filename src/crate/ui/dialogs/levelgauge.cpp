#include "crate/ui/dialogs/levelgauge.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <numbers>

#include "crate/ui/dialogs/dialogstyle.h"
#include "moc_levelgauge.cpp"

namespace crate {

LevelGauge::LevelGauge(QWidget* pParent)
        : QWidget(pParent) {
    setObjectName(QStringLiteral("CrateLevelGauge"));
    const int size = DialogMetrics::installed().px("cacheGaugeSize");
    setFixedSize(size, size);
}

void LevelGauge::setValue(double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    if (clamped != m_value) {
        m_value = clamped;
        update();
    }
}

QSize LevelGauge::sizeHint() const {
    const int size = DialogMetrics::installed().px("cacheGaugeSize");
    return QSize(size, size);
}

void LevelGauge::paintEvent(QPaintEvent* /*pEvent*/) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    const QColor border = metrics.colour("CrateBorder");
    const QColor faint = metrics.colour("CrateTextFaint");
    const QColor level = metrics.colour("CrateAccentHi");
    const double radius = metrics.px("cacheGaugeRadius");
    const double tickStart = radius + metrics.px("cacheGaugeTickGap");
    const int ticks = metrics.px("tickCount");
    const int every = std::max(1, metrics.px("tickEvery"));
    const double hairline = metrics.number("strokeHairline");
    const QPointF centre(width() / 2.0, height() / 2.0);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(border, hairline);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(centre, radius, radius);

    for (int tick = 0; tick < ticks; ++tick) {
        const double angle = 2 * std::numbers::pi * tick / ticks;
        const bool isLong = tick % every == 0;
        const double length = metrics.px(isLong ? "cacheGaugeTickLong" : "cacheGaugeTickShort");
        pen.setColor(isLong ? faint : border);
        painter.setPen(pen);
        const QPointF direction(std::cos(angle), std::sin(angle));
        painter.drawLine(centre + direction * tickStart, centre + direction * (tickStart + length));
    }

    if (m_value <= 0) {
        return;
    }
    // stack-map.css .stack-map__level: 1.5 px, round caps, from the top
    QPen arc(level, metrics.number("strokeActive"));
    arc.setCapStyle(Qt::RoundCap);
    painter.setPen(arc);
    const QRectF ring(centre.x() - radius, centre.y() - radius, 2 * radius, 2 * radius);
    painter.drawArc(ring, 90 * 16, -static_cast<int>(std::lround(m_value * 360 * 16)));
}

} // namespace crate
