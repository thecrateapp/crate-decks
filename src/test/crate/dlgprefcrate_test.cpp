#include "crate/ui/dlgprefcrate.h"

#include <gtest/gtest.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSlider>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <memory>

#include "crate/cache/audiocache.h"
#include "crate/client/crateserveraddress.h"
#include "crate/crateaddserverflow.h"
#include "crate/cratepreferences.h"
#include "crate/crateserverrows.h"
#include "crate/crateservers.h"
#include "crate/db/crateserverprofiles.h"
#include "crate/ui/cratefirstrun.h"
#include "crate/ui/crateprefsections.h"
#include "crate/ui/dialogs/dialogscrim.h"
#include "crate/ui/dialogs/dialogstyle.h"
#include "skin/legacy/launchimage.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"

namespace {

using crate::parseServerAddress;
using Security = crate::ServerAddress::Security;

TEST(CratePreferencesTest, AddServerRequiresHttpsExceptLoopbackAndPrivate) {
    struct Case {
        const char* input;
        Security security;
        const char* origin;
    };
    const Case cases[] = {
            {"crate.example.com",
                    Security::Secure,
                    "https://crate.example.com"},
            {"https://Crate.Example.com/admin?x=1#top",
                    Security::Secure,
                    "https://crate.example.com"},
            {"https://crate.example.com:8443/",
                    Security::Secure,
                    "https://crate.example.com:8443"},
            {"http://localhost:8585",
                    Security::Secure,
                    "http://localhost:8585"},
            {"http://127.0.0.1:8585",
                    Security::Secure,
                    "http://127.0.0.1:8585"},
            {"http://[::1]:8585", Security::Secure, "http://[::1]:8585"},
            {"http://192.168.1.10:8585",
                    Security::LocalNetworkHttp,
                    "http://192.168.1.10:8585"},
            {"http://10.0.0.5", Security::LocalNetworkHttp, "http://10.0.0.5"},
            {"http://172.20.1.1",
                    Security::LocalNetworkHttp,
                    "http://172.20.1.1"},
            {"http://nas:8585", Security::LocalNetworkHttp, "http://nas:8585"},
            {"http://crate.local",
                    Security::LocalNetworkHttp,
                    "http://crate.local"},
            {"http://crate.home.arpa",
                    Security::LocalNetworkHttp,
                    "http://crate.home.arpa"},
            {"http://crate.example.com", Security::Rejected, ""},
            {"http://172.32.0.1", Security::Rejected, ""},
            {"http://8.8.8.8", Security::Rejected, ""},
            {"ftp://crate.example.com", Security::Rejected, ""},
            {"https://dj:secret@crate.example.com", Security::Rejected, ""},
            {"", Security::Rejected, ""},
            {"https://", Security::Rejected, ""},
    };

    for (const Case& testCase : cases) {
        const crate::ServerAddress address = parseServerAddress(QString::fromUtf8(testCase.input));
        EXPECT_EQ(testCase.security, address.security) << testCase.input;
        if (testCase.security == Security::Rejected) {
            EXPECT_FALSE(address.problem.isEmpty()) << testCase.input;
        } else {
            EXPECT_EQ(QUrl(QString::fromUtf8(testCase.origin)), address.origin) << testCase.input;
        }
    }
}

TEST(CratePreferencesTest, AddressesOfTheSameServerHaveOneOrigin) {
    EXPECT_EQ(QUrl(QStringLiteral("https://crate.example.com")),
            parseServerAddress(QStringLiteral("https://crate.example.com:443")).origin);
    EXPECT_EQ(QUrl(QStringLiteral("http://nas")),
            parseServerAddress(QStringLiteral("http://nas:80")).origin);
    EXPECT_EQ(QUrl(QStringLiteral("https://crate.example.com")),
            parseServerAddress(QStringLiteral("https://crate.example.com./")).origin);
    EXPECT_EQ(Security::Rejected,
            parseServerAddress(QStringLiteral("https://crate.example.com:0")).security);
}

using crate::AddServerFlow;
using crate::CrateServers;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeSecretStore;
using Step = crate::AddServerFlow::Step;

const QByteArray kDecksCapabilities = QByteArrayLiteral(R"({"decks": {
    "available": true, "min_client_version": "0.1.0", "contract_version": 1, "features": []}})");
const QByteArray kLoginAnswer = QByteArrayLiteral(
        R"({"id": 7, "email": "dj@crate.test", "role": "user", "token": "access-1",
            "refresh_token": "refresh-1"})");

class CrateAddServerFlowTest : public MixxxDbTest {
  protected:
    CrateAddServerFlowTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection()));
        CrateServers::Dependencies dependencies;
        dependencies.pConfig = config();
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.pSecretStore = &m_store;
        ASSERT_TRUE(m_cacheDir.isValid());
        dependencies.audioCacheDirectory = m_cacheDir.path();
        m_pServers = std::make_unique<CrateServers>(dependencies);
        m_pServers->start();
        m_pFlow = std::make_unique<AddServerFlow>(m_pServers.get());
    }

    void TearDown() override {
        m_pFlow.reset();
        m_pServers.reset();
    }

    FakeNetworkAccessManager::Sent lastSent() const {
        return m_network.last();
    }

    QTemporaryDir m_cacheDir;
    FakeNetworkAccessManager m_network;
    FakeSecretStore m_store;
    std::unique_ptr<CrateServers> m_pServers;
    std::unique_ptr<AddServerFlow> m_pFlow;
};

TEST_F(CrateAddServerFlowTest, AddServerRejectsServerWithoutDecksCapability) {
    m_pFlow->setAddress(QStringLiteral("https://old.test"));
    ASSERT_EQ(Step::CheckingServer, m_pFlow->step());
    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_EQ(QUrl(QStringLiteral("https://old.test/api/capabilities")), lastSent().request.url());
    EXPECT_FALSE(lastSent().request.hasRawHeader("Authorization"));

    lastSent().pReply->respond(200, QByteArrayLiteral(R"({"smart_mix": {"available": true}})"));

    EXPECT_EQ(Step::EnterAddress, m_pFlow->step());
    EXPECT_FALSE(m_pFlow->error().isEmpty());
    EXPECT_TRUE(m_pServers->sessions().isEmpty());
}

TEST_F(CrateAddServerFlowTest, PrivateHttpRequiresConfirmation) {
    m_pFlow->setAddress(QStringLiteral("http://192.168.1.10:8585"));

    EXPECT_EQ(Step::ConfirmLocalNetwork, m_pFlow->step());
    EXPECT_TRUE(m_network.sent.isEmpty());
    m_pFlow->confirmLocalNetwork();

    EXPECT_EQ(Step::CheckingServer, m_pFlow->step());
    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_EQ(QUrl(QStringLiteral("http://192.168.1.10:8585/api/capabilities")),
            lastSent().request.url());
}

TEST_F(CrateAddServerFlowTest, PublicHttpIsRefusedBeforeAnyRequest) {
    m_pFlow->setAddress(QStringLiteral("http://crate.example.com"));

    EXPECT_EQ(Step::EnterAddress, m_pFlow->step());
    EXPECT_FALSE(m_pFlow->error().isEmpty());
    EXPECT_TRUE(m_network.sent.isEmpty());
}

TEST_F(CrateAddServerFlowTest, ServerNeedingANewerClientIsRefused) {
    m_pFlow->setAddress(QStringLiteral("https://new.test"));
    lastSent().pReply->respond(200, QByteArrayLiteral(R"({"decks": {
        "available": true, "min_client_version": "9.0.0", "contract_version": 1, "features": []}})"));

    EXPECT_EQ(Step::EnterAddress, m_pFlow->step());
    EXPECT_FALSE(m_pFlow->error().isEmpty());
    EXPECT_TRUE(m_pServers->sessions().isEmpty());
}

TEST_F(CrateAddServerFlowTest, UnreachableServerAsksForTheAddressAgain) {
    m_pFlow->setAddress(QStringLiteral("https://offline.test"));
    lastSent().pReply->failNetwork();

    EXPECT_EQ(Step::EnterAddress, m_pFlow->step());
    EXPECT_FALSE(m_pFlow->error().isEmpty());
    EXPECT_TRUE(m_pServers->sessions().isEmpty());
}

TEST_F(CrateAddServerFlowTest, AddsTheServerAfterSigningIn) {
    QSignalSpy added(m_pFlow.get(), &AddServerFlow::serverAdded);
    m_pFlow->setAddress(QStringLiteral("crate.test"));
    lastSent().pReply->respond(200, kDecksCapabilities);
    ASSERT_EQ(Step::SignIn, m_pFlow->step());

    m_pFlow->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("wrong"));
    EXPECT_EQ(Step::SigningIn, m_pFlow->step());
    lastSent().pReply->respond(401, QByteArrayLiteral(R"({"detail": "Invalid credentials"})"));
    EXPECT_EQ(Step::SignIn, m_pFlow->step());
    EXPECT_FALSE(m_pFlow->error().isEmpty());
    m_pFlow->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("right"));
    lastSent().pReply->respond(200, kLoginAnswer);

    EXPECT_EQ(Step::Done, m_pFlow->step());
    ASSERT_EQ(1, added.count());
    const QList<crate::ServerProfile> saved = crate::db::loadServerProfiles(dbConnection());
    ASSERT_EQ(1, saved.size());
    EXPECT_EQ(QUrl(QStringLiteral("https://crate.test")), saved.first().baseUrl);
    EXPECT_EQ(QJsonDocument::fromJson(kDecksCapabilities),
            QJsonDocument::fromJson(
                    crate::db::loadLastCapabilities(dbConnection(), saved.first().id)));
}

TEST_F(CrateAddServerFlowTest, CancellingForgetsTheServer) {
    m_pFlow->setAddress(QStringLiteral("https://crate.test"));
    lastSent().pReply->respond(200, kDecksCapabilities);
    ASSERT_EQ(1, m_pServers->sessions().size());

    m_pFlow->cancel();

    EXPECT_EQ(Step::EnterAddress, m_pFlow->step());
    EXPECT_TRUE(m_pServers->sessions().isEmpty());
    EXPECT_TRUE(crate::db::loadServerProfiles(dbConnection()).isEmpty());
}

// TEST_GAP: the page is driven here without being shown; how it looks and
// the browser sign-in are checked by hand against the dev stack (F2.C-PR3).
TEST_F(CrateAddServerFlowTest, PageAddsAServer) {
    crate::DlgPrefCrate page(nullptr, m_pServers.get());
    auto* pList = page.findChild<QListWidget*>(QStringLiteral("crateServers"));
    auto* pAdd = page.findChild<QPushButton*>(QStringLiteral("crateAddServer"));
    auto* pAddress = page.findChild<QLineEdit*>(QStringLiteral("crateServerAddress"));
    auto* pConnect = page.findChild<QPushButton*>(QStringLiteral("crateConnect"));
    auto* pEmail = page.findChild<QLineEdit*>(QStringLiteral("crateEmail"));
    auto* pPassword = page.findChild<QLineEdit*>(QStringLiteral("cratePassword"));
    auto* pSignIn = page.findChild<QPushButton*>(QStringLiteral("crateSignIn"));
    ASSERT_TRUE(pList && pAdd && pAddress && pConnect && pEmail && pPassword && pSignIn);
    EXPECT_EQ(0, pList->count());

    pAdd->click();
    pAddress->setText(QStringLiteral("https://crate.test"));
    pConnect->click();
    lastSent().pReply->respond(200, kDecksCapabilities);
    pEmail->setText(QStringLiteral("dj@crate.test"));
    pPassword->setText(QStringLiteral("secret-password"));
    pSignIn->click();
    EXPECT_TRUE(pPassword->text().isEmpty());
    lastSent().pReply->respond(200, kLoginAnswer);

    ASSERT_EQ(1, pList->count());
    EXPECT_TRUE(pList->item(0)->text().contains(QStringLiteral("https://crate.test")));
}

TEST_F(CrateAddServerFlowTest, ServerRowsShowFeaturesAndState) {
    EXPECT_TRUE(crate::serverRows(*m_pServers).isEmpty());
    const QByteArray capabilities = QByteArrayLiteral(R"({"decks": {
        "available": true, "min_client_version": "0.1.0", "contract_version": 1,
        "features": ["library_cursor", "beatmatch"]}})");
    m_pFlow->setAddress(QStringLiteral("https://crate.test"));
    lastSent().pReply->respond(200, capabilities);
    m_pFlow->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("right"));
    lastSent().pReply->respond(200, kLoginAnswer);

    const QList<crate::ServerRow> rows = crate::serverRows(*m_pServers);
    ASSERT_EQ(1, rows.size());
    const crate::ServerRow& row = rows.first();
    EXPECT_QSTRING_EQ(QStringLiteral("crate.test"), row.name);
    EXPECT_QSTRING_EQ(QStringLiteral("https://crate.test"), row.meta.value(0));
    EXPECT_TRUE(row.meta.join(u' ').contains(QStringLiteral("dj@crate.test")));
    EXPECT_EQ(crate::ServerRow::State::SignedIn, row.state);
    EXPECT_QSTRING_EQ(QStringLiteral("signedIn"), row.stateName());
    // Every feature of the contract, in the mockup's order, available or not
    ASSERT_EQ(6, row.features.size());
    EXPECT_EQ((crate::ServerRow::Feature{QStringLiteral("library"), true}), row.features[0]);
    EXPECT_EQ((crate::ServerRow::Feature{QStringLiteral("analysis"), false}), row.features[1]);
    EXPECT_EQ((crate::ServerRow::Feature{QStringLiteral("beatmatch"), true}), row.features[2]);
    EXPECT_FALSE(row.features[5].available);
}

TEST(CrateServerRowsTest, FeaturesNeedAValidAnswer) {
    EXPECT_TRUE(crate::serverFeatures(QByteArray()).isEmpty());
    EXPECT_EQ(6, crate::serverFeatures(kDecksCapabilities).size());
}

TEST_F(CrateAddServerFlowTest, FirstRunDialogFollowsTheSteps) {
    std::unique_ptr<QDialog> pDialog(crate::createFirstRunDialog(m_pServers.get(), nullptr));
    auto* pStep = pDialog->findChild<QLabel*>(QStringLiteral("MonoFirstRunStep"));
    ASSERT_NE(nullptr, pStep);
    EXPECT_QSTRING_EQ(QStringLiteral("Step 1 of 3"), pStep->text());
    const QList<QFrame*> lanes = pDialog->findChildren<QFrame*>(QStringLiteral("CrateStepLane"));
    ASSERT_EQ(3, lanes.size());
    EXPECT_TRUE(lanes[0]->property("current").toBool());
    EXPECT_FALSE(lanes[1]->property("current").toBool());
    // "Not now" is the first start's cancel
    auto* pCancel = pDialog->findChild<QPushButton*>(QStringLiteral("crateCancel"));
    ASSERT_NE(nullptr, pCancel);
    EXPECT_QSTRING_EQ(QStringLiteral("Not now"), pCancel->text());

    pDialog->findChild<QLineEdit*>(QStringLiteral("crateServerAddress"))
            ->setText(QStringLiteral("crate.test"));
    pDialog->findChild<QPushButton*>(QStringLiteral("crateConnect"))->click();
    EXPECT_QSTRING_EQ(QStringLiteral("Step 2 of 3"), pStep->text());
    lastSent().pReply->respond(200, kDecksCapabilities);
    EXPECT_QSTRING_EQ(QStringLiteral("Step 3 of 3"), pStep->text());
    EXPECT_TRUE(lanes[2]->property("current").toBool());

    QSignalSpy rejected(pDialog.get(), &QDialog::rejected);
    pCancel->click();
    EXPECT_EQ(1, rejected.count());
}

TEST_F(CrateAddServerFlowTest, FirstRunDimsTheMainWindowUntilItCloses) {
    // Crate's styles on a main window, as MixxxMainWindow sets them, undone
    // for the next tests
    const auto restore = qScopeGuard([] { crate::applyStyles(nullptr, QString(), QString()); });
    QWidget mainWindow;
    mainWindow.resize(1280, 720);
    ASSERT_TRUE(crate::applyApplicationStyle(
            &mainWindow, ConfigObject<ConfigValue>::computeResourcePath()));
    mainWindow.show();
    QCoreApplication::processEvents();
    std::unique_ptr<QDialog> pDialog(crate::createFirstRunDialog(m_pServers.get(), &mainWindow));
    // As offerFirstRun() opens it, until "Not now"
    bool dimmed = false;
    QTimer::singleShot(0, pDialog.get(), [&pDialog, &dimmed] {
        const crate::DialogScrim* pScrim = crate::blockingDialogScrim();
        dimmed = pScrim && pScrim->isActive();
        pDialog->findChild<QPushButton*>(QStringLiteral("crateCancel"))->click();
    });
    EXPECT_EQ(QDialog::Rejected, pDialog->exec());
    EXPECT_TRUE(dimmed);
    ASSERT_NE(nullptr, crate::blockingDialogScrim());
    EXPECT_FALSE(crate::blockingDialogScrim()->isActive());
}

/// The first start's sound and music pages without Mixxx: devices and
/// folders the test chooses, and what the pages did with them.
struct FakeFirstRunServices {
    QList<crate::FirstRunOutput> outputs;
    QString mainOutput;
    QStringList setOutputs;
    int searches = 0;
    int preferences = 0;
    QString chosenFolder;
    QStringList addedFolders;

    crate::FirstRunServices services() {
        crate::FirstRunServices services;
        services.outputs = [this] { return outputs; };
        services.mainOutput = [this] { return mainOutput; };
        services.setMainOutput = [this](const QString& id) {
            setOutputs.append(id);
            mainOutput = id;
            return true;
        };
        services.searchOutputs = [this] { ++searches; };
        services.showSoundPreferences = [this] { ++preferences; };
        services.chooseFolder = [this](QWidget*) { return chosenFolder; };
        services.addMusicFolder = [this](const QString& folder) {
            addedFolders.append(folder);
            return true;
        };
        return services;
    }
};

QWidget* currentFirstRunPage(QDialog* pDialog) {
    auto* pPages = pDialog->findChild<QStackedWidget*>(QStringLiteral("CrateFirstRunPages"));
    return pPages ? pPages->currentWidget() : nullptr;
}

QPushButton* firstRunButton(QDialog* pDialog, const char* objectName) {
    QWidget* pPage = currentFirstRunPage(pDialog);
    return pPage ? pPage->findChild<QPushButton*>(QLatin1String(objectName)) : nullptr;
}

TEST_F(CrateAddServerFlowTest, FirstRunPagesFollowTheServerPage) {
    FakeFirstRunServices fake;
    fake.outputs = {{QStringLiteral("0"), QStringLiteral("Built-in Audio")}};
    fake.mainOutput = QStringLiteral("0");
    std::unique_ptr<QDialog> pDialog(
            crate::createFirstRunDialog(m_pServers.get(), fake.services(), nullptr));
    ASSERT_NE(nullptr, currentFirstRunPage(pDialog.get()));
    EXPECT_QSTRING_EQ(QStringLiteral("CrateFirstRunServer"),
            currentFirstRunPage(pDialog.get())->objectName());

    // "Not now" moves on to the sound page instead of closing
    QSignalSpy rejected(pDialog.get(), &QDialog::rejected);
    firstRunButton(pDialog.get(), "crateCancel")->click();
    EXPECT_EQ(0, rejected.count());
    EXPECT_QSTRING_EQ(QStringLiteral("CrateFirstRunSound"),
            currentFirstRunPage(pDialog.get())->objectName());
    auto* pMeta =
            currentFirstRunPage(pDialog.get())
                    ->findChild<QLabel*>(QStringLiteral("MonoFirstRunStep"));
    ASSERT_NE(nullptr, pMeta);
    EXPECT_QSTRING_EQ(QStringLiteral("Sound"), pMeta->text());

    // Continue keeps the device it already uses, then the music page
    firstRunButton(pDialog.get(), "crateFirstRunContinue")->click();
    EXPECT_TRUE(fake.setOutputs.isEmpty());
    EXPECT_QSTRING_EQ(QStringLiteral("CrateFirstRunMusic"),
            currentFirstRunPage(pDialog.get())->objectName());

    QSignalSpy accepted(pDialog.get(), &QDialog::accepted);
    firstRunButton(pDialog.get(), "crateFirstRunFinish")->click();
    EXPECT_EQ(1, accepted.count());
    EXPECT_TRUE(fake.addedFolders.isEmpty()) << "Local folders are optional";
}

// The pages keep the width of the GU1 mockup (first-run: 560 px with its
// padding), "Connect your Crate" included, and the dialog fits the page it
// shows, not the tallest one
TEST_F(CrateAddServerFlowTest, FirstRunPagesKeepTheMockupWidth) {
    FakeFirstRunServices fake;
    std::unique_ptr<QDialog> pDialog(
            crate::createFirstRunDialog(m_pServers.get(), fake.services(), nullptr));
    const int width = crate::DialogMetrics::installed().px("firstRunWidth");
    ASSERT_GT(width, 0);
    const auto size = [&pDialog] {
        pDialog->layout()->activate();
        return pDialog->size();
    };
    const QSize server = size();
    EXPECT_EQ(width, server.width());

    // Without an output the sound page has its three buttons
    firstRunButton(pDialog.get(), "crateCancel")->click();
    const QSize sound = size();
    EXPECT_EQ(width, sound.width());
    EXPECT_LT(sound.height(), server.height());

    firstRunButton(pDialog.get(), "crateFirstRunContinue")->click();
    const QSize music = size();
    EXPECT_EQ(width, music.width());
    EXPECT_LT(music.height(), server.height());
}

TEST_F(CrateAddServerFlowTest, FirstRunSoundPageSetsTheMainOutput) {
    FakeFirstRunServices fake;
    fake.outputs = {{QStringLiteral("0"), QStringLiteral("Built-in Audio")},
            {QStringLiteral("1"), QStringLiteral("DJ Controller")}};
    fake.mainOutput = QStringLiteral("0");
    std::unique_ptr<QDialog> pDialog(
            crate::createFirstRunDialog(m_pServers.get(), fake.services(), nullptr));
    firstRunButton(pDialog.get(), "crateCancel")->click();

    auto* pOutputs = currentFirstRunPage(pDialog.get())
                             ->findChild<QComboBox*>(
                                     QStringLiteral("crateFirstRunOutput"));
    ASSERT_NE(nullptr, pOutputs);
    ASSERT_EQ(2, pOutputs->count());
    EXPECT_QSTRING_EQ(QStringLiteral("Built-in Audio"), pOutputs->currentText());
    EXPECT_FALSE(pOutputs->isHidden());
    pOutputs->setCurrentIndex(1);
    firstRunButton(pDialog.get(), "crateFirstRunContinue")->click();
    EXPECT_EQ(QStringList{QStringLiteral("1")}, fake.setOutputs);

    // "More options" opens Preferences on the sound hardware page
    std::unique_ptr<QDialog> pAgain(
            crate::createFirstRunDialog(m_pServers.get(), fake.services(), nullptr));
    firstRunButton(pAgain.get(), "crateCancel")->click();
    firstRunButton(pAgain.get(), "crateFirstRunMore")->click();
    EXPECT_EQ(1, fake.preferences);
}

TEST_F(CrateAddServerFlowTest, FirstRunSoundPageWithoutOutputsOffersToContinue) {
    FakeFirstRunServices fake;
    std::unique_ptr<QDialog> pDialog(
            crate::createFirstRunDialog(m_pServers.get(), fake.services(), nullptr));
    firstRunButton(pDialog.get(), "crateCancel")->click();
    QWidget* pPage = currentFirstRunPage(pDialog.get());
    EXPECT_TRUE(pPage->findChild<QComboBox*>(QStringLiteral("crateFirstRunOutput"))->isHidden());
    auto* pWarning = pPage->findChild<QLabel*>(QStringLiteral("crateError"));
    ASSERT_NE(nullptr, pWarning);
    EXPECT_FALSE(pWarning->isHidden());
    EXPECT_QSTRING_EQ(QStringLiteral("No output device found."), pWarning->text());

    QPushButton* pSearch = firstRunButton(pDialog.get(), "crateFirstRunSearch");
    ASSERT_NE(nullptr, pSearch);
    EXPECT_FALSE(pSearch->isHidden());
    pSearch->click();
    EXPECT_EQ(1, fake.searches);

    QPushButton* pContinue = firstRunButton(pDialog.get(), "crateFirstRunContinue");
    EXPECT_QSTRING_EQ(QStringLiteral("Continue without sound"), pContinue->text());
    pContinue->click();
    EXPECT_QSTRING_EQ(QStringLiteral("CrateFirstRunMusic"),
            currentFirstRunPage(pDialog.get())->objectName());
}

TEST_F(CrateAddServerFlowTest, FirstRunMusicPageAddsTheChosenFolders) {
    FakeFirstRunServices fake;
    fake.chosenFolder = QStringLiteral("/home/dj/Music");
    std::unique_ptr<QDialog> pDialog(
            crate::createFirstRunDialog(m_pServers.get(), fake.services(), nullptr));
    firstRunButton(pDialog.get(), "crateCancel")->click();
    firstRunButton(pDialog.get(), "crateFirstRunContinue")->click();

    firstRunButton(pDialog.get(), "crateFirstRunAddFolder")->click();
    EXPECT_EQ(QStringList{QStringLiteral("/home/dj/Music")}, fake.addedFolders);
    auto* pFolders = currentFirstRunPage(pDialog.get())
                             ->findChild<QLabel*>(
                                     QStringLiteral("crateFirstRunFolders"));
    ASSERT_NE(nullptr, pFolders);
    EXPECT_FALSE(pFolders->isHidden());
    EXPECT_TRUE(pFolders->text().contains(QLatin1String("Music")));

    // A cancelled file dialog adds nothing
    fake.chosenFolder.clear();
    firstRunButton(pDialog.get(), "crateFirstRunAddFolder")->click();
    EXPECT_EQ(1, fake.addedFolders.size());
}

TEST(CrateFirstRunTest, MenuBarIsHiddenWithoutAsking) {
    auto pConfig = UserSettingsPointer(new UserSettings(QString()));
    EXPECT_TRUE(crate::firstRunPending(pConfig));
    crate::decideMenuBar(pConfig);
    EXPECT_EQ(1, pConfig->getValue<int>(ConfigKey("[Config]", "hide_menubar"), 0));
    EXPECT_EQ(0, pConfig->getValue<int>(ConfigKey("[Config]", "show_menubar_hint"), 1));

    // A choice the user made stays
    pConfig->setValue(ConfigKey("[Config]", "hide_menubar"), 0);
    crate::decideMenuBar(pConfig);
    EXPECT_EQ(0, pConfig->getValue<int>(ConfigKey("[Config]", "hide_menubar"), 1));

    crate::markFirstRunOffered(pConfig);
    EXPECT_FALSE(crate::firstRunPending(pConfig));
}

// With OpenGL, Mixxx loads the skin and the dialog styles only after the
// event loop starts, and then shows its own startup dialogs: the first start
// waits for both, or it shows unstyled over the launch image
TEST_F(CrateAddServerFlowTest, FirstRunWaitsForTheSkinAndOtherDialogs) {
    QMainWindow mainWindow;
    mainWindow.setCentralWidget(new LaunchImage(&mainWindow, QString()));
    mainWindow.show();
    EXPECT_FALSE(crate::firstRunCanShow(&mainWindow));

    // The skin replaces the launch image
    mainWindow.setCentralWidget(new QWidget(&mainWindow));
    EXPECT_TRUE(crate::firstRunCanShow(&mainWindow));

    // Another modal dialog, as Mixxx's "No Output Devices"
    QDialog other(&mainWindow);
    other.setWindowModality(Qt::ApplicationModal);
    other.show();
    EXPECT_FALSE(crate::firstRunCanShow(&mainWindow));
    other.close();
    EXPECT_TRUE(crate::firstRunCanShow(&mainWindow));

    // A parent that is no main window has nothing to wait for
    QWidget parent;
    EXPECT_TRUE(crate::firstRunCanShow(&parent));
}

// TEST_GAP: offerFirstRun() shows a modal dialog, so it is checked by hand;
// the tests cover when it is offered and that it waits for the skin.
TEST_F(CrateAddServerFlowTest, FirstRunIsOfferedOnceWhileThereIsNoServer) {
    EXPECT_TRUE(crate::shouldOfferFirstRun(config(), *m_pServers));

    crate::markFirstRunOffered(config());

    EXPECT_FALSE(crate::shouldOfferFirstRun(config(), *m_pServers));
}

TEST_F(CrateAddServerFlowTest, FirstRunIsNotOfferedWithAServer) {
    m_pFlow->setAddress(QStringLiteral("https://crate.test"));
    lastSent().pReply->respond(200, kDecksCapabilities);
    m_pFlow->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("right"));
    lastSent().pReply->respond(200, kLoginAnswer);

    EXPECT_FALSE(crate::shouldOfferFirstRun(config(), *m_pServers));
}

TEST_F(CrateAddServerFlowTest, AddingTheSameServerTwiceIsRefused) {
    m_pFlow->setAddress(QStringLiteral("crate.test"));
    lastSent().pReply->respond(200, kDecksCapabilities);
    m_pFlow->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("right"));
    lastSent().pReply->respond(200, kLoginAnswer);
    ASSERT_EQ(Step::Done, m_pFlow->step());
    const qsizetype requests = m_network.sent.size();

    AddServerFlow again(m_pServers.get());
    again.setAddress(QStringLiteral("https://crate.test:443/"));

    EXPECT_EQ(Step::EnterAddress, again.step());
    EXPECT_FALSE(again.error().isEmpty());
    EXPECT_EQ(requests, m_network.sent.size());
    EXPECT_EQ(1, m_pServers->sessions().size());
}

TEST_F(CrateAddServerFlowTest, ServerThatCannotBeSavedIsNotAdded) {
    QSqlQuery drop(dbConnection());
    ASSERT_TRUE(drop.exec(QStringLiteral("DROP TABLE crateserver_servers")));

    m_pFlow->setAddress(QStringLiteral("crate.test"));
    lastSent().pReply->respond(200, kDecksCapabilities);
    m_pFlow->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("right"));
    lastSent().pReply->respond(200, kLoginAnswer);

    EXPECT_NE(Step::Done, m_pFlow->step());
    EXPECT_FALSE(m_pFlow->error().isEmpty());
    EXPECT_TRUE(m_pServers->sessions().isEmpty());
}

TEST_F(CrateAddServerFlowTest, PageWarnsWhenTheKeychainIsUnavailable) {
    m_store.unavailable = true;
    crate::DlgPrefCrate page(nullptr, m_pServers.get());
    auto* pWarning = page.findChild<QLabel*>(QStringLiteral("crateKeychainWarning"));
    ASSERT_TRUE(pWarning);
    EXPECT_TRUE(pWarning->isHidden());

    page.findChild<QPushButton*>(QStringLiteral("crateAddServer"))->click();
    page.findChild<QLineEdit*>(QStringLiteral("crateServerAddress"))
            ->setText(QStringLiteral("crate.test"));
    page.findChild<QPushButton*>(QStringLiteral("crateConnect"))->click();
    lastSent().pReply->respond(200, kDecksCapabilities);
    page.findChild<QLineEdit*>(QStringLiteral("crateEmail"))
            ->setText(QStringLiteral("dj@crate.test"));
    page.findChild<QLineEdit*>(QStringLiteral("cratePassword"))->setText(QStringLiteral("pw"));
    page.findChild<QPushButton*>(QStringLiteral("crateSignIn"))->click();
    lastSent().pReply->respond(200, kLoginAnswer);

    EXPECT_FALSE(pWarning->isHidden());
}

TEST_F(CrateAddServerFlowTest, ServerTextIsShownAsPlainText) {
    crate::DlgPrefCrate page(nullptr, m_pServers.get());

    for (const char* name : {"crateError",
                 "crateBusy",
                 "crateServerStatus",
                 "crateLocalNetworkWarning"}) {
        auto* pLabel = page.findChild<QLabel*>(QLatin1String(name));
        ASSERT_TRUE(pLabel) << name;
        EXPECT_EQ(Qt::PlainText, pLabel->textFormat()) << name;
    }
}

// The cache section of the page sets the preferences that the audio cache
// follows (plan F4.C-PR4): its limit at once, and design §9's 20 GiB until
// the user chooses another.
TEST_F(CrateAddServerFlowTest, CacheLimitDrivesTheAudioCache) {
    crate::DlgPrefCrate page(nullptr, m_pServers.get());
    crate::CacheSection* pCache = page.cacheSection();
    ASSERT_NE(nullptr, pCache);
    EXPECT_FALSE(pCache->isHidden());
    crate::CratePreferences* pPreferences = m_pServers->preferences();
    EXPECT_EQ(20 * crate::cache::AudioCache::kGiB, pPreferences->cacheLimitBytes());
    // Gigabytes on the slider, from 1 GB up
    EXPECT_EQ(21, pCache->limit()->value());
    EXPECT_EQ(1, pCache->limit()->minimum());
    // Showing the page chose nothing
    EXPECT_TRUE(config()->getValueString(
                                ConfigKey(QStringLiteral("[Crate]"),
                                        QStringLiteral("CacheLimitBytes")))
                        .isEmpty());

    crate::cache::AudioCache* pAudioCache = m_pServers->audioCache();
    ASSERT_NE(nullptr, pAudioCache);
    EXPECT_EQ(20 * crate::cache::AudioCache::kGiB, pAudioCache->limitBytes());
    ASSERT_TRUE(QTest::qWaitFor([&] { return pAudioCache->isLoaded(); }, 5000));

    // Let go at 40 GB: the cache takes it at once and the settings keep it
    pCache->limit()->setValue(40);
    EXPECT_EQ(40'000'000'000, pAudioCache->limitBytes());
    EXPECT_EQ(40'000'000'000, pPreferences->cacheLimitBytes());
    EXPECT_QSTRING_EQ(QStringLiteral("40000000000"),
            config()->getValueString(ConfigKey(
                    QStringLiteral("[Crate]"), QStringLiteral("CacheLimitBytes"))));
    EXPECT_EQ(40'000'000'000, crate::CratePreferences(config()).cacheLimitBytes());
    EXPECT_EQ(QStringLiteral("40 GB"),
            pCache->findChild<QLabel*>(QStringLiteral("CrateCacheLimitValue"))->text());
    // What the cache holds, on the ring
    EXPECT_EQ(QStringLiteral("of 40 GB"),
            pCache->findChild<QLabel*>(QStringLiteral("CrateCacheOf"))->text());
}

// TEST_GAP: "Choose…" opens a modal folder dialog; the test sets the
// preference it sets.
TEST_F(CrateAddServerFlowTest, CacheLocationTakesEffectAtTheNextStart) {
    crate::DlgPrefCrate page(nullptr, m_pServers.get());
    crate::CacheSection* pCache = page.cacheSection();
    crate::cache::AudioCache* pAudioCache = m_pServers->audioCache();
    EXPECT_QSTRING_EQ(QDir::toNativeSeparators(m_cacheDir.path()), pCache->location()->text());
    EXPECT_TRUE(pCache->locationHint()->isHidden());

    QTemporaryDir chosen;
    ASSERT_TRUE(chosen.isValid());
    m_pServers->preferences()->setCacheDirectory(chosen.path());

    // The cache in use stays where it is until the next start
    EXPECT_QSTRING_EQ(m_cacheDir.path(), pAudioCache->directory());
    EXPECT_QSTRING_EQ(QDir::toNativeSeparators(chosen.path()), pCache->location()->text());
    EXPECT_FALSE(pCache->locationHint()->isHidden());
    EXPECT_QSTRING_EQ(QDir::cleanPath(chosen.path()),
            config()->getValueString(
                    ConfigKey(QStringLiteral("[Crate]"), QStringLiteral("CacheDirectory"))));

    // The next start creates the cache there
    CrateServers::Dependencies dependencies;
    dependencies.pConfig = config();
    dependencies.pDbConnectionPool = dbConnectionPooler();
    dependencies.pNetworkAccessManager = &m_network;
    dependencies.pSecretStore = &m_store;
    CrateServers nextStart(dependencies);
    ASSERT_NE(nullptr, nextStart.audioCache());
    EXPECT_QSTRING_EQ(QDir::cleanPath(chosen.path()), nextStart.audioCache()->directory());
}

TEST_F(CrateAddServerFlowTest, PrefetchAndAnalysisSwitchesSetThePreferences) {
    crate::DlgPrefCrate page(nullptr, m_pServers.get());
    crate::CratePreferences* pPreferences = m_pServers->preferences();
    QCheckBox* pPrefetch = page.cacheSection()->prefetchAutoDj();
    QCheckBox* pKey = page.analysisSection()->keyFromCrate();
    QCheckBox* pGain = page.analysisSection()->gainFromLufs();
    EXPECT_FALSE(page.analysisSection()->isHidden());
    // Defaults: prefetch on; beats, key and gain from the local analysis
    EXPECT_TRUE(pPrefetch->isChecked());
    EXPECT_FALSE(pKey->isChecked());
    EXPECT_FALSE(pGain->isChecked());
    const crate::analysis::Policy defaults = pPreferences->analysisPolicy();
    EXPECT_FALSE(defaults.crateBeats);
    EXPECT_FALSE(defaults.crateKey);
    EXPECT_FALSE(defaults.gainFromLufs);
    // Prewarm (F5) and progressive playback (F4b) wait for their plans
    EXPECT_TRUE(page.cacheSection()->prewarm()->parentWidget()->isHidden());
    EXPECT_TRUE(page.cacheSection()->progressivePlayback()->parentWidget()->isHidden());

    QSignalSpy prefetchChanged(pPreferences, &crate::CratePreferences::prefetchAutoDjChanged);
    QSignalSpy policyChanged(pPreferences, &crate::CratePreferences::analysisPolicyChanged);
    pPrefetch->click();
    pKey->click();
    pGain->click();

    EXPECT_FALSE(pPreferences->prefetchAutoDj());
    ASSERT_EQ(1, prefetchChanged.size());
    EXPECT_EQ(2, policyChanged.size());
    const crate::analysis::Policy policy = pPreferences->analysisPolicy();
    EXPECT_FALSE(policy.crateBeats);
    EXPECT_TRUE(policy.crateKey);
    EXPECT_TRUE(policy.gainFromLufs);
    // Kept in the settings for the next start
    const crate::CratePreferences saved(config());
    EXPECT_FALSE(saved.prefetchAutoDj());
    EXPECT_TRUE(saved.keyFromCrate());
    EXPECT_TRUE(saved.gainFromLufs());

    // And a page opened later shows them
    crate::DlgPrefCrate again(nullptr, m_pServers.get());
    EXPECT_FALSE(again.cacheSection()->prefetchAutoDj()->isChecked());
    EXPECT_TRUE(again.analysisSection()->keyFromCrate()->isChecked());

    // Reset to defaults puts everything back
    page.slotResetToDefaults();
    EXPECT_TRUE(pPreferences->prefetchAutoDj());
    EXPECT_FALSE(pPreferences->keyFromCrate());
    EXPECT_FALSE(pPreferences->gainFromLufs());
    EXPECT_TRUE(pPrefetch->isChecked());
    EXPECT_FALSE(pKey->isChecked());
}

TEST_F(CrateAddServerFlowTest, CacheSectionShowsTheCacheUse) {
    crate::DlgPrefCrate page(nullptr, m_pServers.get());
    auto* pUsage = page.cacheSection()->findChild<QLabel*>(QStringLiteral("CrateCacheCaption"));
    ASSERT_NE(nullptr, pUsage);
    EXPECT_EQ(QStringLiteral("0 tracks · 0 downloading"), pUsage->text());

    // The cache comes once there is a server; the page follows it
    m_pFlow->setAddress(QStringLiteral("https://crate.test"));
    lastSent().pReply->respond(200, kDecksCapabilities);
    m_pFlow->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("right"));
    lastSent().pReply->respond(200, kLoginAnswer);
    ASSERT_EQ(1, m_pServers->sessions().size());
    const QString serverId = m_pServers->sessions().first()->profile().id;
    crate::cache::AudioCache* pAudioCache = m_pServers->audioCache();
    ASSERT_TRUE(QTest::qWaitFor([&] { return pAudioCache->isLoaded(); }, 5000));
    pAudioCache->ensure(serverId,
            QStringLiteral("0f6b3d9e-5a1c-4e2b-8d7f-1a2b3c4d5e61"),
            crate::cache::Priority::Deck);
    EXPECT_TRUE(QTest::qWaitFor(
            [&] { return pUsage->text() == QStringLiteral("0 tracks · 1 downloading"); },
            5000));
}

} // namespace
