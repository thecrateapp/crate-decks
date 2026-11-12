#pragma once

// Crate Decks: the help links of Mixxx's code lead to Crate's documentation
// (plan "Own brand", BR.PR7). src/defs_urls.h includes this file after its
// own definitions, so every use of these macros (Help menu, Preferences,
// error dialogs, --help) reads crate::helpUrl() without patching the code
// that shows them. MIXXX_WEBSITE_URL stays: the credits in About (design D2)
// and MusicBrainz' user agent name the project the code comes from.

#include <QString>

#include "crate/brand/helpurl.h"

#undef MIXXX_SUPPORT_URL
#undef MIXXX_TRANSLATION_URL
#undef MIXXX_CONTROLLER_FORUMS_URL
#undef MIXXX_WIKI_URL
#undef MIXXX_WIKI_TROUBLESHOOTING_SOUND_URL
#undef MIXXX_WIKI_HARDWARE_COMPATIBILITY_URL
#undef MIXXX_WIKI_AUDIO_LATENCY_URL
#undef MIXXX_WIKI_CONTROLLER_MAPPING_FORMAT_URL
#undef MIXXX_WIKI_MIDI_SCRIPTING_URL
#undef MIXXX_MANUAL_URL
#undef MIXXX_MANUAL_SHORTCUTS_URL
#undef MIXXX_MANUAL_COMMANDLINEOPTIONS_URL
#undef MIXXX_MANUAL_CONTROLLERS_URL
#undef MIXXX_MANUAL_CONTROLLERMANUAL_PREFIX
#undef MIXXX_MANUAL_CONTROLLERMANUAL_SUFFIX
#undef MIXXX_MANUAL_CONTROLS_URL
#undef MIXXX_MANUAL_SOUND_URL
#undef MIXXX_MANUAL_SOUND_API_URL
#undef MIXXX_MANUAL_LIBRARY_URL
#undef MIXXX_MANUAL_CUE_MODES_URL
#undef MIXXX_MANUAL_SYNC_MODES_URL
#undef MIXXX_MANUAL_TRACK_SEARCH_URL
#undef MIXXX_MANUAL_MIC_MONITOR_MODES_URL
#undef MIXXX_MANUAL_MIC_LATENCY_URL
#undef MIXXX_MANUAL_BEATS_URL
#undef MIXXX_MANUAL_KEY_URL
#undef MIXXX_MANUAL_EQ_URL
#undef MIXXX_MANUAL_BROADCAST_URL
#undef MIXXX_MANUAL_VINYL_URL
#undef MIXXX_MANUAL_VINYL_TROUBLESHOOTING_URL
#undef MIXXX_MANUAL_SETTINGS_DIRECTORY_URL
#undef MIXXX_MANUAL_OUTPUT_AND_INPUT_DEVICES
#undef MIXXX_MANUAL_FILENAME
#undef MIXXX_KBD_SHORTCUTS_FILENAME
// No donation link (decision 7.2): a new use fails to build
#undef MIXXX_DONATE_URL

#define MIXXX_SUPPORT_URL crate::helpUrl(crate::HelpLink::Community)
#define MIXXX_TRANSLATION_URL crate::helpUrl(crate::HelpLink::Translations)
#define MIXXX_CONTROLLER_FORUMS_URL crate::helpUrl(crate::HelpLink::ControllerForums)
#define MIXXX_WIKI_URL crate::helpUrl(crate::HelpLink::Manual)
#define MIXXX_WIKI_TROUBLESHOOTING_SOUND_URL \
    crate::helpUrl(crate::HelpLink::SoundTroubleshooting)
#define MIXXX_WIKI_HARDWARE_COMPATIBILITY_URL \
    crate::helpUrl(crate::HelpLink::HardwareCompatibility)
#define MIXXX_WIKI_AUDIO_LATENCY_URL crate::helpUrl(crate::HelpLink::AudioLatency)
#define MIXXX_WIKI_CONTROLLER_MAPPING_FORMAT_URL crate::helpUrl(crate::HelpLink::MappingFormat)
#define MIXXX_WIKI_MIDI_SCRIPTING_URL crate::helpUrl(crate::HelpLink::ControllerScripting)
#define MIXXX_MANUAL_URL crate::helpUrl(crate::HelpLink::Manual)
#define MIXXX_MANUAL_SHORTCUTS_URL crate::helpUrl(crate::HelpLink::KeyboardShortcuts)
#define MIXXX_MANUAL_COMMANDLINEOPTIONS_URL crate::helpUrl(crate::HelpLink::CommandLineOptions)
#define MIXXX_MANUAL_CONTROLLERS_URL crate::helpUrl(crate::HelpLink::Controllers)
#define MIXXX_MANUAL_CONTROLLERMANUAL_PREFIX crate::controllerManualUrlPrefix()
#define MIXXX_MANUAL_CONTROLLERMANUAL_SUFFIX crate::kControllerManualSuffix
#define MIXXX_MANUAL_CONTROLS_URL crate::helpUrl(crate::HelpLink::Controls)
#define MIXXX_MANUAL_SOUND_URL crate::helpUrl(crate::HelpLink::Sound)
#define MIXXX_MANUAL_SOUND_API_URL crate::helpUrl(crate::HelpLink::SoundApi)
#define MIXXX_MANUAL_LIBRARY_URL crate::helpUrl(crate::HelpLink::Library)
#define MIXXX_MANUAL_CUE_MODES_URL crate::helpUrl(crate::HelpLink::CueModes)
#define MIXXX_MANUAL_SYNC_MODES_URL crate::helpUrl(crate::HelpLink::SyncModes)
#define MIXXX_MANUAL_TRACK_SEARCH_URL crate::helpUrl(crate::HelpLink::TrackSearch)
#define MIXXX_MANUAL_MIC_MONITOR_MODES_URL crate::helpUrl(crate::HelpLink::Microphones)
#define MIXXX_MANUAL_MIC_LATENCY_URL crate::helpUrl(crate::HelpLink::MicrophoneLatency)
#define MIXXX_MANUAL_BEATS_URL crate::helpUrl(crate::HelpLink::BeatDetection)
#define MIXXX_MANUAL_KEY_URL crate::helpUrl(crate::HelpLink::KeyDetection)
#define MIXXX_MANUAL_EQ_URL crate::helpUrl(crate::HelpLink::Equalizers)
#define MIXXX_MANUAL_BROADCAST_URL crate::helpUrl(crate::HelpLink::Broadcasting)
#define MIXXX_MANUAL_VINYL_URL crate::helpUrl(crate::HelpLink::VinylControl)
#define MIXXX_MANUAL_VINYL_TROUBLESHOOTING_URL \
    crate::helpUrl(crate::HelpLink::VinylTroubleshooting)
#define MIXXX_MANUAL_SETTINGS_DIRECTORY_URL crate::helpUrl(crate::HelpLink::SettingsDirectory)
#define MIXXX_MANUAL_OUTPUT_AND_INPUT_DEVICES \
    crate::helpUrl(crate::HelpLink::OutputAndInputDevices)
// The Help menu opens a PDF with these names next to the resources when there
// is one, else the page. Crate Decks ships none, so they are never Mixxx's
// manual or keyboard shortcuts (res/Mixxx-Keyboard-Shortcuts.pdf)
#define MIXXX_MANUAL_FILENAME "Crate-Decks-Manual.pdf"
#define MIXXX_KBD_SHORTCUTS_FILENAME "Crate-Decks-Keyboard-Shortcuts.pdf"
