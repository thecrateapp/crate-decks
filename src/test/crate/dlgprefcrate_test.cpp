#include "crate/ui/dlgprefcrate.h"

#include <gtest/gtest.h>

#include <QDialog>
#include <QFrame>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <memory>

#include "crate/client/crateserveraddress.h"
#include "crate/crateaddserverflow.h"
#include "crate/crateserverrows.h"
#include "crate/crateservers.h"
#include "crate/db/crateserverprofiles.h"
#include "crate/ui/cratefirstrun.h"
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

} // namespace
