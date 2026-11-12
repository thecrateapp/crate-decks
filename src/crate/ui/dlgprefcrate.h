#pragma once

#include <QList>
#include <QPointer>

#include "crate/crateserverrows.h"
#include "preferences/dialog/dlgpreferencepage.h"

class QLabel;
class QListWidget;
class QPushButton;
class QWidget;

namespace crate {

class AddServerWidget;
class AnalysisSection;
class CacheSection;
class CacheStatusModel;
class CrateServers;
class SmartMixSection;

/// The "Crate" page of the preferences: the Crate servers of this
/// installation, their sign-in state and features, adding one and signing
/// out of one. Changes apply at once; there is nothing to apply or reset.
///
/// Laid out as the approved GU1 mockup (preferences-crate, plan V2.PR5):
/// an intro, the servers as rows with their features, then the cache on the
/// left and Auto DJ and Smart Mix on the right, which stays hidden until
/// plan F6 gives it its data. The cache section (F4.C-PR4) shows the audio
/// cache's use and sets the CratePreferences: its limit, its location (for
/// the next start) and Auto DJ's prefetch. Under Smart Mix, Crate analysis
/// sets the key and the gain the import takes from Crate; the mockups have
/// no such section.
class DlgPrefCrate : public DlgPreferencePage {
    Q_OBJECT
  public:
    DlgPrefCrate(QWidget* pParent, CrateServers* pServers);

    /// Shows `rows` instead of this installation's servers until the
    /// servers change. For tests and the screenshots of the page, which have
    /// no server to sign in to (as ServerStatus::showStatus()).
    void showServerRows(const QList<ServerRow>& rows);

    CacheSection* cacheSection() const {
        return m_pCache;
    }
    SmartMixSection* smartMixSection() const {
        return m_pSmartMix;
    }
    AnalysisSection* analysisSection() const {
        return m_pAnalysis;
    }

  public slots:
    void slotUpdate() override;
    void slotApply() override {
    }
    void slotResetToDefaults() override;

  private:
    void showRows(const QList<ServerRow>& rows);
    QWidget* rowWidget(const ServerRow& row);
    /// Connects the cache and analysis sections to the preferences.
    void bindPreferences();
    void showPreferences();
    void showCache();
    void chooseCacheLocation();
    void clearCache();

    const QPointer<CrateServers> m_pServers;
    QListWidget* m_pServerList;
    QLabel* m_pStatus;
    QLabel* m_pKeychainWarning;
    QPushButton* m_pAdd;
    QWidget* m_pAddBox;
    AddServerWidget* m_pAddServer;
    CacheSection* m_pCache;
    SmartMixSection* m_pSmartMix;
    AnalysisSection* m_pAnalysis;
    CacheStatusModel* m_pCacheStatus = nullptr;
    QWidget* m_pColumns;
};

} // namespace crate
