#include "crate/ui/dlgprefcrate.h"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>
#include <QUrl>
#include <memory>

#include "crate/client/crateserveraddress.h"
#include "crate/crateaddserverflow.h"
#include "crate/crateservers.h"
#include "crate/db/crateserverprofiles.h"
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

} // namespace
