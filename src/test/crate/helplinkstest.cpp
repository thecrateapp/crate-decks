#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QXmlStreamReader>
#include <utility>

#include "controllers/midi/legacymidicontrollermapping.h"
#include "crate/brand/brandtext.h"
#include "crate/brand/brandtranslator.h"
#include "crate/brand/helpurl.h"
#include "defs_urls.h"
#include "preferences/configobject.h"
#include "preferences/dialog/dlgpreferencepage.h"
#include "test/crate/offscreendecks.h"
#include "test/mixxxtest.h"

// Crate Decks' help links lead to Crate's own documentation (plan "Own
// brand", BR.PR7, decision 7.1): the table of crate::helpUrl(), the URL
// macros of defs_urls.h that read it, the links that come with the shipped
// controller mappings, and, in the real application, every link of the
// inventory's texts, the Help menu and the Preferences pages.
//
// TEST_GAP: the database upgrade error (mixxxdb.cpp) is a modal box at start
// and the per-mapping links of Preferences › Controllers only show with a
// controller plugged in; both use what this test checks (MIXXX_SUPPORT_URL,
// crate::helpLink()), not their dialogs.

namespace {

using crate::HelpLink;

const QString kDocs = QStringLiteral("https://docs.cratemusic.app/decks/");

/// The pages and anchors of the docs site that Crate Decks links to: the
/// contract with the "decks" section of docs.cratemusic.app (monorepo
/// docs/decks/), whose headings give these anchors.
const QList<std::pair<HelpLink, QString>>& contract() {
    static const QList<std::pair<HelpLink, QString>> kContract = {
            {HelpLink::Manual, QStringLiteral("user-manual")},
            {HelpLink::KeyboardShortcuts, QStringLiteral("keyboard-shortcuts")},
            {HelpLink::CommandLineOptions, QStringLiteral("user-manual#command-line-options")},
            {HelpLink::SettingsDirectory, QStringLiteral("user-manual#settings-directory")},
            {HelpLink::HardwareCompatibility,
                    QStringLiteral("user-manual#hardware-compatibility")},
            {HelpLink::Community, QStringLiteral("community")},
            {HelpLink::BugReports, QStringLiteral("community#reporting-bugs")},
            {HelpLink::Translations, QStringLiteral("community#translations")},
            {HelpLink::ControllerForums,
                    QStringLiteral("community#sharing-controller-mappings")},
            {HelpLink::Library, QStringLiteral("library")},
            {HelpLink::TrackSearch, QStringLiteral("library#searching")},
            {HelpLink::CueModes, QStringLiteral("mixing#cue-modes")},
            {HelpLink::SyncModes, QStringLiteral("mixing#sync-lock")},
            {HelpLink::Equalizers, QStringLiteral("mixing#equalizers")},
            {HelpLink::BeatDetection, QStringLiteral("analysis#beat-detection")},
            {HelpLink::KeyDetection, QStringLiteral("analysis#key-detection")},
            {HelpLink::Sound, QStringLiteral("sound")},
            {HelpLink::SoundApi, QStringLiteral("sound#sound-api")},
            {HelpLink::OutputAndInputDevices, QStringLiteral("sound#output-and-input-devices")},
            {HelpLink::AudioLatency, QStringLiteral("sound#latency")},
            {HelpLink::Microphones, QStringLiteral("sound#microphones")},
            {HelpLink::MicrophoneLatency,
                    QStringLiteral("sound#microphone-latency-compensation")},
            {HelpLink::SoundTroubleshooting, QStringLiteral("sound#troubleshooting")},
            {HelpLink::Controllers, QStringLiteral("controllers")},
            {HelpLink::SupportedControllers, QStringLiteral("controllers#supported-controllers")},
            {HelpLink::MappingFormat, QStringLiteral("controllers#mapping-file-format")},
            {HelpLink::ControllerScripting, QStringLiteral("controllers#scripting")},
            {HelpLink::Controls, QStringLiteral("controllers#controls")},
            {HelpLink::ControllerEngineVersions,
                    QStringLiteral("controllers#controller-engine-versions")},
            {HelpLink::Broadcasting, QStringLiteral("recording-and-broadcasting#broadcasting")},
            {HelpLink::VinylControl, QStringLiteral("vinyl-control")},
            {HelpLink::VinylTroubleshooting, QStringLiteral("vinyl-control#troubleshooting")},
    };
    return kContract;
}

/// Every link of a rich text: its href attributes and bare web addresses.
QStringList linksOf(const QString& text) {
    static const QRegularExpression kHref(QStringLiteral("href\\s*=\\s*['\"]([^'\"]*)['\"]"),
            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kBare(QStringLiteral("(?<![='\"])https?://[^\\s<'\"]+"));
    QStringList links;
    for (const auto& match : kHref.globalMatch(text)) {
        links.append(match.captured(1));
    }
    for (const auto& match : kBare.globalMatch(text)) {
        links.append(match.captured(0));
    }
    return links;
}

TEST(CrateHelpUrlTest, EveryLinkHasItsCrateDocsPage) {
    QList<HelpLink> contracted;
    for (const auto& [link, page] : contract()) {
        EXPECT_QSTRING_EQ(kDocs + page, crate::helpUrl(link));
        EXPECT_TRUE(QUrl(crate::helpUrl(link)).isValid()) << qPrintable(page);
        contracted.append(link);
    }
    EXPECT_EQ(contracted, crate::helpLinks()) << "Every kind has its page in the contract";
}

TEST(CrateHelpUrlTest, MixxxUrlMacrosLeadToCrateDocs) {
    const QStringList urls = {
            MIXXX_SUPPORT_URL,
            MIXXX_TRANSLATION_URL,
            MIXXX_CONTROLLER_FORUMS_URL,
            MIXXX_WIKI_URL,
            MIXXX_WIKI_TROUBLESHOOTING_SOUND_URL,
            MIXXX_WIKI_HARDWARE_COMPATIBILITY_URL,
            MIXXX_WIKI_AUDIO_LATENCY_URL,
            MIXXX_WIKI_CONTROLLER_MAPPING_FORMAT_URL,
            MIXXX_WIKI_MIDI_SCRIPTING_URL,
            MIXXX_MANUAL_URL,
            MIXXX_MANUAL_SHORTCUTS_URL,
            MIXXX_MANUAL_COMMANDLINEOPTIONS_URL,
            MIXXX_MANUAL_CONTROLLERS_URL,
            MIXXX_MANUAL_CONTROLS_URL,
            MIXXX_MANUAL_SOUND_URL,
            MIXXX_MANUAL_SOUND_API_URL,
            MIXXX_MANUAL_LIBRARY_URL,
            MIXXX_MANUAL_CUE_MODES_URL,
            MIXXX_MANUAL_SYNC_MODES_URL,
            MIXXX_MANUAL_TRACK_SEARCH_URL,
            MIXXX_MANUAL_MIC_MONITOR_MODES_URL,
            MIXXX_MANUAL_MIC_LATENCY_URL,
            MIXXX_MANUAL_BEATS_URL,
            MIXXX_MANUAL_KEY_URL,
            MIXXX_MANUAL_EQ_URL,
            MIXXX_MANUAL_BROADCAST_URL,
            MIXXX_MANUAL_VINYL_URL,
            MIXXX_MANUAL_VINYL_TROUBLESHOOTING_URL,
            MIXXX_MANUAL_SETTINGS_DIRECTORY_URL,
            MIXXX_MANUAL_OUTPUT_AND_INPUT_DEVICES,
    };
    for (const QString& url : urls) {
        EXPECT_TRUE(url.startsWith(kDocs)) << qPrintable(url);
        EXPECT_FALSE(crate::leadsToMixxx(url)) << qPrintable(url);
    }

    // The manual of a shipped controller mapping
    LegacyMidiControllerMapping mapping;
    mapping.setManualPage(QStringLiteral("pioneer_ddj_400"));
    EXPECT_QSTRING_EQ(kDocs +
                    QStringLiteral("controllers?controller=pioneer_ddj_400"
                                   "#supported-controllers"),
            mapping.manualLink());

    // The Help menu opens a local PDF when it finds one by these names: never
    // Mixxx's (res/Mixxx-Keyboard-Shortcuts.pdf, which CMake still installs)
    EXPECT_FALSE(crate::leadsToMixxx(QUrl::fromLocalFile(
            QDir::temp().filePath(QStringLiteral(MIXXX_MANUAL_FILENAME)))
                    .toString()));
    EXPECT_FALSE(crate::leadsToMixxx(QUrl::fromLocalFile(
            QDir::temp().filePath(QStringLiteral(MIXXX_KBD_SHORTCUTS_FILENAME)))
                    .toString()));
}

TEST(CrateHelpUrlTest, KnowsWhatLeadsToMixxx) {
    for (const char* url : {"https://www.mixxx.org/support/",
                 "https://manual.mixxx.org/2.7/chapters/library.html",
                 "https://mixxx.discourse.group/t/pioneer-ddj-400/17476",
                 "https://github.com/mixxxdj/mixxx/wiki/Hardware-Compatibility",
                 "https://explore.transifex.com/mixxx-dj-software/",
                 "file:///usr/share/doc/mixxx/Mixxx-Keyboard-Shortcuts.pdf"}) {
        EXPECT_TRUE(crate::leadsToMixxx(QString::fromLatin1(url))) << url;
    }
    for (const char* url : {"https://docs.cratemusic.app/decks/sound",
                 "https://cratemusic.app",
                 "https://github.com/thecrateapp/crate/issues",
                 "file:///home/dj/.cratedecks/"}) {
        EXPECT_FALSE(crate::leadsToMixxx(QString::fromLatin1(url))) << url;
    }
}

TEST(CrateHelpUrlTest, ShippedMappingLinksLeadToCrateDocs) {
    const QString controllers =
            ConfigObject<ConfigValue>::computeResourcePath() + QStringLiteral("controllers");
    QDirIterator it(controllers, {QStringLiteral("*.xml")}, QDir::Files);
    int links = 0;
    while (it.hasNext()) {
        QFile file(it.next());
        ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << qPrintable(file.fileName());
        QXmlStreamReader xml(&file);
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() &&
                    (xml.name() == QLatin1String("forums") ||
                            xml.name() == QLatin1String("wiki"))) {
                const QString url = xml.readElementText().trimmed();
                if (url.isEmpty()) {
                    continue;
                }
                ++links;
                const QString shown = crate::helpLink(url);
                EXPECT_TRUE(shown.startsWith(kDocs))
                        << qPrintable(file.fileName()) << ": " << qPrintable(url);
            }
        }
    }
    EXPECT_GT(links, 100);
}

/// Records every URL the application opens instead of opening it.
class OpenedUrls : public QObject {
    Q_OBJECT
  public:
    OpenedUrls() {
        for (const char* scheme : {"http", "https", "file"}) {
            QDesktopServices::setUrlHandler(QString::fromLatin1(scheme), this, "open");
        }
    }
    ~OpenedUrls() override {
        for (const char* scheme : {"http", "https", "file"}) {
            QDesktopServices::unsetUrlHandler(QString::fromLatin1(scheme));
        }
    }
    QStringList urls;

  public slots:
    void open(const QUrl& url) {
        urls.append(url.toString());
    }
};

class CrateBrandTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pBrand = crate::installBrandTranslator(application());
    }
    void TearDown() override {
        delete m_pBrand;
    }

  private:
    crate::BrandTranslator* m_pBrand = nullptr;
};

QWidget* topLevelOf(const char* className) {
    const QWidgetList windows = QApplication::topLevelWidgets();
    for (QWidget* pWindow : windows) {
        if (pWindow->inherits(className)) {
            return pWindow;
        }
    }
    return nullptr;
}

TEST_F(CrateBrandTest, NoLinkLeadsToMixxx) {
    // Where each link was found, and the link
    QList<std::pair<QString, QString>> found;

    // The inventory's texts, as Crate Decks shows them in English and Spanish;
    // the credits it keeps in About (design D2) stay out
    const crate::BrandInventory& inventory = crate::BrandInventory::instance();
    const QStringList sources = inventory.sources();
    for (const QString& source : sources) {
        if (inventory.string(source)->treatment == crate::BrandInventory::Treatment::Keep) {
            continue;
        }
        for (bool spanish : {false, true}) {
            QString shown = crate::brandedTranslation(source, QString(), spanish);
            if (shown.isNull()) {
                shown = source;
            }
            const QStringList links = linksOf(shown);
            for (const QString& link : links) {
                found.append({QStringLiteral("inventory: ") + source.left(60), link});
            }
        }
    }

    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    crate::test::OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(QStringLiteral("CrateDecks"));
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());

    // The Help menu: every action, triggered, but About
    OpenedUrls opened;
    QMenu* pHelp = nullptr;
    ASSERT_NE(nullptr, decks.mainWindow()->menuBar());
    const QList<QAction*> menus = decks.mainWindow()->menuBar()->actions();
    for (QAction* pMenuAction : menus) {
        QMenu* pMenu = pMenuAction->menu();
        if (!pMenu) {
            continue;
        }
        emit pMenu->aboutToShow();
        const QList<QAction*> actions = pMenu->actions();
        for (QAction* pAction : actions) {
            if (pAction->menuRole() == QAction::AboutRole) {
                pHelp = pMenu;
            }
        }
    }
    ASSERT_NE(nullptr, pHelp);
    const QList<QAction*> helpActions = pHelp->actions();
    for (QAction* pAction : helpActions) {
        if (pAction->isSeparator() || pAction->menu() ||
                pAction->menuRole() == QAction::AboutRole) {
            continue;
        }
        opened.urls.clear();
        pAction->trigger();
        for (const QString& url : std::as_const(opened.urls)) {
            found.append({QStringLiteral("Help menu: ") + pAction->text(), url});
        }
    }

    // Every Preferences page: its help button and the links of its labels
    auto* pPreferences = qobject_cast<QDialog*>(topLevelOf("DlgPreferences"));
    ASSERT_NE(nullptr, pPreferences);
    auto* pTree = pPreferences->findChild<QTreeWidget*>(QStringLiteral("contentsTreeWidget"));
    ASSERT_NE(nullptr, pTree);
    for (QTreeWidgetItemIterator it(pTree); *it; ++it) {
        pTree->setCurrentItem(*it);
        QCoreApplication::processEvents();
    }
    const QList<DlgPreferencePage*> pages = pPreferences->findChildren<DlgPreferencePage*>();
    EXPECT_GT(pages.size(), 15);
    for (DlgPreferencePage* pPage : pages) {
        const QString where = QStringLiteral("Preferences ") +
                QString::fromLatin1(pPage->metaObject()->className());
        if (pPage->helpUrl().isValid()) {
            found.append({where + QStringLiteral(" (help)"), pPage->helpUrl().toString()});
        }
        const QList<QLabel*> labels = pPage->findChildren<QLabel*>();
        for (QLabel* pLabel : labels) {
            const QStringList links = linksOf(pLabel->text());
            for (const QString& link : links) {
                found.append({where, link});
            }
        }
    }

    int help = 0;
    QStringList mixxx;
    for (const auto& [where, link] : std::as_const(found)) {
        if (link.startsWith(kDocs)) {
            ++help;
        }
        if (crate::leadsToMixxx(link)) {
            mixxx.append(where + QStringLiteral(": ") + link);
        }
    }
    EXPECT_GT(help, 25) << "Links to Crate's docs found";
    EXPECT_TRUE(mixxx.isEmpty()) << mixxx.size() << " links lead to Mixxx:\n"
                                 << qPrintable(mixxx.join(QLatin1Char('\n')));
}

} // namespace

#include "helplinkstest.moc"
