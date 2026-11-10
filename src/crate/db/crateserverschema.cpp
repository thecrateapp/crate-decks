#include "crate/db/crateserverschema.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

#include "util/db/sqltransaction.h"
#include "util/logger.h"

namespace crate::db {

namespace {

const mixxx::Logger kLogger("crate::db::CrateServerSchema");

// F3: the Crate library as Decks has synced it (design §6). Separate from
// kStatements so that each phase adds its tables without touching another's.
const QStringList kLibraryStatements = {
        // The catalogue: one row per track of a server that some container
        // listed. `id` keys the rows of Mixxx's track models (TrackId); the
        // track's identity is (server_id, entity_uid). `mixxx_track_id` is
        // set once the track is in Mixxx's library (F4).
        QStringLiteral("CREATE TABLE IF NOT EXISTS crateserver_tracks ("
                       "id INTEGER PRIMARY KEY, "
                       "server_id TEXT NOT NULL, "
                       "entity_uid TEXT NOT NULL, "
                       "global_track_uid TEXT, "
                       "mixxx_track_id INTEGER, "
                       "title TEXT NOT NULL DEFAULT '', "
                       "artist TEXT NOT NULL DEFAULT '', "
                       "album TEXT NOT NULL DEFAULT '', "
                       "duration_ms INTEGER, "
                       "bpm REAL, "
                       "camelot TEXT, "
                       "energy REAL, "
                       "genre TEXT, "
                       "format TEXT, "
                       "bitrate INTEGER, "
                       "sample_rate INTEGER, "
                       "bit_depth INTEGER, "
                       "byte_length INTEGER, "
                       "source_revision TEXT, "
                       "profile_revision TEXT, "
                       "profile_quality TEXT, "
                       "playable INTEGER, "
                       "cover_url TEXT, "
                       "updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS crateserver_tracks_entity "
                       "ON crateserver_tracks (server_id, entity_uid)"),
        // The sidebar's tree. `etag` is the one the server's tree gives: it
        // changes with the container's membership or order. `synced_etag` is
        // the one the stored tracks match. `pages` is a JSON array with the
        // cursor, ETag and track count of each page of the last sync: a page
        // ETag also changes with the tracks' metadata, so every page is
        // revalidated with If-None-Match. `track_count` is the server's
        // count, when it has one. `dirty` is set by the server's
        // invalidation events.
        QStringLiteral("CREATE TABLE IF NOT EXISTS crateserver_containers ("
                       "id INTEGER PRIMARY KEY, "
                       "server_id TEXT NOT NULL, "
                       "kind TEXT NOT NULL, "
                       "remote_id TEXT NOT NULL, "
                       "name TEXT NOT NULL DEFAULT '', "
                       "parent_id INTEGER, "
                       "position INTEGER NOT NULL DEFAULT 0, "
                       "track_count INTEGER, "
                       "etag TEXT NOT NULL DEFAULT '', "
                       "synced_etag TEXT NOT NULL DEFAULT '', "
                       "pages TEXT NOT NULL DEFAULT '', "
                       "dirty INTEGER NOT NULL DEFAULT 0, "
                       "fetched_at TEXT, "
                       "UNIQUE (server_id, kind, remote_id))"),
        // The tracks of each container, in order.
        QStringLiteral("CREATE TABLE IF NOT EXISTS crateserver_container_tracks ("
                       "container_id INTEGER NOT NULL, "
                       "position INTEGER NOT NULL, "
                       "entity_uid TEXT NOT NULL, "
                       "PRIMARY KEY (container_id, position))"),
        // What the server's tree says of the whole library, which is not a
        // container. `track_count` is NULL when the server does not give it.
        QStringLiteral("CREATE TABLE IF NOT EXISTS crateserver_libraries ("
                       "server_id TEXT PRIMARY KEY NOT NULL, "
                       "track_count INTEGER)"),
};

// Each statement must be idempotent: they run at every start.
const QStringList kStatements = {
        // Design §5.2. No secrets: the refresh token is in the keychain.
        QStringLiteral("CREATE TABLE IF NOT EXISTS crateserver_servers ("
                       "id TEXT PRIMARY KEY NOT NULL, "
                       "origin TEXT NOT NULL UNIQUE, "
                       "account_id TEXT NOT NULL DEFAULT '', "
                       "display_name TEXT NOT NULL DEFAULT '', "
                       "last_capabilities_json TEXT NOT NULL DEFAULT '', "
                       "created_at TEXT NOT NULL)"),
        // Smart Mix Stems ST4: the models crate::ModelStore installed and
        // checked, one version per model. file_name is inside the models
        // directory of the data directory.
        QStringLiteral("CREATE TABLE IF NOT EXISTS crateserver_models ("
                       "name TEXT PRIMARY KEY NOT NULL, "
                       "version TEXT NOT NULL, "
                       "file_name TEXT NOT NULL, "
                       "sha256 TEXT NOT NULL, "
                       "bytes INTEGER NOT NULL, "
                       "installed_at TEXT NOT NULL)"),
};

} // namespace

bool createCrateServerSchema(const QSqlDatabase& database) {
    SqlTransaction transaction(database);
    if (!transaction) {
        kLogger.warning() << "Cannot start a transaction for the Crate tables";
        return false;
    }
    const QStringList statements = kStatements + kLibraryStatements;
    for (const QString& statement : statements) {
        QSqlQuery query(database);
        if (!query.exec(statement)) {
            kLogger.warning() << "Cannot create the Crate tables:" << query.lastError().text();
            transaction.rollback();
            return false;
        }
    }
    return transaction.commit();
}

} // namespace crate::db
