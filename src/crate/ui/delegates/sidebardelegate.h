#pragma once

#include <QHash>
#include <QPointer>
#include <QSize>
#include <QStyledItemDelegate>

#include "crate/ui/delegates/delegatestyle.h"

class QAbstractItemView;

namespace crate::delegates {

/// A sidebar label and the count Mixxx writes into it: "Tracks (1234)" and
/// "Warm-up (12) 1:02:33" are "Tracks" with "1 234", and "Warm-up" with
/// "12 · 1:02:33". The admin's navigation shows counts on the right, in mono.
struct LabelAndCount {
    QString label;
    QString count;
};
LabelAndCount splitCount(const QString& text);

/// The library sidebar as the admin's navigation (design 14.1, plan V2.PR4,
/// GU1 mockups decks.css .side): rows of 30 px with an 11 px MonoLabel in
/// capitals; the active row in textPrimary with a 2 px bar in accentHi on
/// its left, the hovered one in accentHi, the rest in navIdle; a Solar icon
/// on top-level items and a count in mono on the right (SidebarCountRole).
///
/// Top-level items belong to sections, which start with a MonoLabel header
/// in the accent tone above their first item. An item's section is its
/// SidebarSectionRole or, without one, the default section: Mixxx's own
/// features have none and fall in "Mixxx", and Crate's section (F3) gives
/// its own. Child items indent 20 px a level and have no icon, as in the
/// mockups; the view's own indentation and branch arrows are off.
class SidebarDelegate : public QStyledItemDelegate {
    Q_OBJECT
  public:
    SidebarDelegate(const DelegateStyle& style, QAbstractItemView* pView);

    /// The section of top-level items without SidebarSectionRole.
    void setDefaultSection(const QString& title) {
        m_defaultSection = title;
    }
    /// The Solar icon (an SVG path) of top-level items, by the icon name the
    /// model gives under `role`, as SidebarModel::IconNameRole does.
    void setIcons(const QHash<QString, QString>& svgPathOfIconName, int role) {
        m_iconPaths = svgPathOfIconName;
        m_iconNameRole = role;
    }

    /// Items whose `role` is true show the logo at `svgPath`, in its own
    /// colours, at `size` before their label: the Crate page in the
    /// navigation of Preferences (GU1 mockups preferences-crate).
    void setLogo(int role, const QString& svgPath, QSize size);

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    /// The header painted above `index`: its section's title when it is the
    /// first top-level item of a section, otherwise empty.
    QString sectionHeader(const QModelIndex& index) const;
    /// Height of a section header.
    int sectionHeaderHeight() const;

  private:
    QString sectionOf(const QModelIndex& index) const;

    const DelegateStyle m_style;
    QPointer<QAbstractItemView> m_pView;
    QString m_defaultSection;
    QHash<QString, QString> m_iconPaths;
    int m_iconNameRole = -1;
    int m_logoRole = -1;
    QString m_logoPath;
    QSize m_logoSize;
};

} // namespace crate::delegates
