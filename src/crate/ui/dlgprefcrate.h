#pragma once

#include <QPointer>

#include "preferences/dialog/dlgpreferencepage.h"

class QLabel;
class QListWidget;
class QPushButton;

namespace crate {

class AddServerWidget;
class CrateServers;

/// The "Crate" page of the preferences: the Crate servers of this
/// installation, their sign-in state and features, adding one and signing
/// out of one. Changes apply at once; there is nothing to apply or reset.
class DlgPrefCrate : public DlgPreferencePage {
    Q_OBJECT
  public:
    DlgPrefCrate(QWidget* pParent, CrateServers* pServers);

  public slots:
    void slotUpdate() override;
    void slotApply() override {
    }
    void slotResetToDefaults() override {
    }

  private:
    void showSelectedServer();

    const QPointer<CrateServers> m_pServers;
    QListWidget* m_pServerList;
    QLabel* m_pStatus;
    QLabel* m_pKeychainWarning;
    QPushButton* m_pAdd;
    QPushButton* m_pRemove;
    AddServerWidget* m_pAddServer;
};

} // namespace crate
