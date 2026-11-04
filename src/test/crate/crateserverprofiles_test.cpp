#include "crate/db/crateserverprofiles.h"

#include <gtest/gtest.h>

#include "crate/db/crateserverschema.h"
#include "test/mixxxdbtest.h"

namespace {

using crate::ServerProfile;

class CrateServerProfilesTest : public MixxxDbTest {
  protected:
    CrateServerProfilesTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection()));
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
    }

    static ServerProfile profile(const char* id, const char* origin) {
        ServerProfile server;
        server.id = QString::fromLatin1(id);
        server.baseUrl = QUrl(QString::fromLatin1(origin));
        server.displayName = QStringLiteral("Home");
        server.accountId = QStringLiteral("7");
        server.accountEmail = QStringLiteral("dj@crate.test");
        return server;
    }
};

TEST_F(CrateServerProfilesTest, SavesLoadsAndRemovesProfiles) {
    ASSERT_TRUE(crate::db::saveServerProfile(dbConnection(), profile("a", "https://a.test")));
    ASSERT_TRUE(crate::db::saveServerProfile(dbConnection(), profile("b", "http://nas:8585")));

    const QList<ServerProfile> loaded = crate::db::loadServerProfiles(dbConnection());
    ASSERT_EQ(2, loaded.size());
    EXPECT_EQ(QStringLiteral("a"), loaded[0].id);
    EXPECT_EQ(QUrl(QStringLiteral("https://a.test")), loaded[0].baseUrl);
    EXPECT_EQ(QStringLiteral("Home"), loaded[0].displayName);
    EXPECT_EQ(QStringLiteral("7"), loaded[0].accountId);
    EXPECT_TRUE(loaded[0].accountEmail.isEmpty());
    EXPECT_EQ(QStringLiteral("b"), loaded[1].id);

    ASSERT_TRUE(crate::db::removeServerProfile(dbConnection(), QStringLiteral("a")));
    ASSERT_EQ(1, crate::db::loadServerProfiles(dbConnection()).size());
}

TEST_F(CrateServerProfilesTest, SavingAgainUpdatesTheProfile) {
    ASSERT_TRUE(crate::db::saveServerProfile(dbConnection(), profile("a", "https://a.test")));
    ServerProfile renamed = profile("a", "https://a.test");
    renamed.displayName = QStringLiteral("Club");
    renamed.accountId = QStringLiteral("8");

    ASSERT_TRUE(crate::db::saveServerProfile(dbConnection(), renamed));

    const QList<ServerProfile> loaded = crate::db::loadServerProfiles(dbConnection());
    ASSERT_EQ(1, loaded.size());
    EXPECT_EQ(QStringLiteral("Club"), loaded[0].displayName);
    EXPECT_EQ(QStringLiteral("8"), loaded[0].accountId);
}

TEST_F(CrateServerProfilesTest, OneProfilePerOrigin) {
    ASSERT_TRUE(crate::db::saveServerProfile(dbConnection(), profile("a", "https://a.test")));

    EXPECT_FALSE(crate::db::saveServerProfile(dbConnection(), profile("b", "https://a.test")));
    EXPECT_EQ(1, crate::db::loadServerProfiles(dbConnection()).size());
}

TEST_F(CrateServerProfilesTest, KeepsTheLastCapabilities) {
    ASSERT_TRUE(crate::db::saveServerProfile(dbConnection(), profile("a", "https://a.test")));
    const QByteArray capabilities = QByteArrayLiteral(R"({"decks": {"available": true}})");

    ASSERT_TRUE(crate::db::saveLastCapabilities(dbConnection(), QStringLiteral("a"), capabilities));

    EXPECT_EQ(capabilities, crate::db::loadLastCapabilities(dbConnection(), QStringLiteral("a")));
    EXPECT_TRUE(crate::db::loadLastCapabilities(dbConnection(), QStringLiteral("b")).isEmpty());
}

} // namespace
