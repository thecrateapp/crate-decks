#pragma once

#include <QObject>
#include <QPointer>

#include "control/controlproxy.h"
#include "preferences/configobject.h"

class QAction;
class QWidget;

namespace crate {

/// [CrateDecks],show_preferences: the skin's toolbar button that opens
/// Preferences (plan V2.PR5). The skin declares the control in its manifest.
const ConfigKey& showPreferencesKey();

/// The action of `pWindow`'s menu bar that opens Preferences, the one with
/// QAction::PreferencesRole; nullptr if there is none.
QAction* findPreferencesAction(QWidget* pWindow);

/// Opens Preferences when [CrateDecks],show_preferences goes to 1, and puts
/// the control back to 0. Mixxx has no control that opens Preferences: its
/// menu bar's action does (WMainMenuBar). This triggers that action of the
/// window `pSkinRoot` is in, so the button, the menu and the keyboard
/// shortcut do the same thing, also when the menu bar is hidden. It belongs
/// to the skin root and goes with it on a skin reload.
class PreferencesControl : public QObject {
    Q_OBJECT
  public:
    explicit PreferencesControl(QWidget* pSkinRoot);

  private slots:
    void slotValueChanged(double value);

  private:
    QPointer<QWidget> m_pSkinRoot;
    ControlProxy m_control;
};

} // namespace crate
