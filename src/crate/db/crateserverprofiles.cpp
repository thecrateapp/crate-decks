#include "crate/db/crateserverprofiles.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "util/logger.h"

namespace crate::db {

namespace {

const mixxx::Logger kLogger("crate::db::ServerProfiles");

bool run(QSqlQuery& query, const char* what) {
    if (query.exec()) {
        return true;
    }
    kLogger.warning() << "Cannot" << what << ":" << query.lastError().text();
    return false;
}

} // namespace

QList<ServerProfile> loadServerProfiles(const QSqlDatabase& database) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "SELECT id, origin, account_id, display_name FROM crateserver_servers "
            "ORDER BY created_at, rowid"));
    QList<ServerProfile> profiles;
    if (!run(query, "load the Crate servers")) {
        return profiles;
    }
    while (query.next()) {
        ServerProfile profile;
        profile.id = query.value(0).toString();
        profile.baseUrl = QUrl(query.value(1).toString());
        profile.accountId = query.value(2).toString();
        profile.displayName = query.value(3).toString();
        profiles.append(profile);
    }
    return profiles;
}

bool saveServerProfile(const QSqlDatabase& database, const ServerProfile& profile) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "INSERT INTO crateserver_servers "
            "(id, origin, account_id, display_name, created_at) "
            "VALUES (:id, :origin, :account_id, :display_name, :created_at) "
            "ON CONFLICT(id) DO UPDATE SET origin = excluded.origin, "
            "account_id = excluded.account_id, display_name = excluded.display_name"));
    query.bindValue(QStringLiteral(":id"), profile.id);
    query.bindValue(QStringLiteral(":origin"), profile.baseUrl.toString(QUrl::FullyEncoded));
    query.bindValue(QStringLiteral(":account_id"), profile.accountId);
    query.bindValue(QStringLiteral(":display_name"), profile.displayName);
    query.bindValue(QStringLiteral(":created_at"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return run(query, "save a Crate server");
}

bool saveLastCapabilities(const QSqlDatabase& database,
        const QString& serverId,
        const QByteArray& capabilitiesJson) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "UPDATE crateserver_servers SET last_capabilities_json = :json WHERE id = :id"));
    query.bindValue(QStringLiteral(":json"), QString::fromUtf8(capabilitiesJson));
    query.bindValue(QStringLiteral(":id"), serverId);
    return run(query, "save the capabilities of a Crate server") && query.numRowsAffected() == 1;
}

QByteArray loadLastCapabilities(const QSqlDatabase& database, const QString& serverId) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "SELECT last_capabilities_json FROM crateserver_servers WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), serverId);
    if (!run(query, "load the capabilities of a Crate server") || !query.next()) {
        return QByteArray();
    }
    return query.value(0).toString().toUtf8();
}

bool removeServerProfile(const QSqlDatabase& database, const QString& serverId) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM crateserver_servers WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), serverId);
    return run(query, "remove a Crate server");
}

} // namespace crate::db
