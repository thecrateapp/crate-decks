#include "crate/db/crateserverschema.h"

#include <gtest/gtest.h>

#include <QSqlQuery>
#include <QStringList>

#include "test/mixxxdbtest.h"

namespace {

class CrateServerSchemaTest : public MixxxDbTest {
  protected:
    CrateServerSchemaTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        // The tables of Mixxx, so that the test checks they stay untouched.
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection()));
    }

    /// Every table, index, view and trigger that does not belong to a Crate
    /// table, with its SQL. SQLite names the automatic indexes of a table
    /// sqlite_autoindex_<table>_N, so they are grouped by tbl_name.
    QStringList mixxxObjects() const {
        QStringList objects;
        QSqlQuery query(dbConnection());
        EXPECT_TRUE(query.exec(QStringLiteral(
                "SELECT type, name, sql FROM sqlite_master "
                "WHERE tbl_name NOT LIKE 'crateserver\\_%' ESCAPE '\\' ORDER BY type, name")));
        while (query.next()) {
            objects.append(query.value(0).toString() + QLatin1Char(' ') +
                    query.value(1).toString() + QLatin1Char(' ') + query.value(2).toString());
        }
        return objects;
    }

    QStringList columns(const QString& table) const {
        QStringList names;
        QSqlQuery query(dbConnection());
        EXPECT_TRUE(query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)));
        while (query.next()) {
            names.append(query.value(1).toString());
        }
        return names;
    }
};

TEST_F(CrateServerSchemaTest, CreatesTablesIdempotently) {
    ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
    QSqlQuery insert(dbConnection());
    ASSERT_TRUE(insert.exec(QStringLiteral(
            "INSERT INTO crateserver_servers (id, origin, created_at) "
            "VALUES ('5b0c', 'https://crate.test', '2026-11-04T10:00:00Z')")));

    ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));

    // The columns of design §5.2: no secrets.
    EXPECT_EQ(QStringList({QStringLiteral("id"),
                      QStringLiteral("origin"),
                      QStringLiteral("account_id"),
                      QStringLiteral("display_name"),
                      QStringLiteral("last_capabilities_json"),
                      QStringLiteral("created_at")}),
            columns(QStringLiteral("crateserver_servers")));
    QSqlQuery count(dbConnection());
    ASSERT_TRUE(count.exec(QStringLiteral("SELECT COUNT(*) FROM crateserver_servers")));
    ASSERT_TRUE(count.next());
    EXPECT_EQ(1, count.value(0).toInt());
}

TEST_F(CrateServerSchemaTest, DoesNotTouchMixxxTables) {
    const QStringList before = mixxxObjects();
    ASSERT_FALSE(before.isEmpty());

    ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));

    EXPECT_EQ(before, mixxxObjects());
}

} // namespace
