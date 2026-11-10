#include "crate/db/crateserverschema.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

#include "util/db/sqltransaction.h"
#include "util/logger.h"

namespace crate::db {

namespace {

const mixxx::Logger kLogger("crate::db::CrateServerSchema");

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
    for (const QString& statement : kStatements) {
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
