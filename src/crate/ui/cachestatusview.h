#pragma once

#include <QWidget>

#include "library/libraryview.h"

class QLabel;
class QScrollArea;
class QTableView;
class QVBoxLayout;

namespace crate {

class CacheStatusModel;
class LevelGauge;

/// The Cache item of the Crate section (plan F4.C-PR4, GU1 mockups sidebar
/// "Cache"), shown in the library pane through WLibrary::registerView(): the
/// cache's use against its limit on the level ring of the preferences, then
/// its downloads and its offline sets as tables in the library's style.
///
/// A thin layer over CacheStatusModel, which it does not own. There is no
/// mockup of this pane: it takes the Crate page's cache header (LevelGauge,
/// as the Preferences mockup) and the library's tables, rules, MonoLabels
/// and delegates (each download's progress and an offline set's cache as
/// LevelRings of r 7, as the library's Cache column), with sizes from the
/// dialog tokens and colours and fonts from the skin's style
/// (generated/style.qss).
///
/// The tables are as tall as their rows and everything scrolls together, so
/// a short pane (the library under the decks at 1366 x 768) clips nothing;
/// there the header is compact, without its caption line. The library's
/// delegates need the skin's style, which the skin sets after the library
/// binds its views: the view takes it when it first shows.
class CacheStatusView : public QWidget, public LibraryView {
    Q_OBJECT
  public:
    CacheStatusView(CacheStatusModel* pModel, QWidget* pParent);
    ~CacheStatusView() override;

    // LibraryView
    void onShow() override;
    bool hasFocus() const override;

    LevelGauge* gauge() const {
        return m_pGauge;
    }
    QLabel* usageLabel() const {
        return m_pUsage;
    }
    QLabel* captionLabel() const {
        return m_pCaption;
    }
    QTableView* downloadsTable() const {
        return m_pDownloads;
    }
    QTableView* offlineSetsTable() const {
        return m_pOfflineSets;
    }
    QScrollArea* scrollArea() const {
        return m_pScroll;
    }
    /// Whether the tables have the library's delegates (the skin's style).
    bool isStyled() const {
        return m_styled;
    }
    /// Whether the header is compact for a short pane.
    bool isCompact() const {
        return m_compact;
    }
    QSize minimumSizeHint() const override;

  protected:
    void showEvent(QShowEvent* pEvent) override;
    void resizeEvent(QResizeEvent* pEvent) override;

  private:
    void showModel();
    QTableView* makeTable(const char* objectName);
    /// Gives the tables the library's delegates and row sizes once the
    /// skin's style is there.
    void applyStyle();
    /// Each table as tall as its header and rows.
    void fitTables();
    void setCompact(bool compact);

    CacheStatusModel* const m_pModel;
    LevelGauge* m_pGauge;
    QLabel* m_pUsage;
    QLabel* m_pOf;
    QLabel* m_pCaption;
    QTableView* m_pDownloads;
    QLabel* m_pNoDownloads;
    QTableView* m_pOfflineSets;
    QLabel* m_pNoOfflineSets;
    QVBoxLayout* m_pLayout = nullptr;
    QScrollArea* m_pScroll = nullptr;
    QWidget* m_pHeader = nullptr;
    bool m_styled = false;
    bool m_compact = false;
};

} // namespace crate
