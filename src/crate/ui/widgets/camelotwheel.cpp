#include "crate/ui/widgets/camelotwheel.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <numbers>

#include "moc_camelotwheel.cpp"

namespace crate::widgets {

namespace {

constexpr double kDegreesPerKey = 360.0 / 12;

/// The direction of `degrees` measured clockwise from 12 o'clock, as the
/// mockup's polar() places the keys
QPointF clockwise(double degrees) {
    const double radians = degrees * std::numbers::pi / 180;
    return QPointF(std::sin(radians), -std::cos(radians));
}

double degreesOf(const CamelotWheelModel::Node& node) {
    return node.number * kDegreesPerKey;
}

NodeState stateOf(CamelotWheelModel::Relation relation) {
    switch (relation) {
    case CamelotWheelModel::Relation::Master:
        return NodeState::Selected;
    case CamelotWheelModel::Relation::Compatible:
        return NodeState::Active;
    case CamelotWheelModel::Relation::Context:
        return NodeState::Context;
    case CamelotWheelModel::Relation::Rest:
        break;
    }
    return NodeState::Rest;
}

} // namespace

CamelotWheel::CamelotWheel(CamelotWheelModel* pModel,
        const InstrumentStyle& style,
        QWidget* pParent)
        : QWidget(pParent),
          m_pModel(pModel),
          m_style(style) {
    setObjectName(QStringLiteral("CrateCamelotWheel"));
    setMouseTracking(true);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    // Paint only when what the wheel shows changes (design 9)
    connect(m_pModel, &CamelotWheelModel::changed, this, &CamelotWheel::invalidate);
}

void CamelotWheel::setSubtitle(const QString& subtitle) {
    if (subtitle != m_subtitle) {
        m_subtitle = subtitle;
        invalidate();
    }
}

void CamelotWheel::invalidate() {
    m_frame = QPixmap();
    update();
}

QSize CamelotWheel::sizeHint() const {
    return QSize(m_style.camelotWheelSize, m_style.camelotWheelSize);
}

QSize CamelotWheel::minimumSizeHint() const {
    // Below half the mockup's size the labels of a ring run into each other
    return sizeHint() / 2;
}

double CamelotWheel::scale() const {
    if (m_style.wheelCanvas <= 0) {
        return 0;
    }
    return std::min(width(), height()) / m_style.wheelCanvas;
}

QPointF CamelotWheel::centre() const {
    return QPointF(width() / 2.0, height() / 2.0);
}

QPointF CamelotWheel::nodeCentre(int index) const {
    if (index < 0 || index >= m_pModel->nodes().size()) {
        return QPointF();
    }
    const CamelotWheelModel::Node& node = m_pModel->nodes().at(index);
    const double ring = node.major ? m_style.camelotRingB : m_style.camelotRingA;
    return centre() + clockwise(degreesOf(node)) * ring * scale();
}

int CamelotWheel::nodeAt(QPointF position) const {
    // A key answers up to where its label starts
    const double reach = m_style.nodeRoot + m_style.camelotLabelGap;
    int nearest = -1;
    double nearestDistance = reach;
    for (int index = 0; index < m_pModel->nodes().size(); ++index) {
        const double distance = QLineF(position, nodeCentre(index)).length();
        if (distance <= nearestDistance) {
            nearest = index;
            nearestDistance = distance;
        }
    }
    return nearest;
}

void CamelotWheel::paintScale() {
    const qreal ratio = devicePixelRatioF();
    m_scale = QPixmap(size() * ratio);
    m_scale.setDevicePixelRatio(ratio);
    m_scale.fill(Qt::transparent);
    QPainter painter(&m_scale);
    painter.setRenderHint(QPainter::Antialiasing);
    const double k = scale();
    paintGuideRing(&painter, centre(), m_style.camelotRingA * k, m_style);
    paintGuideRing(&painter, centre(), m_style.camelotRingB * k, m_style);

    // The relative keys at rest: GenreWheel's related pairs, a dotted spoke
    // from each key of the inner ring to the one of the outer ring
    QPen spoke = patternPen(
            StrokePattern::Spoke, m_style.borderSubtle, m_style.strokeHairline, m_style);
    spoke.setCapStyle(Qt::RoundCap);
    spoke.setCosmetic(true);
    painter.setPen(spoke);
    const QVector<CamelotWheelModel::Node>& nodes = m_pModel->nodes();
    const int ring = static_cast<int>(nodes.size() / 2);
    for (int index = 0; index < ring; ++index) {
        painter.drawLine(QLineF(nodeCentre(index), nodeCentre(index + ring)));
    }
}

void CamelotWheel::paintEdges(QPainter* pPainter) const {
    const int master = m_pModel->indexOf(m_pModel->masterKey());
    if (master < 0) {
        return;
    }
    const CamelotWheelModel::Node& from = m_pModel->nodes().at(master);
    const QPointF start = nodeCentre(master);
    QPen pen(m_style.accentHi, m_style.strokeSelected);
    // Like the mockup's edges, as wide at any size of the wheel
    pen.setCosmetic(true);
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setBrush(Qt::NoBrush);
    for (const auto key : m_pModel->compatibleKeys()) {
        const int target = m_pModel->indexOf(key);
        const CamelotWheelModel::Node& to = m_pModel->nodes().at(target);
        const QPointF end = nodeCentre(target);
        QPainterPath path(start);
        QPointF towards = start;
        if (to.major != from.major) {
            // The relative key, straight across the rings
            path.lineTo(end);
        } else {
            // Along a ring, the edge bows inwards around the centre figure
            double middle = (degreesOf(from) + degreesOf(to)) / 2;
            if (std::abs(degreesOf(from) - degreesOf(to)) > 180) {
                middle += 180;
            }
            const double ring = from.major ? m_style.camelotRingB : m_style.camelotRingA;
            towards = centre() + clockwise(middle) * ring * m_style.camelotEdgeBow * scale();
            path.quadTo(towards, end);
        }
        pPainter->setPen(pen);
        pPainter->drawPath(path);
        paintArrow(pPainter, end, end - towards, m_style.strokeSelected, m_style.accentHi, m_style);
    }
    pPainter->restore();
}

void CamelotWheel::paintNodeAndLabel(QPainter* pPainter, int index) const {
    const CamelotWheelModel::Node& node = m_pModel->nodes().at(index);
    const bool isMaster = node.relation == CamelotWheelModel::Relation::Master;
    const bool isLinked = isMaster || node.relation == CamelotWheelModel::Relation::Compatible;
    const double radius = isMaster ? m_style.nodeSelected : m_style.nodeRoot;
    const QPointF position = nodeCentre(index);
    paintNode(pPainter, position, radius, stateOf(node.relation), m_style, index == m_hovered);

    // The label runs along the radius, inwards from the inner ring and
    // outwards from the outer one, and turns over on the left half so that
    // it never reads upside down
    const QFont font = m_style.monoFont(m_style.fontSizeWheelLabel, m_style.trackingWheelLabel);
    const QFontMetricsF metrics(font);
    const double degrees = degreesOf(node);
    const bool inward = !node.major;
    const bool flip = degrees > 180 && degrees < 360;
    const double rotation = flip ? degrees + 90 : degrees - 90;
    const double offset = radius + m_style.camelotLabelGap;
    const double anchor = (flip ? -1 : 1) * (inward ? -offset : offset);
    const double keyWidth = metrics.horizontalAdvance(node.camelot);
    const double width = keyWidth + m_style.camelotNameGap + metrics.horizontalAdvance(node.name);
    const double left = flip == inward ? anchor : anchor - width;
    const double baseline = m_style.camelotLabelShift * m_style.fontSizeWheelLabel;

    pPainter->save();
    pPainter->setOpacity(pPainter->opacity() * node.opacity);
    pPainter->translate(position);
    pPainter->rotate(rotation);
    pPainter->setFont(font);
    pPainter->setPen(isLinked ? m_style.textPrimary : m_style.textMuted);
    pPainter->drawText(QPointF(left, baseline), node.camelot);
    pPainter->setPen(m_style.textSubtle);
    pPainter->drawText(QPointF(left + keyWidth + m_style.camelotNameGap, baseline), node.name);
    pPainter->restore();
}

void CamelotWheel::paintCentre(QPainter* pPainter) const {
    const QPointF middle = centre();
    const QString figure = CamelotWheelModel::camelotOf(m_pModel->masterKey());
    paintCentredText(pPainter,
            QPointF(middle.x(), middle.y() + m_style.camelotFigureBaseline),
            figure,
            m_style.displayFont(m_style.fontSizeDisplay, m_style.trackingWheelFigure),
            m_style.textPrimary);
    paintCentredText(pPainter,
            QPointF(middle.x(), middle.y() + m_style.camelotSubtitleBaseline),
            m_subtitle,
            m_style.monoLabelFont(m_style.fontSizeMeta),
            m_style.textMuted);
}

void CamelotWheel::paintEvent(QPaintEvent* /*pEvent*/) {
    const qreal ratio = devicePixelRatioF();
    if (m_frame.isNull() || m_frame.devicePixelRatio() != ratio ||
            m_frame.deviceIndependentSize() != QSizeF(size())) {
        if (m_scale.isNull() || m_scale.devicePixelRatio() != ratio ||
                m_scale.deviceIndependentSize() != QSizeF(size())) {
            paintScale();
        }
        m_frame = QPixmap(size() * ratio);
        m_frame.setDevicePixelRatio(ratio);
        m_frame.fill(Qt::transparent);
        QPainter frame(&m_frame);
        frame.setRenderHint(QPainter::Antialiasing);
        frame.setRenderHint(QPainter::TextAntialiasing);
        frame.drawPixmap(0, 0, m_scale);
        paintEdges(&frame);
        for (int index = 0; index < m_pModel->nodes().size(); ++index) {
            paintNodeAndLabel(&frame, index);
        }
        paintCentre(&frame);
    }
    QPainter painter(this);
    painter.drawPixmap(0, 0, m_frame);
}

void CamelotWheel::resizeEvent(QResizeEvent* pEvent) {
    // The static scale and the wheel are painted again for the new size
    m_scale = QPixmap();
    m_frame = QPixmap();
    QWidget::resizeEvent(pEvent);
}

void CamelotWheel::setHovered(int index) {
    if (index == m_hovered) {
        return;
    }
    m_hovered = index;
    if (index >= 0) {
        setCursor(Qt::PointingHandCursor);
    } else {
        unsetCursor();
    }
    invalidate();
}

void CamelotWheel::mouseMoveEvent(QMouseEvent* pEvent) {
    setHovered(nodeAt(pEvent->position()));
    QWidget::mouseMoveEvent(pEvent);
}

void CamelotWheel::mousePressEvent(QMouseEvent* pEvent) {
    if (pEvent->button() == Qt::LeftButton) {
        m_pressed = nodeAt(pEvent->position());
    }
    QWidget::mousePressEvent(pEvent);
}

void CamelotWheel::mouseReleaseEvent(QMouseEvent* pEvent) {
    if (pEvent->button() == Qt::LeftButton) {
        const int released = nodeAt(pEvent->position());
        if (released >= 0 && released == m_pressed) {
            emit keyClicked(m_pModel->nodes().at(released).key);
        }
        m_pressed = -1;
    }
    QWidget::mouseReleaseEvent(pEvent);
}

void CamelotWheel::leaveEvent(QEvent* pEvent) {
    setHovered(-1);
    QWidget::leaveEvent(pEvent);
}

} // namespace crate::widgets
