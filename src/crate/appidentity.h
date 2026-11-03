#pragma once

namespace crate {

// Identity of Crate Decks as an application (design D3), gathered in one
// place so that it can be reviewed and changed without touching the Mixxx
// code that uses it. It differs from Mixxx's so that Crate Decks never shares
// settings, the desktop entry or keychain entries with an installed Mixxx.
//
// The Linux settings directory (~/.cratedecks/) is fixed at compile time by
// MIXXX_SETTINGS_PATH in CMakeLists.txt.

/// Visible name of the application: window title, dialogs, JACK client.
inline constexpr char kApplicationName[] = "Crate Decks";
inline constexpr char kOrganizationName[] = "Crate";
inline constexpr char kOrganizationDomain[] = "cratemusic.app";
/// Wayland app_id, which must match the basename of the .desktop file.
inline constexpr char kDesktopFileName[] = "app.cratemusic.decks";
/// Prefix of the keychain service names of the broadcast profiles.
inline constexpr char kBroadcastKeychainPrefix[] = "Crate - ";

/// Sets the organization and the desktop file name on the Qt application.
/// Must be called before the QApplication is constructed.
void applyApplicationIdentity();

} // namespace crate
