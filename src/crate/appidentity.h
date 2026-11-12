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
// MIXXX_SETTINGS_PATH in CMakeLists.txt; kSettingsFolderName names it for
// the texts that show it.

/// Visible name of the application: window title, dialogs, JACK client.
inline constexpr char kApplicationName[] = "Crate Decks";
/// Product version of Crate Decks. Builds show it with the commit until there
/// is a decks-vX.Y.Z tag (buildVersion()). It is not VersionStore::version(),
/// the version of the inherited code, which Upgrade stores in the settings
/// file as the version of the settings.
inline constexpr char kApplicationVersion[] = "0.1.0";
inline constexpr char kOrganizationName[] = "Crate";
inline constexpr char kOrganizationDomain[] = "cratemusic.app";
/// Wayland app_id, which must match the basename of the .desktop file.
inline constexpr char kDesktopFileName[] = "app.cratemusic.decks";
/// The Linux settings folder in the home folder (MIXXX_SETTINGS_PATH).
inline constexpr char kSettingsFolderName[] = ".cratedecks";
/// Files of the settings folder (plan BR2), named after the executable like
/// the settings file (MIXXX_SETTINGS_FILE in config.h.in). The log rotates
/// to "<name>.1" up to "<name>.10".
inline constexpr char kLogFileName[] = "crate-decks.log";
inline constexpr char kDatabaseFileName[] = "crate-decks.sqlite";
/// Crate Decks reads and writes only its own settings folder (plan BR2):
/// upstream's migrations from its old settings folders would read, move and
/// delete the files of a Mixxx installed on the same computer.
inline constexpr bool kReadsMixxxSettings = false;
/// Folder of the installed data under share/ on Linux (MIXXX_INSTALL_DATADIR
/// in CMakeLists.txt), apart from an installed Mixxx's (plan BR2).
inline constexpr char kInstallDataDirName[] = "crate-decks";
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

/// The visible credits of what Crate Decks ships from others, for About, as
/// rich text with links in `linkStyle`: the Solar icons, whose CC BY 4.0
/// licence asks for attribution and to say what changed (design 14.3, plan
/// V2.PR5), and the admin's fonts under the SIL Open Font License.
QString attributionText(const QString& linkStyle);

/// Sets the organization, the desktop file name and the application version
/// on the Qt application.
/// Must be called before the QApplication is constructed.
void applyApplicationIdentity();

} // namespace crate
