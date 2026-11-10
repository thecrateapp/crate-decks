#pragma once

#include <Qt>

class QDialog;

namespace crate {

/// The data role of the item of Preferences' navigation that shows the Crate
/// logo: the Crate page (GU1 mockups preferences-crate).
inline constexpr int kPreferencesLogoRole = Qt::UserRole + 0x4c4f;

/// Gives Mixxx's Preferences (DlgPreferences, by the object names of its
/// .ui) the dialog of the GU1 mockups (plan V2.PR5):
/// - DialogChrome with the title "Crate Decks · Preferences";
/// - the navigation as the admin's, with MonoLabel items, the active one
///   with a 2 px bar in accentHi, and the Crate logo on the Crate page;
/// - a header over the pages: the "Preferences" MonoLabel and the page's
///   title in display type, with the mockups' page padding;
/// - a full-width foot with the actions in the mockups' order: Restore
///   defaults (and Help, when the page has a manual page) on the left,
///   Cancel, Apply and OK on the right. They click Mixxx's own buttons,
///   which stay in their QDialogButtonBox, hidden, so DlgPreferences works
///   as before.
/// The pages keep their logic; generated/default.qss styles their widgets.
void stylePreferences(QDialog* pDialog);

} // namespace crate
