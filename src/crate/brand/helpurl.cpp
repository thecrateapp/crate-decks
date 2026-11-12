#include "crate/brand/helpurl.h"

#include <QFileInfo>
#include <QUrl>
#include <array>
#include <utility>

namespace crate {

namespace {

/// The Crate Decks pages of the documentation site and their sections. The
/// anchors are the ids that the site gives the pages' headings (docs/decks/
/// in the Crate monorepo).
constexpr std::array kPages = {
        std::pair{HelpLink::Manual, "user-manual"},
        std::pair{HelpLink::KeyboardShortcuts, "keyboard-shortcuts"},
        std::pair{HelpLink::CommandLineOptions, "user-manual#command-line-options"},
        std::pair{HelpLink::SettingsDirectory, "user-manual#settings-directory"},
        std::pair{HelpLink::HardwareCompatibility, "user-manual#hardware-compatibility"},
        std::pair{HelpLink::Community, "community"},
        std::pair{HelpLink::BugReports, "community#reporting-bugs"},
        std::pair{HelpLink::Translations, "community#translations"},
        std::pair{HelpLink::ControllerForums, "community#sharing-controller-mappings"},
        std::pair{HelpLink::Library, "library"},
        std::pair{HelpLink::TrackSearch, "library#searching"},
        std::pair{HelpLink::CueModes, "mixing#cue-modes"},
        std::pair{HelpLink::SyncModes, "mixing#sync-lock"},
        std::pair{HelpLink::Equalizers, "mixing#equalizers"},
        std::pair{HelpLink::BeatDetection, "analysis#beat-detection"},
        std::pair{HelpLink::KeyDetection, "analysis#key-detection"},
        std::pair{HelpLink::Sound, "sound"},
        std::pair{HelpLink::SoundApi, "sound#sound-api"},
        std::pair{HelpLink::OutputAndInputDevices, "sound#output-and-input-devices"},
        std::pair{HelpLink::AudioLatency, "sound#latency"},
        std::pair{HelpLink::Microphones, "sound#microphones"},
        std::pair{HelpLink::MicrophoneLatency, "sound#microphone-latency-compensation"},
        std::pair{HelpLink::SoundTroubleshooting, "sound#troubleshooting"},
        std::pair{HelpLink::Controllers, "controllers"},
        std::pair{HelpLink::SupportedControllers, "controllers#supported-controllers"},
        std::pair{HelpLink::MappingFormat, "controllers#mapping-file-format"},
        std::pair{HelpLink::ControllerScripting, "controllers#scripting"},
        std::pair{HelpLink::Controls, "controllers#controls"},
        std::pair{HelpLink::ControllerEngineVersions, "controllers#controller-engine-versions"},
        std::pair{HelpLink::Broadcasting, "recording-and-broadcasting#broadcasting"},
        std::pair{HelpLink::VinylControl, "vinyl-control"},
        std::pair{HelpLink::VinylTroubleshooting, "vinyl-control#troubleshooting"},
};

QString decksPage(const char* page) {
    return QLatin1String(kDocsUrl) + QLatin1String("/decks/") + QLatin1String(page);
}

} // namespace

QList<HelpLink> helpLinks() {
    QList<HelpLink> links;
    links.reserve(static_cast<qsizetype>(kPages.size()));
    for (const auto& [link, page] : kPages) {
        links.append(link);
    }
    return links;
}

QString helpUrl(HelpLink link) {
    for (const auto& [kind, page] : kPages) {
        if (kind == link) {
            return decksPage(page);
        }
    }
    return decksPage("user-manual");
}

QString controllerManualUrlPrefix() {
    // The page ignores the query; it keeps the mapping in the address
    return decksPage("controllers?controller=");
}

QString helpLink(const QString& url) {
    if (!leadsToMixxx(url)) {
        return url;
    }
    const QString address = QUrl(url).host() + QUrl(url).path();
    if (address.contains(QLatin1String("discourse"), Qt::CaseInsensitive) ||
            address.contains(QLatin1String("forum"), Qt::CaseInsensitive)) {
        return helpUrl(HelpLink::ControllerForums);
    }
    return helpUrl(HelpLink::SupportedControllers);
}

bool leadsToMixxx(const QString& url) {
    const QUrl parsed(url);
    if (parsed.isLocalFile()) {
        // Mixxx's documents (Mixxx-Manual.pdf, Mixxx-Keyboard-Shortcuts.pdf);
        // folders have no suffix
        const QFileInfo file(parsed.toLocalFile());
        return !file.suffix().isEmpty() &&
                file.fileName().contains(QLatin1String("mixxx"), Qt::CaseInsensitive);
    }
    // mixxx.org and its subdomains, mixxx.discourse.group, github.com/mixxxdj
    // and the translation project mixxx-dj-software
    const QString address = parsed.host() + parsed.path();
    return address.contains(QLatin1String("mixxx"), Qt::CaseInsensitive);
}

} // namespace crate
