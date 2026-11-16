#pragma once

#include <QAction>
#include <QList>
#include <functional>

#include "track/track_decl.h"

class QMenu;
class QObject;

namespace crate::ui {

/// The entries Crate adds to upstream's track menu (WTrackMenu) of one
/// track, wherever it opens: the decks, Auto DJ, upstream's lists and the
/// Crate tables (plan F8.C-PR2, Diego, 2026-11-16). WTrackMenu calls
/// updateTrackMenu() as it opens and holds no Crate logic; the library sets
/// the provider while it is there.
using TrackMenuProvider =
        std::function<QList<QAction*>(const TrackPointer& pTrack, QObject* pParent)>;

/// Sets the provider; an empty one removes it.
void setTrackMenuProvider(TrackMenuProvider provider);

/// The provider's entries for `pTrack`, owned by `pParent`; none without a
/// provider or a track.
QList<QAction*> trackMenuActions(const TrackPointer& pTrack, QObject* pParent);

/// Replaces the entries this added to `pMenu` before with the provider's
/// for `pTrack` (nullptr for none: no track, or more than one), at the end
/// of the menu after a separator.
void updateTrackMenu(QMenu* pMenu, const TrackPointer& pTrack);

} // namespace crate::ui
