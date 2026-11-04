#include "crate/crateservers.h"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <memory>

#include "crate/db/crateserverprofiles.h"
#include "crate/db/crateserverschema.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"

namespace {

using crate::CrateServers;
using crate::ServerProfile;
using crate::Session;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeSecretStore;

const QByteArray kLoginAnswer = QByteArrayLiteral(
        R"({"id": 7, "email": "dj@crate.test", "role": "user", "token": "access-1",
            "refresh_token": "refresh-1"})");

class CrateServersTest : public MixxxDbTest {
  protected:
    CrateServersTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection()));
    }

    std::unique_ptr<CrateServers> makeServers() {
        CrateServers::Dependencies dependencies;
        dependencies.pConfig = config();
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.pSecretStore = &m_store;
        return std::make_unique<CrateServers>(dependencies);
    }

    FakeNetworkAccessManager::Sent lastSent() const {
        return m_network.sent.last();
    }

    QList<ServerProfile> savedProfiles() const {
        return crate::db::loadServerProfiles(dbConnection());
    }

    FakeNetworkAccessManager m_network;
    FakeSecretStore m_store;
};

TEST_F(CrateServersTest, StartRestoresTheSavedServers) {
    ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
    ServerProfile saved;
    saved.id = QStringLiteral("a");
    saved.baseUrl = QUrl(QStringLiteral("https://a.test"));
    ASSERT_TRUE(crate::db::saveServerProfile(dbConnection(), saved));
    m_store.secrets.insert(QStringLiteral("a:refresh"), QStringLiteral("refresh-a"));
    const auto pServers = makeServers();

    pServers->start();

    ASSERT_EQ(1, pServers->sessions().size());
    EXPECT_EQ(QStringLiteral("a"), pServers->sessions().first()->profile().id);
    ASSERT_FALSE(m_network.sent.isEmpty());
    EXPECT_EQ(QUrl(QStringLiteral("https://a.test/api/auth/refresh")), lastSent().request.url());
    EXPECT_EQ(QStringLiteral("refresh-a"),
            QJsonDocument::fromJson(lastSent().body)
                    .object()
                    .value(QStringLiteral("refresh_token"))
                    .toString());
}

TEST_F(CrateServersTest, AddedServerIsSavedOnceSignedIn) {
    const auto pServers = makeServers();
    pServers->start();
    QSignalSpy changed(pServers.get(), &CrateServers::serversChanged);

    Session* pSession = pServers->addServer(
            QUrl(QStringLiteral("https://b.test")), QStringLiteral("Club"));
    ASSERT_NE(nullptr, pSession);
    EXPECT_TRUE(savedProfiles().isEmpty());
    pSession->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("pw"));
    ASSERT_FALSE(m_network.sent.isEmpty());
    EXPECT_EQ(QUrl(QStringLiteral("https://b.test/api/auth/login")), lastSent().request.url());
    lastSent().pReply->respond(200, kLoginAnswer);

    const QList<ServerProfile> profiles = savedProfiles();
    ASSERT_EQ(1, profiles.size());
    EXPECT_EQ(pSession->profile().id, profiles.first().id);
    EXPECT_FALSE(profiles.first().id.isEmpty());
    EXPECT_EQ(QUrl(QStringLiteral("https://b.test")), profiles.first().baseUrl);
    EXPECT_EQ(QStringLiteral("Club"), profiles.first().displayName);
    EXPECT_EQ(QStringLiteral("7"), profiles.first().accountId);
    EXPECT_GE(changed.count(), 1);
}

TEST_F(CrateServersTest, RemoveServerSignsOutAndForgets) {
    const auto pServers = makeServers();
    pServers->start();
    Session* pSession = pServers->addServer(
            QUrl(QStringLiteral("https://b.test")), QStringLiteral("Club"));
    ASSERT_NE(nullptr, pSession);
    pSession->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("pw"));
    ASSERT_FALSE(m_network.sent.isEmpty());
    lastSent().pReply->respond(200, kLoginAnswer);
    const QString id = pSession->profile().id;

    pServers->removeServer(id);

    EXPECT_TRUE(pServers->sessions().isEmpty());
    EXPECT_TRUE(savedProfiles().isEmpty());
    EXPECT_FALSE(m_store.secrets.contains(id + QStringLiteral(":refresh")));
    EXPECT_EQ(QUrl(QStringLiteral("https://b.test/api/auth/logout")), lastSent().request.url());
    EXPECT_EQ(QByteArrayLiteral("Bearer access-1"), lastSent().request.rawHeader("Authorization"));
    // The revoke is still on its way: the removed server's client is kept.
    ASSERT_TRUE(lastSent().pReply);
    EXPECT_FALSE(lastSent().pReply->aborted());
}

TEST_F(CrateServersTest, ServerRemovedBeforeSigningInIsNeverSaved) {
    const auto pServers = makeServers();
    pServers->start();
    Session* pSession = pServers->addServer(
            QUrl(QStringLiteral("https://b.test")), QStringLiteral("Club"));
    ASSERT_NE(nullptr, pSession);

    pServers->removeServer(pSession->profile().id);

    EXPECT_TRUE(pServers->sessions().isEmpty());
    EXPECT_TRUE(savedProfiles().isEmpty());
    EXPECT_TRUE(m_network.sent.isEmpty());
}

} // namespace
