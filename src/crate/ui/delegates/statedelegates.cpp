#include "crate/ui/delegates/statedelegates.h"

#include <QFontMetrics>
#include <QPainter>

#include "crate/ui/delegates/delegateroles.h"
#include "moc_statedelegates.cpp"

namespace crate::delegates {

namespace {

/// The centre of a dot at the start of `content`. The mockups' dots sit on
/// the text line, two pixels under the middle of the row.
QPointF dotCentre(const QRect& content, const DelegateStyle& style) {
    return QPointF(content.left() + style.dot / 2.0, content.center().y() + 2.5);
}

} // namespace

ProfileQualityDelegate::ProfileQualityDelegate(const DelegateStyle& style, QObject* pParent)
        : CellDelegate(style, pParent) {
}

void ProfileQualityDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (!m_style.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    paintBackground(pPainter, option, index);
    const QRect content = contentRect(option);
    switch (static_cast<ProfileQuality>(index.data(ProfileQualityRole).toInt())) {
    case ProfileQuality::Partial:
        paintDot(pPainter, dotCentre(content, m_style), m_style.dot, m_style.warning);
        break;
    case ProfileQuality::Missing:
        paintText(pPainter,
                content,
                tr("No profile"),
                m_style.monoLabelFont(m_style.fontSizeMark),
                m_style.textSubtle,
                Qt::AlignLeft | Qt::AlignVCenter);
        break;
    case ProfileQuality::Full:
    case ProfileQuality::Unknown:
        break;
    }
}

CacheStateDelegate::CacheStateDelegate(const DelegateStyle& style, QObject* pParent)
        : CellDelegate(style, pParent) {
}

void CacheStateDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (!m_style.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    paintBackground(pPainter, option, index);
    const QRect content = contentRect(option);
    switch (static_cast<CacheState>(index.data(CacheStateRole).toInt())) {
    case CacheState::Ready:
        paintDot(pPainter, dotCentre(content, m_style), m_style.dot, m_style.accentHi);
        break;
    case CacheState::Downloading: {
        // The ring's box is its diameter and two pixels on each side, at
        // the start of the cell and in the middle of the row
        const QPointF centre(content.left() + m_style.ringRadius + 2,
                option.rect.top() + m_style.rowHeight / 2.0);
        paintLevelRing(pPainter,
                centre,
                index.data(CacheProgressRole).toDouble(),
                m_style.border,
                m_style.accentHi,
                m_style);
        break;
    }
    case CacheState::Failed: {
        paintDot(pPainter, dotCentre(content, m_style), m_style.dot, m_style.warning);
        QRect text = content;
        text.setLeft(content.left() + m_style.dot + m_style.stateGap);
        paintText(pPainter,
                text,
                tr("Failed").toUpper(),
                m_style.monoFont(m_style.fontSizeMark),
                m_style.warningText,
                Qt::AlignLeft | Qt::AlignVCenter);
        break;
    }
    case CacheState::None:
        break;
    }
}

} // namespace crate::delegates
