#pragma once

#include <QString>
#include <QUrl>

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
/// Product version of Crate Decks. Builds show it with the commit until there
/// is a decks-vX.Y.Z tag (buildVersion()). It is not VersionStore::version(),
/// which stays Mixxx's because Upgrade stores it in mixxx.cfg as the version
/// of the settings.
inline constexpr char kApplicationVersion[] = "0.1.0";
inline constexpr char kOrganizationName[] = "Crate";
inline constexpr char kOrganizationDomain[] = "cratemusic.app";
/// Wayland app_id, which must match the basename of the .desktop file.
inline constexpr char kDesktopFileName[] = "app.cratemusic.decks";
/// Prefix of the keychain service names of the broadcast profiles.
inline constexpr char kBroadcastKeychainPrefix[] = "Crate - ";

/// Website of Crate, linked from the About dialog.
inline constexpr char kHomepageUrl[] = "https://cratemusic.app";
/// Repository with the source code of Crate Decks (GPL, design D2).
inline constexpr char kSourceRepositoryUrl[] = "https://github.com/thecrateapp/crate-decks";

/// Link to the source code of the build described by `git describe`
/// (VersionStore::gitDescribe()): the tag, the commit or, if unknown, the
/// repository.
QUrl sourceCodeUrl(const QString& gitDescribe);

/// Version of the build described by `git describe`
/// (VersionStore::gitDescribe()), shown by About and --version.
/// GitInfo.cmake only describes the build with decks-vX.Y.Z tags, so it is:
/// - from a tag, the version of the tag: "0.1.0", or "0.1.0-5-g<hash>" for
///   five commits after it;
/// - without one, the product version and the commit: "0.1.0-dev+g<hash>";
/// - if unknown, "0.1.0-unknown".
/// "-modified" marks a modified work tree. packaging/CPackConfig.cmake
/// computes the same version for the package file names.
QString buildVersion(const QString& gitDescribe);

/// Sets the organization, the desktop file name and the application version
/// on the Qt application.
/// Must be called before the QApplication is constructed.
void applyApplicationIdentity();

} // namespace crate
