#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QUrl>

#include "crate/appidentity.h"
#include "defs_urls.h"
#include "dialog/dlgabout.h"
#ifdef __BROADCAST__
#include "preferences/broadcastprofile.h"
#endif
#include "preferences/configobject.h"
#include "preferences/dialog/dlgprefrecord.h"
#include "recording/defs_recording.h"
#include "test/mixxxtest.h"
#include "util/cmdlineargs.h"
#include "util/versionstore.h"

namespace {

const QString kMixxxSettingsDir = QStringLiteral(".mixxx");

QDir resourceDir() {
    return QDir(ConfigObject<ConfigValue>::computeResourcePath());
}

/// Keys of the [Desktop Entry] group of a freedesktop .desktop file.
QHash<QString, QString> readDesktopEntry(const QString& path) {
    QHash<QString, QString> entry;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return entry;
    }
    bool inDesktopEntry = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QChar('['))) {
            inDesktopEntry = line == QStringLiteral("[Desktop Entry]");
            continue;
        }
        const int separator = line.indexOf(QChar('='));
        if (inDesktopEntry && separator > 0) {
            entry.insert(line.left(separator).trimmed(), line.mid(separator + 1).trimmed());
        }
    }
    return entry;
}

QString appIconPath(int size) {
    return resourceDir().filePath(QStringLiteral("images/icons/%1x%1/apps/%2.png")
                    .arg(QString::number(size), QString::fromUtf8(crate::kDesktopFileName)));
}

class CrateBrandingTest : public testing::Test {
  protected:
    void SetUp() override {
        m_organizationName = QCoreApplication::organizationName();
        m_organizationDomain = QCoreApplication::organizationDomain();
        m_desktopFileName = QGuiApplication::desktopFileName();
        m_applicationVersion = QCoreApplication::applicationVersion();
    }

    void TearDown() override {
        // Other tests must keep seeing the identity of mixxx-test.
        QCoreApplication::setOrganizationName(m_organizationName);
        QCoreApplication::setOrganizationDomain(m_organizationDomain);
        QGuiApplication::setDesktopFileName(m_desktopFileName);
        QCoreApplication::setApplicationVersion(m_applicationVersion);
    }

  private:
    QString m_organizationName;
    QString m_organizationDomain;
    QString m_desktopFileName;
    QString m_applicationVersion;
};

TEST_F(CrateBrandingTest, ApplicationNameIsCrateDecks) {
    EXPECT_QSTRING_EQ(QStringLiteral("Crate Decks"), VersionStore::applicationName());
}

TEST_F(CrateBrandingTest, ApplyApplicationIdentitySetsCrateIdentity) {
    crate::applyApplicationIdentity();

    EXPECT_QSTRING_EQ(QStringLiteral("Crate"), QCoreApplication::organizationName());
    EXPECT_QSTRING_EQ(QStringLiteral("cratemusic.app"), QCoreApplication::organizationDomain());
#ifdef __LINUX__
    EXPECT_QSTRING_EQ(QStringLiteral("app.cratemusic.decks"), QGuiApplication::desktopFileName());
#endif
    // QCommandLineParser prints this version for --version.
    EXPECT_QSTRING_EQ(crate::buildVersion(VersionStore::gitDescribe()),
            QCoreApplication::applicationVersion());
}

TEST_F(CrateBrandingTest, ProductVersionDoesNotReplaceTheConfigVersion) {
    // Upgrade::versionUpgrade() stores VersionStore::version() in mixxx.cfg
    // and compares it with Mixxx versions, so it must stay Mixxx's.
    EXPECT_QSTRING_EQ(QStringLiteral("0.1.0"), QString::fromUtf8(crate::kApplicationVersion));
    EXPECT_NE(QString::fromUtf8(crate::kApplicationVersion), VersionStore::version());
}

TEST_F(CrateBrandingTest, BuildVersionComesFromDecksTags) {
    const QString product = QString::fromUtf8(crate::kApplicationVersion);
    const struct {
        QString gitDescribe;
        QString version;
    } cases[] = {
            // Built from a decks-vX.Y.Z tag, or from commits after one.
            {QStringLiteral("decks-v0.1.0"), QStringLiteral("0.1.0")},
            {QStringLiteral("decks-v0.2.0-beta"), QStringLiteral("0.2.0-beta")},
            {QStringLiteral("decks-v0.1.0-modified"), QStringLiteral("0.1.0-modified")},
            {QStringLiteral("decks-v0.1.0-5-ge9ea8313bd"),
                    QStringLiteral("0.1.0-5-ge9ea8313bd")},
            {QStringLiteral("decks-v0.1.0-5-ge9ea8313bd-modified"),
                    QStringLiteral("0.1.0-5-ge9ea8313bd-modified")},
            // No decks-v tag yet: the product version and the commit.
            {QStringLiteral("e9ea8313bd"), product + QStringLiteral("-dev+ge9ea8313bd")},
            {QStringLiteral("e9ea8313bd-modified"),
                    product + QStringLiteral("-dev+ge9ea8313bd-modified")},
            // Unknown, which a Mixxx version also is.
            {QString(), product + QStringLiteral("-unknown")},
            {QStringLiteral("2.7-alpha-463-gbc801a2dfe"), product + QStringLiteral("-unknown")},
    };
    for (const auto& testCase : cases) {
        SCOPED_TRACE(qPrintable(testCase.gitDescribe));
        EXPECT_QSTRING_EQ(testCase.version, crate::buildVersion(testCase.gitDescribe));
    }
}

// TEST_GAP: the package file names come from packaging/CPackConfig.cmake,
// which computes the version like crate::buildVersion() and is not visible to
// the tests. The AppImage job of .github/workflows/crate-decks-build.yml
// checks that the AppImage is named after the version that --version prints.
TEST_F(CrateBrandingTest, BuildIsVersionedOnlyByDecksTags) {
    const QString gitDescribe = VersionStore::gitDescribe();
    if (gitDescribe.isEmpty()) {
        GTEST_SKIP() << "Built without git information";
    }
    const QString version = crate::buildVersion(gitDescribe);

    // The Mixxx tags in the history (2.7-alpha) do not version the build.
    const QString mixxxVersion = QStringLiteral("%1.%2").arg(
            QString::number(VersionStore::versionNumber().majorVersion()),
            QString::number(VersionStore::versionNumber().minorVersion()));
    EXPECT_FALSE(gitDescribe.startsWith(mixxxVersion)) << qPrintable(gitDescribe);
    EXPECT_FALSE(version.startsWith(mixxxVersion)) << qPrintable(version);
    const QString tagPrefix = QStringLiteral("decks-v");
    if (gitDescribe.startsWith(tagPrefix)) {
        EXPECT_QSTRING_EQ(gitDescribe.mid(tagPrefix.size()), version);
    } else {
        // Without a decks-v tag, `git describe --always` gives the commit.
        static const QRegularExpression kCommitOnly(QStringLiteral("^[0-9a-f]+(-modified)?$"));
        EXPECT_TRUE(kCommitOnly.match(gitDescribe).hasMatch()) << qPrintable(gitDescribe);
        EXPECT_QSTRING_EQ(QStringLiteral("0.1.0-dev+g") + gitDescribe, version);
    }
}

TEST_F(CrateBrandingTest, DefaultSettingsPathIsNotMixxxs) {
#if defined(__LINUX__) || defined(__BSD__)
    const CmdlineArgs args;

    EXPECT_FALSE(args.getSettingsPathSet());
    EXPECT_QSTRING_EQ(QDir::homePath() + QStringLiteral("/.cratedecks/"), args.getSettingsPath());
    EXPECT_FALSE(args.getSettingsPath().contains(kMixxxSettingsDir));
#else
    // TEST_GAP: on macOS and Windows the default comes from
    // QStandardPaths::AppLocalDataLocation, which follows the application
    // name that main() sets; mixxx-test runs with its own name.
    GTEST_SKIP() << "The default settings path is only fixed at compile time on Linux";
#endif
}

TEST_F(CrateBrandingTest, SettingsPathOptionOverridesDefault) {
#ifdef MIXXX_DEBUG_ASSERTIONS_ENABLED
    // TEST_GAP: CmdlineArgs::parse() asserts that there is no
    // QCoreApplication yet, and mixxx-test always has one. The smoke test
    // with --settingsPath covers the option in debug builds.
    GTEST_SKIP() << "CmdlineArgs::parse() requires that no QCoreApplication exists";
#else
    const QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    QByteArray program = QByteArrayLiteral("crate-decks");
    QByteArray path = QDir::toNativeSeparators(settingsDir.path()).toLocal8Bit();

    for (const char* option : {"--settings-path", "--settingsPath"}) {
        SCOPED_TRACE(option);
        QByteArray optionName(option);
        char* argv[] = {program.data(), optionName.data(), path.data()};
        CmdlineArgs args;

        ASSERT_TRUE(args.parse(3, argv));

        EXPECT_TRUE(args.getSettingsPathSet());
        EXPECT_QSTRING_EQ(QDir::toNativeSeparators(settingsDir.path() + QStringLiteral("/")),
                args.getSettingsPath());
    }
#endif
}

TEST_F(CrateBrandingTest, BroadcastKeychainPrefixIsNotMixxxs) {
    EXPECT_STREQ("Crate - ", crate::kBroadcastKeychainPrefix);
}

// Plan "Own brand", BR.PR2: what leaves the window names Crate Decks. A new
// broadcast profile streams as the application, with Crate's website, which
// listeners and Preferences see.
TEST_F(CrateBrandingTest, DefaultStreamNameIsTheApplication) {
#ifdef __BROADCAST__
    const BroadcastProfile profile(QStringLiteral("Test"));
    EXPECT_QSTRING_EQ(QStringLiteral("Crate Decks"), profile.getStreamName());
    EXPECT_QSTRING_EQ(QStringLiteral("https://cratemusic.app"), profile.getStreamWebsite());
#else
    GTEST_SKIP() << "Built without broadcasting";
#endif
}

// Plan "Own brand", BR.PR2: the default recordings folder, which Preferences
// shows and the file manager too, is named after the application
TEST_F(CrateBrandingTest, RecordingsFolderIsNamedAfterTheApplication) {
    QTemporaryDir settings;
    ASSERT_TRUE(settings.isValid());
    auto pConfig = UserSettingsPointer(
            new UserSettings(settings.filePath(QStringLiteral("mixxx.cfg"))));
    const DlgPrefRecord page(nullptr, pConfig);
    const QString folder = pConfig->getValueString(ConfigKey(RECORDING_PREF_KEY, "Directory"));
    EXPECT_TRUE(folder.endsWith(QStringLiteral("/Crate Decks/Recordings"))) << qPrintable(folder);
    EXPECT_FALSE(folder.contains(QLatin1String("Mixxx"))) << qPrintable(folder);
}

// TEST_GAP: whether the icon looks right is a human decision (gate G1).
TEST_F(CrateBrandingTest, AppIconExistsInEveryInstalledSize) {
    for (const int size : {32, 64, 128, 256, 512}) {
        const QString path = appIconPath(size);
        SCOPED_TRACE(qPrintable(path));
        const QImage icon(path);

        ASSERT_FALSE(icon.isNull());
        EXPECT_EQ(QSize(size, size), icon.size());
    }
}

TEST_F(CrateBrandingTest, WindowIconIsTheAppIcon) {
    const QImage windowIcon(QStringLiteral(MIXXX_ICON_PATH));
    const QImage appIcon(appIconPath(256));
    ASSERT_FALSE(windowIcon.isNull());
    ASSERT_FALSE(appIcon.isNull());

    EXPECT_EQ(appIcon.size(), windowIcon.size());
    EXPECT_TRUE(appIcon.convertToFormat(QImage::Format_ARGB32) ==
            windowIcon.convertToFormat(QImage::Format_ARGB32))
            << "MIXXX_ICON_PATH is " << MIXXX_ICON_PATH;
}

TEST_F(CrateBrandingTest, AboutShowsAppIconAndNoMixxxLogo) {
    const DlgAbout about;

    const auto* pMixxxLogo = about.findChild<QWidget*>(QStringLiteral("mixxx_logo"));
    ASSERT_NE(nullptr, pMixxxLogo);
    EXPECT_TRUE(pMixxxLogo->isHidden());
    const auto* pAppIcon = about.findChild<QLabel*>(QStringLiteral("crate_app_icon"));
    ASSERT_NE(nullptr, pAppIcon);
    EXPECT_FALSE(pAppIcon->pixmap().isNull());
}

// TEST_GAP: the executable name (OUTPUT_NAME in CMakeLists.txt) is not
// visible to the tests; `cmake --install` shows bin/crate-decks.
TEST_F(CrateBrandingTest, DesktopEntryMatchesDesktopFileName) {
    const QString desktopFileName = QString::fromUtf8(crate::kDesktopFileName);
    const QString path = resourceDir().filePath(
            QStringLiteral("linux/%1.desktop").arg(desktopFileName));
    const QHash<QString, QString> entry = readDesktopEntry(path);
    ASSERT_FALSE(entry.isEmpty()) << qPrintable(path);

    EXPECT_QSTRING_EQ(QStringLiteral("Application"), entry.value(QStringLiteral("Type")));
    EXPECT_QSTRING_EQ(QStringLiteral("Crate Decks"), entry.value(QStringLiteral("Name")));
    EXPECT_QSTRING_EQ(desktopFileName, entry.value(QStringLiteral("Icon")));
    const QString exec = entry.value(QStringLiteral("Exec"));
    EXPECT_TRUE(exec.contains(QStringLiteral("crate-decks"))) << qPrintable(exec);
    EXPECT_FALSE(exec.contains(QStringLiteral("mixxx"))) << qPrintable(exec);
}

TEST_F(CrateBrandingTest, MetainfoMatchesDesktopFileName) {
    const QString desktopFileName = QString::fromUtf8(crate::kDesktopFileName);
    const QString path = resourceDir().filePath(
            QStringLiteral("linux/%1.metainfo.xml").arg(desktopFileName));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << qPrintable(path);
    QDomDocument metainfo;
    ASSERT_TRUE(metainfo.setContent(&file));
    const QDomElement component = metainfo.documentElement();

    EXPECT_QSTRING_EQ(desktopFileName,
            component.firstChildElement(QStringLiteral("id")).text());
    EXPECT_QSTRING_EQ(desktopFileName + QStringLiteral(".desktop"),
            component.firstChildElement(QStringLiteral("launchable")).text());
    EXPECT_QSTRING_EQ(QStringLiteral("Crate Decks"),
            component.firstChildElement(QStringLiteral("name")).text());
    EXPECT_QSTRING_EQ(QStringLiteral("hello@cratemusic.app"),
            component.firstChildElement(QStringLiteral("update_contact")).text());
    EXPECT_QSTRING_EQ(QStringLiteral("crate-decks"),
            component.firstChildElement(QStringLiteral("provides"))
                    .firstChildElement(QStringLiteral("binary"))
                    .text());
}

TEST_F(CrateBrandingTest, SourceCodeUrlPointsAtTheBuiltSource) {
    const QString repository = QString::fromUtf8(crate::kSourceRepositoryUrl);
    const struct {
        QString gitDescribe;
        QString url;
    } cases[] = {
            {QStringLiteral("decks-v0.1.0"), repository + QStringLiteral("/tree/decks-v0.1.0")},
            {QStringLiteral("decks-v0.1.0-5-ge9ea8313bd"),
                    repository + QStringLiteral("/tree/e9ea8313bd")},
            {QStringLiteral("2.6-beta-3815-ge9ea8313bd"),
                    repository + QStringLiteral("/tree/e9ea8313bd")},
            {QStringLiteral("2.6-beta-3815-ge9ea8313bd-modified"),
                    repository + QStringLiteral("/tree/e9ea8313bd")},
            {QStringLiteral("e9ea8313bd"), repository + QStringLiteral("/tree/e9ea8313bd")},
            {QString(), repository},
    };
    for (const auto& testCase : cases) {
        SCOPED_TRACE(qPrintable(testCase.gitDescribe));
        EXPECT_QSTRING_EQ(testCase.url, crate::sourceCodeUrl(testCase.gitDescribe).toString());
    }
}

TEST_F(CrateBrandingTest, AboutCreditsMixxxAndLinksTheSourceCode) {
    const DlgAbout about;

    EXPECT_QSTRING_EQ(QStringLiteral("About Crate Decks"), about.windowTitle());
    const auto* pVersion = about.findChild<QLabel*>(QStringLiteral("version_label"));
    ASSERT_NE(nullptr, pVersion);
    EXPECT_QSTRING_EQ(QStringLiteral("Crate Decks ") +
                    crate::buildVersion(VersionStore::gitDescribe()),
            pVersion->text());
    const auto* pBasedOn = about.findChild<QLabel*>(QStringLiteral("crate_based_on"));
    ASSERT_NE(nullptr, pBasedOn);
    const QString basedOn = pBasedOn->text();
    EXPECT_TRUE(basedOn.contains(QStringLiteral("is based on"))) << qPrintable(basedOn);
    EXPECT_TRUE(basedOn.contains(QStringLiteral(MIXXX_WEBSITE_URL))) << qPrintable(basedOn);
    EXPECT_TRUE(basedOn.contains(QStringLiteral("</a> ") + VersionStore::version()))
            << qPrintable(basedOn);
    EXPECT_TRUE(basedOn.contains(QStringLiteral("GNU General Public License")))
            << qPrintable(basedOn);
    const QString sourceCodeUrl =
            crate::sourceCodeUrl(VersionStore::gitDescribe()).toString();
    EXPECT_FALSE(sourceCodeUrl.isEmpty());
    EXPECT_TRUE(basedOn.contains(sourceCodeUrl)) << qPrintable(basedOn);

    // The Mixxx credits and its copyright notice stay.
    const auto* pCredits = about.findChild<QTextBrowser*>(QStringLiteral("textBrowser"));
    ASSERT_NE(nullptr, pCredits);
    EXPECT_TRUE(pCredits->toPlainText().contains(QStringLiteral("Mixxx")));
    const auto* pLicense = about.findChild<QTextBrowser*>(QStringLiteral("licenseText"));
    ASSERT_NE(nullptr, pLicense);
    EXPECT_TRUE(pLicense->toPlainText().contains(QStringLiteral("Mixxx Development Team")));
}

TEST_F(CrateBrandingTest, AboutOffersNoDonation) {
    const DlgAbout about;

    // Of Mixxx, About keeps only the legal line, the credits and the
    // licence (design D2); Crate Decks asks for no donations to Mixxx
    const auto* pDonate = about.findChild<QPushButton*>(QStringLiteral("btnDonate"));
    if (pDonate) {
        EXPECT_TRUE(pDonate->isHidden());
    }
    const QList<QPushButton*> buttons = about.findChildren<QPushButton*>();
    for (const QPushButton* pButton : buttons) {
        if (!pButton->isHidden()) {
            EXPECT_FALSE(pButton->text().contains(QStringLiteral("Donate"), Qt::CaseInsensitive))
                    << qPrintable(pButton->text());
        }
    }
}

TEST_F(CrateBrandingTest, AboutCreditsTheSolarIcons) {
    const DlgAbout about;

    // CC BY 4.0 asks for visible credit, its licence and what changed
    // (design 14.3)
    const auto* pAttribution = about.findChild<QLabel*>(QStringLiteral("crate_attribution"));
    ASSERT_NE(nullptr, pAttribution);
    const QString text = pAttribution->text();
    EXPECT_TRUE(text.contains(QStringLiteral("Solar Icons Set"))) << qPrintable(text);
    EXPECT_TRUE(text.contains(QStringLiteral("480 Design"))) << qPrintable(text);
    EXPECT_TRUE(text.contains(QStringLiteral("https://creativecommons.org/licenses/by/4.0/")))
            << qPrintable(text);
    EXPECT_TRUE(text.contains(QStringLiteral("recoloured"))) << qPrintable(text);
    // And the fonts' licence
    EXPECT_TRUE(text.contains(QStringLiteral("SIL Open Font License"))) << qPrintable(text);
    EXPECT_TRUE(pAttribution->openExternalLinks());
}

#ifdef __CRATE__
TEST_F(CrateBrandingTest, AboutHasTheFirstStartLook) {
    const DlgAbout about;

    // No window frame: the dialog draws its own, as the first start
    EXPECT_TRUE(about.windowFlags().testFlag(Qt::FramelessWindowHint));
    EXPECT_NE(nullptr, about.findChild<QWidget*>(QStringLiteral("CrateDialogFrame")));
    const auto* pTitle = about.findChild<QLabel*>(QStringLiteral("CrateAboutTitle"));
    ASSERT_NE(nullptr, pTitle);
    EXPECT_QSTRING_EQ(QStringLiteral("Crate Decks"), pTitle->text());
    EXPECT_NE(nullptr, about.findChild<QLabel*>(QStringLiteral("MonoAboutKicker")));
}
#endif

} // namespace
