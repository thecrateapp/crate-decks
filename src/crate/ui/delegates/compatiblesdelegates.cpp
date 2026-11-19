#include "crate/ui/delegates/compatiblesdelegates.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

#include "crate/ui/delegates/delegateroles.h"
#include "moc_compatiblesdelegates.cpp"

namespace crate::delegates {

ScoreDelegate::ScoreDelegate(const DelegateStyle& style, QObject* pParent)
        : CellDelegate(style, pParent) {
}

QRect ScoreDelegate::laneRect(const QStyleOptionViewItem& option) const {
    const QRect content = contentRect(option);
    const int left = content.left() + m_style.scoreFigure + m_style.scoreGap;
    return QRect(left,
            content.top() + content.height() / 2,
            std::max(0, content.right() - left + 1),
            m_style.rule);
}

int ScoreDelegate::litWidth(const QRect& lane, double score) {
    const double value = std::isfinite(score) ? std::clamp(score, 0.0, 1.0) : 0.0;
    return static_cast<int>(std::lround(lane.width() * value));
}

void ScoreDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (!m_style.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    paintBackground(pPainter, option, index);
    const QRect content = contentRect(option);
    paintText(pPainter,
            QRect(content.left(), content.top(), m_style.scoreFigure, content.height()),
            index.data(Qt::DisplayRole).toString(),
            m_style.monoFont(m_style.fontSizeMeta),
            m_style.textPrimary,
            Qt::AlignLeft | Qt::AlignVCenter);
    const QVariant score = index.data(ScoreRole);
    if (!score.isValid()) {
        return;
    }
    const QRect lane = laneRect(option);
    pPainter->fillRect(lane, m_style.border);
    const int lit = litWidth(lane, score.toDouble());
    if (lit > 0) {
        pPainter->fillRect(QRect(lane.left(), lane.top(), lit, lane.height()), m_style.accentHi);
    }
}

HarmonyDelegate::HarmonyDelegate(const DelegateStyle& style, QObject* pParent)
        : CellDelegate(style, pParent) {
}

QColor HarmonyDelegate::textColour(const QModelIndex& index) const {
    return index.data(HarmonyMatchRole).toBool() ? m_style.accentHi : m_style.textSubtle;
}

void HarmonyDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (!m_style.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    paintBackground(pPainter, option, index);
    paintText(pPainter,
            contentRect(option),
            index.data(Qt::DisplayRole).toString().toUpper(),
            m_style.monoLabelFont(m_style.fontSizeHeader),
            textColour(index),
            Qt::AlignLeft | Qt::AlignVCenter);
}

LikeDelegate::LikeDelegate(const DelegateStyle& style,
        const QString& heartIconPath,
        const QString& likedIconPath,
        QObject* pParent)
        : CellDelegate(style, pParent),
          m_heartIconPath(heartIconPath),
          m_likedIconPath(likedIconPath) {
}

QString LikeDelegate::iconPath(const QModelIndex& index) const {
    return index.data(LikedRole).toBool() ? m_likedIconPath : m_heartIconPath;
}

QColor LikeDelegate::iconColour(const QModelIndex& index) const {
    return index.data(LikedRole).toBool() ? m_style.accentHi : m_style.textFaint;
}

void LikeDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (!m_style.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    paintBackground(pPainter, option, index);
    const QRect content = contentRect(option);
    const int size = m_style.likeIcon;
    const QRect icon(content.left(), content.center().y() - size / 2 + 1, size, size);
    paintIcon(pPainter, icon, iconPath(index), iconColour(index));
}

} // namespace crate::delegates
