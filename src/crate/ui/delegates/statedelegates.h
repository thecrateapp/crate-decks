#pragma once

#include "crate/ui/delegates/celldelegate.h"

namespace crate::delegates {

/// Crate's mix profile of a track (design 14.3, Crate states), from
/// ProfileQualityRole: nothing for a full profile, an amber dot for a
/// partial one and "NO PROFILE" in mono, textSubtle, when there is none.
class ProfileQualityDelegate : public CellDelegate {
    Q_OBJECT
  public:
    ProfileQualityDelegate(const DelegateStyle& style, QObject* pParent);

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
};

/// Where a track's audio is (design 14.3, Crate states), from CacheStateRole
/// and CacheProgressRole: an accentHi dot when it is cached, StackMap's level
/// ring of r 7 while it downloads and an amber dot with "FAILED" when the
/// download failed. Red stays for destructive actions only.
class CacheStateDelegate : public CellDelegate {
    Q_OBJECT
  public:
    CacheStateDelegate(const DelegateStyle& style, QObject* pParent);

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
};

/// A download's progress (the cache view's Progress column, plan V3.PR2):
/// StackMap's level ring of r 7 from CacheProgressRole, as the library's
/// Cache column draws a download, then the display text ("33.5 MB of
/// 54.0 MB", "Queued") in 11 px mono.
class DownloadProgressDelegate : public CellDelegate {
    Q_OBJECT
  public:
    DownloadProgressDelegate(const DelegateStyle& style, QObject* pParent);

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    /// Where the text starts in a cell's content: after the ring and its gap.
    int textOffset() const;
};

} // namespace crate::delegates
