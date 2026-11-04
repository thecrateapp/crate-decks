#pragma once

#include "preferences/usersettings.h"

namespace crate {

class CrateServers;

/// Whether to offer connecting a Crate server: there is none yet and it was
/// not offered before on this installation.
bool shouldOfferFirstRun(const UserSettingsPointer& pConfig, const CrateServers& servers);

/// Remembers that connecting a server was offered on this installation.
void markFirstRunOffered(const UserSettingsPointer& pConfig);

/// Offers connecting a Crate server in a dialog, once per installation. Does
/// nothing on the offscreen platform, where nobody could answer it (tests).
void offerFirstRun(const UserSettingsPointer& pConfig, CrateServers* pServers);

} // namespace crate
