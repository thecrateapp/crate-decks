#include "crate/ui/widgets/camelotlegend.h"

#include <QFontMetricsF>
#include <QPainter>
#include <algorithm>
#include <cmath>

#include "moc_camelotlegend.cpp"

namespace crate::widgets {

CamelotLegend::CamelotLegend(const InstrumentStyle& style, QWidget* pParent)
        : QWidget(pParent),
          m_style(style) {
    setObjectName(QStringLiteral("CrateCamelotLegend"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    //: Title of the Camelot wheel's legend
    m_title = tr("Key");
    m_items = {
            //: Legend of the Camelot wheel: the key of the deck on air
            {Sample::Master, tr("Master deck")},
            //: Legend of the Camelot wheel: the edges to the keys that mix
            {Sample::Compatible, tr("Compatible: ±1, relative, energy +7")},
            //: Legend of the Camelot wheel: the spokes between a key and its relative
            {Sample::Relative, tr("Relative keys")},
            //: Legend of the Camelot wheel: a key with tracks in the list
            {Sample::InResults, tr("In results")},
            //: Legend of the Camelot wheel: a key without tracks in the list
            {Sample::NoResults, tr("No results")},
    };
    m_note = tr(
            "Click a key to filter the list by it. The wheel follows the master deck, or the "
            "selected track.");
}

double CamelotLegend::itemWidth(const Item& item) const {
    const QFontMetricsF metrics(m_style.monoFont(
            m_style.fontSizeWheelLegend, m_style.trackingWheelLabel));
    const double sample = item.sample == Sample::Compatible || item.sample == Sample::Relative
            ? m_style.wheelLegendLine
            : m_style.wheelLegendGlyph;
    return sample + m_style.wheelLegendItemGap + metrics.horizontalAdvance(item.text);
}

QRectF CamelotLegend::noteRect(double top, int width) const {
    const QFontMetricsF metrics(m_style.monoFont(
            m_style.fontSizeWheelLegend, m_style.trackingWheelLabel));
    const QRectF bounds = metrics.boundingRect(
            QRectF(0, 0, std::max(1, width), 1e6), Qt::TextWordWrap, m_note);
    return QRectF(0, top, std::max(1, width), std::ceil(bounds.height()));
}

QVector<QRectF> CamelotLegend::layout(int width) const {
    // A row of flex items with `gap: rowGap gap`: an item that does not fit
    // starts the next row; the note takes a row of its own
    const double line = std::max(m_style.wheelLegendGlyph,
            QFontMetricsF(m_style.monoFont(m_style.fontSizeWheelLegend, m_style.trackingWheelLabel))
                    .height());
    QVector<QRectF> rects;
    const QFontMetricsF titleMetrics(m_style.monoLabelFont(m_style.fontSizeWheelLegend));
    double x = titleMetrics.horizontalAdvance(m_title);
    double y = 0;
    rects.append(QRectF(0, 0, x, line));
    for (const Item& item : m_items) {
        const double itemW = itemWidth(item);
        if (x > 0 && x + m_style.wheelLegendGap + itemW > width) {
            x = 0;
            y += line + m_style.wheelLegendRowGap;
        } else if (x > 0) {
            x += m_style.wheelLegendGap;
        }
        rects.append(QRectF(x, y, itemW, line));
        x += itemW;
    }
    rects.append(noteRect(y + line + m_style.wheelLegendRowGap, width));
    return rects;
}

int CamelotLegend::heightForWidth(int width) const {
    return static_cast<int>(std::ceil(layout(width).last().bottom()));
}

QSize CamelotLegend::sizeHint() const {
    // As wide as its items in one row
    double width = QFontMetricsF(m_style.monoLabelFont(m_style.fontSizeWheelLegend))
                           .horizontalAdvance(m_title);
    for (const Item& item : m_items) {
        width += m_style.wheelLegendGap + itemWidth(item);
    }
    const int whole = static_cast<int>(std::ceil(width));
    return QSize(whole, heightForWidth(whole));
}

QSize CamelotLegend::minimumSizeHint() const {
    double widest = 0;
    for (const Item& item : m_items) {
        widest = std::max(widest, itemWidth(item));
    }
    const int whole = static_cast<int>(std::ceil(widest));
    return QSize(whole, heightForWidth(whole));
}

void CamelotLegend::paintSample(QPainter* pPainter, Sample sample, const QRectF& box) const {
    // On a pixel's middle, so that the lines' core pixels are whole
    const QPointF centre(box.center().x(), std::floor(box.center().y()) + 0.5);
    switch (sample) {
    case Sample::Master:
        paintNode(pPainter, centre, m_style.wheelLegendNode + 1, NodeState::Selected, m_style);
        break;
    case Sample::InResults:
    case Sample::NoResults:
        // A key without results is the same node, its whole item at the
        // context opacity
        paintNode(pPainter, centre, m_style.wheelLegendNode, NodeState::Rest, m_style);
        break;
    case Sample::Compatible: {
        // An active edge with its arrow
        const QLineF line(box.left(), centre.y(), box.right(), centre.y());
        pPainter->save();
        pPainter->setRenderHint(QPainter::Antialiasing);
        QPen pen(m_style.accentHi, m_style.strokeActive);
        pen.setCosmetic(true);
        pPainter->setPen(pen);
        pPainter->drawLine(line);
        pPainter->restore();
        paintArrow(pPainter,
                line.p2(),
                line.p2() - line.p1(),
                m_style.strokeActive,
                m_style.accentHi,
                m_style);
        break;
    }
    case Sample::Relative: {
        // The wheel's dotted spokes, brighter to be seen this small
        QPen pen = patternPen(
                StrokePattern::Spoke, m_style.textMuted, m_style.strokeActive, m_style);
        pen.setCapStyle(Qt::RoundCap);
        pen.setCosmetic(true);
        pPainter->save();
        pPainter->setRenderHint(QPainter::Antialiasing);
        pPainter->setPen(pen);
        pPainter->drawLine(QLineF(box.left(), centre.y(), box.right(), centre.y()));
        pPainter->restore();
        break;
    }
    case Sample::None:
        break;
    }
}

void CamelotLegend::paintEvent(QPaintEvent* /*pEvent*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QVector<QRectF> rects = layout(width());
    const QFont font = m_style.monoFont(m_style.fontSizeWheelLegend, m_style.trackingWheelLabel);

    painter.setFont(m_style.monoLabelFont(m_style.fontSizeWheelLegend));
    painter.setPen(m_style.accentHi);
    painter.drawText(rects.first(), Qt::AlignLeft | Qt::AlignVCenter, m_title);

    painter.setFont(font);
    for (int index = 0; index < m_items.size(); ++index) {
        const Item& item = m_items.at(index);
        const QRectF rect = rects.at(index + 1);
        const double sampleWidth =
                item.sample == Sample::Compatible || item.sample == Sample::Relative
                ? m_style.wheelLegendLine
                : m_style.wheelLegendGlyph;
        const QRectF sample(rect.left(), rect.top(), sampleWidth, rect.height());
        painter.save();
        if (item.sample == Sample::NoResults) {
            // The whole item at the context opacity, as the mockup's
            painter.setOpacity(m_style.opacityContext);
        }
        paintSample(&painter, item.sample, sample);
        painter.setPen(m_style.textMuted);
        painter.setFont(font);
        painter.drawText(rect.adjusted(sampleWidth + m_style.wheelLegendItemGap, 0, 0, 0),
                Qt::AlignLeft | Qt::AlignVCenter,
                item.text);
        painter.restore();
    }
    painter.setPen(m_style.textMuted);
    painter.drawText(rects.last(), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, m_note);
}

} // namespace crate::widgets
