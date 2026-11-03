#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QString>
#include <QTemporaryDir>

#include "crate/appidentity.h"
#include "defs_urls.h"
#include "dialog/dlgabout.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"
#include "util/cmdlineargs.h"
#include "util/versionstore.h"

namespace {

const QString kMixxxSettingsDir = QStringLiteral(".mixxx");

QDir resourceDir() {
    return QDir(ConfigObject<ConfigValue>::computeResourcePath());
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
    }

    void TearDown() override {
        // Other tests must keep seeing the identity of mixxx-test.
        QCoreApplication::setOrganizationName(m_organizationName);
        QCoreApplication::setOrganizationDomain(m_organizationDomain);
        QGuiApplication::setDesktopFileName(m_desktopFileName);
    }

  private:
    QString m_organizationName;
    QString m_organizationDomain;
    QString m_desktopFileName;
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

} // namespace
