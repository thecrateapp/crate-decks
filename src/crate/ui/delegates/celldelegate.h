#pragma once

#include <QStyledItemDelegate>
#include <functional>

#include "crate/ui/delegates/delegatestyle.h"

namespace crate::delegates {

/// A cell of the CrateDecks track table (GU1 mockups decks.css .tracks td):
/// 10 px of padding, the text centred over the row's 1 px rule, no focus
/// frame and no selection fill of its own. The row itself (rule, hover,
/// selection) is painted under the cells by the table (crate/ui/skin/
/// librarystyle.h), so a cell only paints what it shows. Without a valid
/// style it paints as QStyledItemDelegate does.
class CellDelegate : public QStyledItemDelegate {
    Q_OBJECT
  public:
    CellDelegate(const DelegateStyle& style, QObject* pParent);

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    const DelegateStyle& style() const {
        return m_style;
    }

  protected:
    /// The track colour of upstream's tables (Qt::BackgroundRole), if any.
    void paintBackground(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const;
    /// The cell less its padding and the row's rule.
    QRect contentRect(const QStyleOptionViewItem& option) const;
    /// The colour upstream gives played and missing tracks
    /// (Qt::ForegroundRole), or `fallback`.
    QColor foreground(const QModelIndex& index, const QColor& fallback) const;

    const DelegateStyle m_style;
};

/// The text cells of a track: the title in the body font (in the colour of
/// the deck that has it loaded), names in the body font and muted, and
/// numbers (BPM, duration, dates) in 11 px mono with tabular figures.
class TrackTextDelegate : public CellDelegate {
    Q_OBJECT
  public:
    enum class Kind {
        Title,
        Name,
        Meta,
    };
    /// The deck that has the track of a row loaded ("A" to "D"), for models
    /// that do not give LoadedDeckRole themselves, such as upstream's.
    using LoadedDeck = std::function<QString(const QModelIndex&)>;

    TrackTextDelegate(const DelegateStyle& style, Kind kind, QObject* pParent);

    void setLoadedDeck(LoadedDeck loadedDeck) {
        m_loadedDeck = std::move(loadedDeck);
    }

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;

    /// The colour the text of `index` takes.
    QColor textColour(const QModelIndex& index) const;
    /// The deck letter of `index`'s track, if a deck has it loaded.
    QString loadedDeck(const QModelIndex& index) const;

  protected:
    QFont textFont() const;
    Qt::Alignment alignment(const QModelIndex& index) const;

    const Kind m_kind;
    LoadedDeck m_loadedDeck;
};

/// The BPM of a track: mono, right-aligned, with a Solar lock when the BPM
/// is locked (Qt::CheckStateRole). The lock only shows the state; it is set
/// from the track menu, so that no invisible target toggles it.
class BpmDelegate : public TrackTextDelegate {
    Q_OBJECT
  public:
    BpmDelegate(const DelegateStyle& style, const QString& lockIconPath, QObject* pParent);

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
    bool editorEvent(QEvent* pEvent,
            QAbstractItemModel* pModel,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) override;

  private:
    const QString m_lockIconPath;
};

} // namespace crate::delegates
