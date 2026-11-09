#pragma once

#include "preferences/usersettings.h"

class QDialog;
class QWidget;

namespace crate {

class CrateServers;

/// Whether to offer connecting a Crate server: there is none yet and it was
/// not offered before on this installation.
bool shouldOfferFirstRun(const UserSettingsPointer& pConfig, const CrateServers& servers);

/// Remembers that connecting a server was offered on this installation.
void markFirstRunOffered(const UserSettingsPointer& pConfig);

/// The first-start dialog of the GU1 mockups (first-run, plan V2.PR5):
/// the logo, "First start · Step n of 3", "Connect your Crate", the three
/// steps of adding a server (AddServerWidget) as lanes, and "Not now". It
/// accepts once a server is added and rejects on "Not now".
QDialog* createFirstRunDialog(CrateServers* pServers, QWidget* pParent);

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

} // namespace crate
