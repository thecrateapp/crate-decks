#pragma once

#include <QHash>

#include "crate/ui/delegates/celldelegate.h"

namespace crate::delegates {

/// The key of a track in Camelot ("8A"), in 11 px mono in its fixed column
/// (design 14.3: Camelot in mono, no colour per key). The tone follows how
/// the key relates to the master deck's (CamelotToneRole): at rest in
/// textSecondary, compatible keys in accentHi, and the master deck's own key
/// as an accent chip (GU1 mockups decks.css .chip[data-tone=accent]).
///
/// The key comes from CamelotRole or, for upstream's key column, from the
/// display text in whatever notation the user picked, read back as a key and
/// written in Camelot.
class CamelotDelegate : public CellDelegate {
    Q_OBJECT
  public:
    CamelotDelegate(const DelegateStyle& style, QObject* pParent);

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;

    /// The Camelot key of `index`, or an empty string.
    QString camelotKey(const QModelIndex& index) const;

  private:
    // Key texts of upstream's notations and their Camelot keys, which paint
    // once per visible row
    mutable QHash<QString, QString> m_camelotOfText;
};

} // namespace crate::delegates
