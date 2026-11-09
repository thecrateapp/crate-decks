#include "crate/ui/skin/preferencescontrol.h"

#include <QAction>
#include <QWidget>

#include "moc_preferencescontrol.cpp"

namespace crate {

const ConfigKey& showPreferencesKey() {
    static const ConfigKey kKey(QStringLiteral("[CrateDecks]"), QStringLiteral("show_preferences"));
    return kKey;
}

QAction* findPreferencesAction(QWidget* pWindow) {
    if (!pWindow) {
        return nullptr;
    }
    const QList<QAction*> actions = pWindow->findChildren<QAction*>();
    for (QAction* pAction : actions) {
        if (pAction->menuRole() == QAction::PreferencesRole) {
            return pAction;
        }
    }
    return nullptr;
}

PreferencesControl::PreferencesControl(QWidget* pSkinRoot)
        : QObject(pSkinRoot),
          m_pSkinRoot(pSkinRoot),
          m_control(showPreferencesKey(), this) {
    m_control.connectValueChanged(this, &PreferencesControl::slotValueChanged);
}

void PreferencesControl::slotValueChanged(double value) {
    if (value <= 0) {
        return;
    }
    // A push, not a state: the button lights only while it is pressed
    m_control.set(0);
    if (QAction* pAction = findPreferencesAction(m_pSkinRoot ? m_pSkinRoot->window() : nullptr)) {
        pAction->trigger();
    }
}

} // namespace crate
