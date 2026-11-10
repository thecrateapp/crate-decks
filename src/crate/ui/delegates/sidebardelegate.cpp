#include "crate/ui/delegates/sidebardelegate.h"

#include <QAbstractItemView>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>

#include "crate/ui/delegates/delegateroles.h"
#include "moc_sidebardelegate.cpp"

namespace crate::delegates {

LabelAndCount splitCount(const QString& text) {
    static const QRegularExpression kCount(QStringLiteral("^(.*\\S) \\((\\d+)\\)(?: (\\S.*))?$"));
    const QRegularExpressionMatch match = kCount.match(text);
    if (!match.hasMatch()) {
        return {text, QString()};
    }
    QString count = groupThousands(match.captured(2));
    if (!match.captured(3).isEmpty()) {
        count += QStringLiteral(" · ") + match.captured(3);
    }
    return {match.captured(1), count};
}

namespace {

int depthOf(const QModelIndex& index) {
    int depth = 0;
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
        ++depth;
    }
    return depth;
}

} // namespace

SidebarDelegate::SidebarDelegate(const DelegateStyle& style, QAbstractItemView* pView)
        : QStyledItemDelegate(pView),
          m_style(style),
          m_pView(pView) {
}

void SidebarDelegate::setLogo(int role, const QString& svgPath, QSize size) {
    m_logoRole = role;
    m_logoPath = svgPath;
    m_logoSize = size;
}

bool SidebarDelegate::isSectionRoot(const QModelIndex& index) const {
    return !m_sectionRootIcon.isEmpty() && m_iconNameRole >= 0 && index.isValid() &&
            !index.parent().isValid() &&
            index.data(m_iconNameRole).toString() == m_sectionRootIcon;
}

QString SidebarDelegate::sectionOf(const QModelIndex& index) const {
    if (isSectionRoot(index)) {
        return index.data(Qt::DisplayRole).toString();
    }
    const QString section = index.data(SidebarSectionRole).toString();
    return section.isEmpty() ? m_defaultSection : section;
}

int SidebarDelegate::levelOf(const QModelIndex& index) const {
    int depth = depthOf(index);
    if (depth > 0) {
        QModelIndex top = index;
        while (top.parent().isValid()) {
            top = top.parent();
        }
        if (isSectionRoot(top)) {
            --depth;
        }
    }
    return depth;
}

QString SidebarDelegate::sectionHeader(const QModelIndex& index) const {
    if (!index.isValid() || index.parent().isValid()) {
        return QString();
    }
    const QString section = sectionOf(index);
    if (isSectionRoot(index)) {
        return section;
    }
    if (index.row() == 0) {
        return section;
    }
    const QModelIndex previous = index.siblingAtRow(index.row() - 1);
    return sectionOf(previous) == section ? QString() : section;
}

int SidebarDelegate::sectionHeaderHeight() const {
    return m_style.sidebarSectionTop + m_style.monoLabelLineHeight(m_style.fontSizeNav) +
            m_style.sidebarSectionBottom;
}

QSize SidebarDelegate::sizeHint(
        const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (!m_style.isValid()) {
        return QStyledItemDelegate::sizeHint(option, index);
    }
    if (isSectionRoot(index)) {
        // Only its header: its children are the section's rows
        const int width = m_pView ? m_pView->viewport()->width() : option.rect.width();
        return QSize(width, sectionHeaderHeight());
    }
    int height = m_style.rowHeight;
    if (!sectionHeader(index).isEmpty()) {
        height += sectionHeaderHeight();
    }
    // As wide as the sidebar: the whole row takes the clicks
    const int width = m_pView ? m_pView->viewport()->width() : option.rect.width();
    return QSize(width, height);
}

void SidebarDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (!m_style.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    // The view has no indentation and its only column is as wide as the
    // sidebar (sizeHint()): the item's rect is the whole row
    QRect row = option.rect;

    const QString header = sectionHeader(index);
    if (!header.isEmpty()) {
        const int lineHeight = m_style.monoLabelLineHeight(m_style.fontSizeNav);
        const QRect title(row.left() + m_style.sidebarSectionX,
                row.top() + m_style.sidebarSectionTop,
                row.width() - 2 * m_style.sidebarSectionX,
                lineHeight);
        paintText(pPainter,
                title,
                header,
                m_style.monoLabelFont(m_style.fontSizeNav),
                m_style.accentHi,
                Qt::AlignLeft | Qt::AlignVCenter);
        row.setTop(row.top() + sectionHeaderHeight());
    }
    if (isSectionRoot(index)) {
        return;
    }

    const QRect item = row.adjusted(m_style.sidebarItemInset, 0, -m_style.sidebarItemInset, 0);
    const bool active = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const QColor colour = active ? m_style.textPrimary
                                 : (hovered ? m_style.accentHi : m_style.navIdle);
    if (active) {
        pPainter->fillRect(QRect(item.left(),
                                   item.top() + m_style.sidebarBarInset,
                                   m_style.activeBar,
                                   item.height() - 2 * m_style.sidebarBarInset),
                m_style.accentHi);
    }

    const int depth = levelOf(index);
    int left = item.left() + m_style.sidebarItemPadding + depth * m_style.sidebarIndent;
    int right = item.right() - m_style.sidebarItemPadding;
    if (m_logoRole >= 0 && index.data(m_logoRole).toBool() && !m_logoSize.isEmpty()) {
        // The logo keeps its colours: it is the brand, not a Solar icon
        QSvgRenderer logo(m_logoPath);
        const QRect rect(left,
                item.top() + (item.height() - m_logoSize.height()) / 2,
                m_logoSize.width(),
                m_logoSize.height());
        logo.render(pPainter, rect);
        left += m_logoSize.width() + m_style.sidebarIconGap;
    } else if (depth == 0 && m_iconNameRole >= 0) {
        // A section root's children name their icon through their QIcon
        const QString iconName = index.parent().isValid()
                ? index.data(Qt::DecorationRole).value<QIcon>().name()
                : index.data(m_iconNameRole).toString();
        const QString path = m_iconPaths.value(iconName);
        if (!path.isEmpty()) {
            const int size = m_style.sidebarIcon;
            paintIcon(pPainter,
                    QRect(left, item.top() + (item.height() - size) / 2, size, size),
                    path,
                    colour);
            left += size + m_style.sidebarIconGap;
        }
    }

    LabelAndCount text{index.data(Qt::DisplayRole).toString(),
            index.data(SidebarCountRole).toString()};
    if (text.count.isEmpty()) {
        text = splitCount(text.label);
    }
    const QString& count = text.count;
    if (!count.isEmpty()) {
        const QFont font = m_style.monoFont(m_style.fontSizeMark);
        const int width = QFontMetrics(font).horizontalAdvance(count);
        paintText(pPainter,
                QRect(right - width + 1, item.top(), width, item.height()),
                count,
                font,
                m_style.textFaint,
                Qt::AlignRight | Qt::AlignVCenter);
        right -= width + m_style.sidebarIconGap;
    }

    paintText(pPainter,
            QRect(left, item.top(), right - left + 1, item.height()),
            text.label,
            m_style.monoLabelFont(m_style.fontSizeNav),
            colour,
            Qt::AlignLeft | Qt::AlignVCenter);
}

} // namespace crate::delegates
