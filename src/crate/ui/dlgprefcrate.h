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
class CacheSection;
class CrateServers;
class SmartMixSection;

/// The "Crate" page of the preferences: the Crate servers of this
/// installation, their sign-in state and features, adding one and signing
/// out of one. Changes apply at once; there is nothing to apply or reset.
///
/// Laid out as the approved GU1 mockup (preferences-crate, plan V2.PR5):
/// an intro, the servers as rows with their features, and the cache and
/// Auto DJ and Smart Mix sections, which stay hidden until plans F4 and F6
/// give them their data.
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

  public slots:
    void slotUpdate() override;
    void slotApply() override {
    }
    void slotResetToDefaults() override {
    }

  private:
    void showRows(const QList<ServerRow>& rows);
    QWidget* rowWidget(const ServerRow& row);

    const QPointer<CrateServers> m_pServers;
    QListWidget* m_pServerList;
    QLabel* m_pStatus;
    QLabel* m_pKeychainWarning;
    QPushButton* m_pAdd;
    QWidget* m_pAddBox;
    AddServerWidget* m_pAddServer;
    CacheSection* m_pCache;
    SmartMixSection* m_pSmartMix;
    QWidget* m_pColumns;
};

} // namespace crate
