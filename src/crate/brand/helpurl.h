#pragma once

#include <QList>
#include <QString>

namespace crate {

// Crate Decks' help links lead to Crate's own documentation, never to Mixxx's
// sites (plan "Own brand", BR.PR7, decision 7.1 of 2026-11-10). One table
// gives each link its page on docs.cratemusic.app: the URL macros of
// src/defs_urls.h read it (src/crate/brand/helpurlmacros.h), so the Help
// menu, every Preferences page and the error dialogs follow it without
// patching the code that shows them. Compiled always, like the rest of the
// brand layer.

/// The documentation site of Crate.
inline constexpr char kDocsUrl[] = "https://docs.cratemusic.app";

/// What a help link is about: one entry per page or section of the Crate
/// Decks pages of the documentation site.
enum class HelpLink {
    Manual,
    KeyboardShortcuts,
    CommandLineOptions,
    SettingsDirectory,
    HardwareCompatibility,
    Community,
    BugReports,
    Translations,
    ControllerForums,
    Library,
    TrackSearch,
    CueModes,
    SyncModes,
    Equalizers,
    BeatDetection,
    KeyDetection,
    Sound,
    SoundApi,
    OutputAndInputDevices,
    AudioLatency,
    Microphones,
    MicrophoneLatency,
    SoundTroubleshooting,
    Controllers,
    SupportedControllers,
    MappingFormat,
    ControllerScripting,
    Controls,
    ControllerEngineVersions,
    Broadcasting,
    VinylControl,
    VinylTroubleshooting,
};

/// Every kind of help link, in the order of the enum.
QList<HelpLink> helpLinks();

/// The page of the documentation site for `link`.
QString helpUrl(HelpLink link);

/// The start of the link to the help of a shipped controller mapping: the
/// mapping's manual page name and kControllerManualSuffix follow it.
QString controllerManualUrlPrefix();
inline constexpr char kControllerManualSuffix[] = "#supported-controllers";

/// `url`, a link that comes with the data Crate Decks ships (a controller
/// mapping's forum thread or wiki page), led to Crate's documentation when it
/// goes to one of Mixxx's sites; unchanged otherwise.
QString helpLink(const QString& url);

/// Whether opening `url` shows one of Mixxx's sites or documents: a web
/// address on Mixxx's hosts, forum, wiki or translation project, or a local
/// file named after Mixxx (Mixxx-Keyboard-Shortcuts.pdf).
bool leadsToMixxx(const QString& url);

} // namespace crate
