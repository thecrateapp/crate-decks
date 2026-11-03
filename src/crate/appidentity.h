#pragma once

namespace crate {

// Identity of the application, gathered in one place so that it can be
// reviewed and changed without touching the Mixxx code that uses it.

/// Visible name of the application: window title, dialogs, JACK client.
inline constexpr char kApplicationName[] = "Mixxx";
inline constexpr char kOrganizationName[] = "";
inline constexpr char kOrganizationDomain[] = "mixxx.org";
/// Wayland app_id, which must match the basename of the .desktop file.
inline constexpr char kDesktopFileName[] = "org.mixxx.Mixxx";
/// Prefix of the keychain service names of the broadcast profiles.
inline constexpr char kBroadcastKeychainPrefix[] = "Mixxx - ";

/// Sets the organization and the desktop file name on the Qt application.
/// Must be called before the QApplication is constructed.
void applyApplicationIdentity();

} // namespace crate
