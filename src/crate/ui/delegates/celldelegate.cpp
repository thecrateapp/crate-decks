#include "crate/ui/delegates/celldelegate.h"

#include <QEvent>
#include <QLocale>
#include <QPainter>

#include "crate/ui/delegates/delegateroles.h"
#include "moc_celldelegate.cpp"

namespace crate::delegates {

CellDelegate::CellDelegate(const DelegateStyle& style, QObject* pParent)
        : QStyledItemDelegate(pParent),
          m_style(style) {
}

QSize CellDelegate::sizeHint(
        const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    if (m_style.isValid()) {
        size.setHeight(m_style.rowHeight);
    }
    return size;
}

void CellDelegate::paintBackground(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    const QVariant background = index.data(Qt::BackgroundRole);
    if (background.canConvert<QBrush>()) {
        // Leave the row's rule visible under the track colour
        QRect rect = option.rect;
        rect.setBottom(rect.bottom() - m_style.rule);
        pPainter->fillRect(rect, qvariant_cast<QBrush>(background));
    }
}

QRect CellDelegate::contentRect(const QStyleOptionViewItem& option) const {
    // The rule is the row's last pixel: centre on the rest, as the mockups'
    // collapsed table borders do
    return option.rect.adjusted(m_style.cellPadding,
            0,
            -m_style.cellPadding,
            -m_style.rule);
}

QColor CellDelegate::foreground(const QModelIndex& index, const QColor& fallback) const {
    const QVariant value = index.data(Qt::ForegroundRole);
    if (value.canConvert<QColor>()) {
        const QColor colour = value.value<QColor>();
        if (colour.isValid()) {
            return colour;
        }
    }
    if (value.canConvert<QBrush>()) {
        const QColor colour = qvariant_cast<QBrush>(value).color();
        if (colour.isValid()) {
            return colour;
        }
    }
    return fallback;
}

TrackTextDelegate::TrackTextDelegate(const DelegateStyle& style, Kind kind, QObject* pParent)
        : CellDelegate(style, pParent),
          m_kind(kind) {
}

QString TrackTextDelegate::loadedDeck(const QModelIndex& index) const {
    const QVariant deck = index.data(LoadedDeckRole);
    if (deck.isValid()) {
        return deck.toString();
    }
    return m_loadedDeck ? m_loadedDeck(index) : QString();
}

QColor TrackTextDelegate::textColour(const QModelIndex& index) const {
    switch (m_kind) {
    case Kind::Title: {
        const QColor deck = m_style.deckColour(loadedDeck(index));
        if (deck.isValid()) {
            return deck;
        }
        return foreground(index, m_style.textPrimary);
    }
    case Kind::Name:
        return foreground(index, m_style.textMuted);
    case Kind::Meta:
        return foreground(index, m_style.textSecondary);
    }
    return m_style.textPrimary;
}

QFont TrackTextDelegate::textFont() const {
    return m_kind == Kind::Meta ? m_style.monoFont(m_style.fontSizeMeta)
                                : m_style.bodyFont(m_style.fontSizeText);
}

Qt::Alignment TrackTextDelegate::alignment(const QModelIndex& index) const {
    // The model's horizontal alignment (Mixxx right-aligns BPM and
    // duration), always centred vertically
    const QVariant value = index.data(Qt::TextAlignmentRole);
    Qt::Alignment horizontal = Qt::AlignLeft;
    if (value.isValid()) {
        horizontal = Qt::Alignment(value.toInt()) & Qt::AlignHorizontal_Mask;
        if (horizontal == Qt::AlignHCenter && m_kind == Kind::Meta) {
            // Mixxx centres dates; a column of tabular figures reads better
            // from the left, as the mockups' mono cells do
            horizontal = Qt::AlignLeft;
        }
    }
    if (!horizontal) {
        horizontal = Qt::AlignLeft;
    }
    return horizontal | Qt::AlignVCenter;
}

void TrackTextDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (!m_style.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    paintBackground(pPainter, option, index);
    paintText(pPainter,
            contentRect(option),
            index.data(Qt::DisplayRole).toString(),
            textFont(),
            textColour(index),
            alignment(index));
}

BpmDelegate::BpmDelegate(
        const DelegateStyle& style, const QString& lockIconPath, QObject* pParent)
        : TrackTextDelegate(style, Kind::Meta, pParent),
          m_lockIconPath(lockIconPath) {
}

void BpmDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (!m_style.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    // Mixxx writes the BPM with the locale's decimal point; the decks and
    // the mockups write "124.00" whatever the locale
    QString bpm = index.data(Qt::DisplayRole).toString();
    bpm.replace(QLocale().decimalPoint(), QStringLiteral("."));
    paintBackground(pPainter, option, index);
    paintText(pPainter, contentRect(option), bpm, textFont(), textColour(index), alignment(index));
    if (index.data(Qt::CheckStateRole).toInt() != Qt::Checked) {
        return;
    }
    const QRect content = contentRect(option);
    const int size = m_style.smallIcon;
    const QRect lock(content.left(), content.center().y() - size / 2 + 1, size, size);
    paintIcon(pPainter, lock, m_lockIconPath, m_style.textSubtle);
}

bool BpmDelegate::editorEvent(QEvent* pEvent,
        QAbstractItemModel* pModel,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) {
    Q_UNUSED(pEvent);
    Q_UNUSED(pModel);
    Q_UNUSED(option);
    Q_UNUSED(index);
    // No check box to click: the lock is set from the track menu
    return false;
}

} // namespace crate::delegates
