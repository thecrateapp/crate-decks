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

TEST_F(CrateServerSchemaTest, CreatesLibraryTablesWithTheirIndexes) {
    ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
    QSqlQuery insert(dbConnection());
    ASSERT_TRUE(insert.exec(QStringLiteral(
            "INSERT INTO crateserver_tracks (server_id, entity_uid, updated_at) "
            "VALUES ('5b0c', 'e1', '2026-11-09T10:00:00Z')")));

    ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));

    // Design §6, with an integer id: Mixxx's track models key their rows on
    // one (TrackId), and (server_id, entity_uid) stays unique
    const QStringList trackColumns = columns(QStringLiteral("crateserver_tracks"));
    for (const char* column : {"id",
                 "server_id",
                 "entity_uid",
                 "global_track_uid",
                 "mixxx_track_id",
                 "title",
                 "artist",
                 "album",
                 "duration_ms",
                 "bpm",
                 "camelot",
                 "energy",
                 "genre",
                 "format",
                 "bitrate",
                 "sample_rate",
                 "bit_depth",
                 "byte_length",
                 "source_revision",
                 "profile_revision",
                 "profile_quality",
                 "playable",
                 "cover_url",
                 "updated_at"}) {
        EXPECT_TRUE(trackColumns.contains(QLatin1String(column))) << column;
    }
    for (const char* column : {"id",
                 "server_id",
                 "kind",
                 "remote_id",
                 "name",
                 "parent_id",
                 "position",
                 "track_count",
                 "etag",
                 "synced_etag",
                 "pages",
                 "dirty",
                 "fetched_at"}) {
        EXPECT_TRUE(columns(QStringLiteral("crateserver_containers"))
                        .contains(QLatin1String(column)))
                << column;
    }
    EXPECT_EQ(QStringList({QStringLiteral("container_id"),
                      QStringLiteral("position"),
                      QStringLiteral("entity_uid")}),
            columns(QStringLiteral("crateserver_container_tracks")));

    QSqlQuery duplicate(dbConnection());
    EXPECT_FALSE(duplicate.exec(QStringLiteral(
            "INSERT INTO crateserver_tracks (server_id, entity_uid, updated_at) "
            "VALUES ('5b0c', 'e1', '2026-11-09T10:00:00Z')")));
    QSqlQuery plan(dbConnection());
    ASSERT_TRUE(plan.exec(QStringLiteral(
            "EXPLAIN QUERY PLAN SELECT entity_uid FROM crateserver_container_tracks "
            "WHERE container_id = 1 ORDER BY position")));
    QString steps;
    while (plan.next()) {
        steps += plan.value(3).toString() + QLatin1Char('\n');
    }
    EXPECT_FALSE(
            steps.contains(QStringLiteral("SCAN crateserver_container_tracks")))
            << steps.toStdString();
    EXPECT_FALSE(steps.contains(QStringLiteral("TEMP B-TREE"))) << steps.toStdString();
}

} // namespace
