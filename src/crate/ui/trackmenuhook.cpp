#include "crate/ui/trackmenuhook.h"

#include <QMenu>
#include <utility>

namespace crate::ui {

namespace {

/// Marks the entries updateTrackMenu() added.
const char* const kCrateEntryProperty = "crateTrackMenuEntry";

/// The main thread's only: menus open there.
TrackMenuProvider& provider() {
    static TrackMenuProvider s_provider;
    return s_provider;
}

} // namespace

void setTrackMenuProvider(TrackMenuProvider provider) {
    crate::ui::provider() = std::move(provider);
}

QList<QAction*> trackMenuActions(const TrackPointer& pTrack, QObject* pParent) {
    if (!pTrack || !provider()) {
        return {};
    }
    return provider()(pTrack, pParent);
}

void updateTrackMenu(QMenu* pMenu, const TrackPointer& pTrack) {
    if (!pMenu) {
        return;
    }
    const QList<QAction*> current = pMenu->actions();
    for (QAction* pAction : current) {
        if (pAction->property(kCrateEntryProperty).toBool()) {
            pMenu->removeAction(pAction);
            pAction->deleteLater();
        }
    }
    const QList<QAction*> actions = trackMenuActions(pTrack, pMenu);
    if (actions.isEmpty()) {
        return;
    }
    QAction* pSeparator = pMenu->addSeparator();
    pSeparator->setProperty(kCrateEntryProperty, true);
    for (QAction* pAction : actions) {
        pAction->setProperty(kCrateEntryProperty, true);
        pMenu->addAction(pAction);
    }
}

} // namespace crate::ui
