#pragma once

#include <QWidget>

#include "library/libraryview.h"

class QLabel;
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
/// mockup of this pane: it takes the Crate page's cache header and the
/// library's tables, rules and MonoLabels, with sizes from the dialog
/// tokens and colours and fonts from the skin's style (generated/style.qss).
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

  private:
    void showModel();
    QTableView* makeTable(const char* objectName);

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
};

} // namespace crate
