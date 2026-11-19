#pragma once

#include "crate/ui/delegates/celldelegate.h"

namespace crate::delegates {

/// A compatible track's score (GU1 mockup library-compatibles, decks.css
/// .score): the figure ("0.94") in 11 px mono, textPrimary, then a 1 px lane
/// in border with an accentHi part as long as the score (ScoreRole, 0 to 1).
class ScoreDelegate : public CellDelegate {
    Q_OBJECT
  public:
    ScoreDelegate(const DelegateStyle& style, QObject* pParent);

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;

    /// Where the lane of a cell is, and how much of it is lit: the lane's
    /// full rect in `option`, and the lit part's width.
    QRect laneRect(const QStyleOptionViewItem& option) const;
    static int litWidth(const QRect& lane, double score);
};

/// How a compatible track's key relates to the followed track's (decks.css
/// .harmony): a MonoLabel of 10.5 px with the caps tracking, in accentHi when
/// it mixes in key (HarmonyMatchRole) and textSubtle when it does not.
class HarmonyDelegate : public CellDelegate {
    Q_OBJECT
  public:
    HarmonyDelegate(const DelegateStyle& style, QObject* pParent);

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;

    QColor textColour(const QModelIndex& index) const;
};

/// Whether the user likes a track on Crate (LikedRole): the Solar heart in
/// textFaint, or the filled heart in accentHi, as the mockups' last column.
/// Only a sign: likes change from the track menu.
class LikeDelegate : public CellDelegate {
    Q_OBJECT
  public:
    LikeDelegate(const DelegateStyle& style,
            const QString& heartIconPath,
            const QString& likedIconPath,
            QObject* pParent);

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;

    /// The icon `index` shows, and its colour.
    QString iconPath(const QModelIndex& index) const;
    QColor iconColour(const QModelIndex& index) const;

  private:
    const QString m_heartIconPath;
    const QString m_likedIconPath;
};

} // namespace crate::delegates
