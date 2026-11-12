#pragma once

#include "crate/ui/skin/crateskin.h"

class Library;
class QWidget;

namespace crate {

/// Crate Decks' defaults of the library preferences, for a profile that has
/// none: the BPM with two decimals, as the decks and the GU1 mockups write it
/// ("124.00"). Upstream's preferences dialog saves its own defaults when it is
/// created, before any skin loads, so CoreServices applies these first.
void applyLibraryDefaults(const UserSettingsPointer& pConfig);

/// Gives the library of a loaded CrateDecks skin what QSS cannot do (plan
/// V2.PR4, design 14.1 and 14.3, GU1 mockups decks.css .lib):
///
/// - The sidebar paints its items with crate::delegates::SidebarDelegate:
///   MonoLabels, Solar icons, the active bar and section headers.
/// - Every track table paints its rows under the cells: a 1 px rule, hover
///   at 4 % and the selection at 8 % of cyan with a 2 px bar on the left,
///   in a 34 px column with the letter of the deck that has the row's track
///   loaded. Its column headers are MonoLabels, the sorted one brighter,
///   aligned like their cells.
/// - The cells of titles, names, BPM, key and duration use the delegates of
///   crate/ui/delegates/ in place of upstream's text delegates; upstream's other
///   delegates (rating, colour, cover art, preview, ...) stay.
/// - A table without a saved layout shows the columns of the mockups that
///   Upstream has, in their order and widths; the user's own layout wins.
/// - Rows are 30 px unless the user set a row height in the preferences.
///
/// WTrackTableView gives each column its delegate on every model load,
/// between hiding and showing itself: the table is restyled when it shows,
/// so no upstream file is patched for it. Does nothing to a skin without
/// generated/vars.xml.
void styleLibrary(QWidget* pSkinRoot,
        const SkinVariables& variables,
        const QString& skinPath,
        Library* pLibrary,
        const UserSettingsPointer& pConfig);

} // namespace crate
