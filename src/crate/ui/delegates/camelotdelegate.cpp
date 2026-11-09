#include "crate/ui/delegates/camelotdelegate.h"

#include <QFontMetrics>
#include <QPainter>

#include "crate/ui/delegates/delegateroles.h"
#include "moc_camelotdelegate.cpp"
#include "track/keyutils.h"

namespace crate::delegates {

CamelotDelegate::CamelotDelegate(const DelegateStyle& style, QObject* pParent)
        : CellDelegate(style, pParent) {
}

QString CamelotDelegate::camelotKey(const QModelIndex& index) const {
    const QVariant camelot = index.data(CamelotRole);
    if (camelot.isValid()) {
        return camelot.toString();
    }
    const QString text = index.data(Qt::DisplayRole).toString().trimmed();
    if (text.isEmpty()) {
        return QString();
    }
    auto found = m_camelotOfText.constFind(text);
    if (found == m_camelotOfText.constEnd()) {
        const auto key = KeyUtils::guessKeyFromText(text);
        found = m_camelotOfText.insert(text,
                key == mixxx::track::io::key::INVALID
                        ? text
                        : KeyUtils::keyToString(key, KeyUtils::KeyNotation::Lancelot));
    }
    return *found;
}

void CamelotDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (!m_style.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    paintBackground(pPainter, option, index);
    const QString key = camelotKey(index);
    if (key.isEmpty()) {
        return;
    }
    const QRect content = contentRect(option);
    const auto tone = static_cast<CamelotTone>(index.data(CamelotToneRole).toInt());
    if (tone != CamelotTone::Master) {
        const QColor colour = tone == CamelotTone::Compatible
                ? m_style.accentHi
                : foreground(index, m_style.textSecondary);
        paintText(pPainter,
                content,
                key,
                m_style.monoFont(m_style.fontSizeMeta),
                colour,
                Qt::AlignLeft | Qt::AlignVCenter);
        return;
    }
    // The master deck's key as an accent chip: a pill with a 1 px outline,
    // mono text, 7 px of padding
    const QFont font = m_style.monoFont(m_style.fontSizeHeader);
    const QFontMetrics metrics(font);
    const int width = metrics.horizontalAdvance(key) + 2 * m_style.chipPadding;
    const int height = m_style.chipHeight;
    const QRectF chip(content.left() + 0.5,
            content.top() + (content.height() - height) / 2 + 0.5,
            width - 1,
            height - 1);
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setPen(QPen(m_style.accentHiBorder, m_style.rule));
    pPainter->setBrush(Qt::NoBrush);
    pPainter->drawRoundedRect(chip, chip.height() / 2, chip.height() / 2);
    pPainter->restore();
    paintText(pPainter,
            chip.toAlignedRect(),
            key,
            font,
            m_style.accentHi,
            Qt::AlignCenter);
}

} // namespace crate::delegates
