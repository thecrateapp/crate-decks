#pragma once

#include <QList>
#include <QString>
#include <functional>

#include "preferences/usersettings.h"

class QDialog;
class QWidget;

namespace mixxx {
class CoreServices;
} // namespace mixxx

namespace crate {

class CrateServers;

/// Whether to offer connecting a Crate server: there is none yet and it was
/// not offered before on this installation.
bool shouldOfferFirstRun(const UserSettingsPointer& pConfig, const CrateServers& servers);

/// Remembers that connecting a server was offered on this installation.
void markFirstRunOffered(const UserSettingsPointer& pConfig);

/// Whether Crate's first start is still to come on this installation. Mixxx's
/// own first-run questions (no output devices) wait for it: the first start
/// asks them in its pages (plan "Own brand", BR.PR3).
bool firstRunPending(const UserSettingsPointer& pConfig);

/// Decides Mixxx's "hide the menu bar?" question without asking: the
/// CrateDecks skin has no menu bar in its mockups, so it is hidden (Alt shows
/// it, Preferences › Interface changes it). A choice already made stays.
void decideMenuBar(const UserSettingsPointer& pConfig);

/// An output device that the first start's sound page offers.
struct FirstRunOutput {
    QString id;
    QString name;
};

/// What the first start's sound and music pages do in Mixxx. The application
/// builds them from CoreServices (firstRunServices()); tests give their own.
struct FirstRunServices {
    /// The output devices that the main output can use
    std::function<QList<FirstRunOutput>()> outputs;
    /// The ID of the device the main output uses, empty for none
    std::function<QString()> mainOutput;
    /// Uses the device `id` for the main output; false if it cannot open it
    std::function<bool(const QString& id)> setMainOutput;
    /// Searches the sound devices again
    std::function<void()> searchOutputs;
    /// Preferences on the sound hardware page, over the first start
    std::function<void()> showSoundPreferences;
    /// A folder that the user chooses, empty if cancelled
    std::function<QString(QWidget* pParent)> chooseFolder;
    /// Adds `folder` to the local library and scans it; false if it was not
    /// added
    std::function<bool(const QString& folder)> addMusicFolder;

    /// Whether the first start has the sound and music pages
    bool hasPages() const {
        return static_cast<bool>(outputs);
    }
};

/// The first start's services for the running application: the sound
/// manager's output devices, Preferences, and the library's folders.
FirstRunServices firstRunServices(mixxx::CoreServices* pCoreServices);

/// The first-start dialog of the GU1 mockups (first-run, plan V2.PR5):
/// the logo, "First start · Step n of 3", "Connect your Crate", the three
/// steps of adding a server (AddServerWidget) as lanes, and "Not now". It
/// accepts once a server is added and rejects on "Not now".
QDialog* createFirstRunDialog(CrateServers* pServers, QWidget* pParent);

/// The whole first start (plan "Own brand", BR.PR3): "Connect your Crate",
/// then the sound page (the main output, or a warning without devices) and
/// the optional local music page, which absorb Mixxx's first-run dialogs.
/// "Not now" and a connected server move on to the sound page; "Finish"
/// accepts. Without `services.hasPages()` it is the dialog above.
QDialog* createFirstRunDialog(
        CrateServers* pServers, const FirstRunServices& services, QWidget* pParent);

/// Whether the first-start dialog can show over `pMainWindow`: the main
/// window has its skin, no longer Mixxx's launch image, and no other modal
/// dialog is open, such as Mixxx's "No Output Devices".
bool firstRunCanShow(const QWidget* pMainWindow);

/// Offers connecting a Crate server in a dialog over `pParent`, once per
/// installation. Call it once the main window is shown: it waits until
/// firstRunCanShow(), since with OpenGL Mixxx loads the skin and the dialog
/// styles, and shows its own startup dialogs, only after the event loop
/// starts. Does nothing on the offscreen platform, where nobody could answer
/// it (tests).
void offerFirstRun(const UserSettingsPointer& pConfig, CrateServers* pServers, QWidget* pParent);

/// offerFirstRun() with the sound and music pages of `services`.
void offerFirstRun(const UserSettingsPointer& pConfig,
        CrateServers* pServers,
        const FirstRunServices& services,
        QWidget* pParent);

} // namespace crate
