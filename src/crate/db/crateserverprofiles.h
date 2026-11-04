#pragma once

#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "crate/session/cratesession.h"

namespace crate::db {

/// The servers this installation signed in to, from `crateserver_servers`,
/// oldest first. `accountEmail` is never stored.
QList<ServerProfile> loadServerProfiles(const QSqlDatabase& database);

/// Adds the profile, or updates the one with the same id. Returns false if
/// it fails, e.g. because another profile already has that origin.
bool saveServerProfile(const QSqlDatabase& database, const ServerProfile& profile);

/// Keeps the last answer of `GET /api/capabilities` of a server, to show its
/// features while it cannot be reached.
bool saveLastCapabilities(const QSqlDatabase& database,
        const QString& serverId,
        const QByteArray& capabilitiesJson);
QByteArray loadLastCapabilities(const QSqlDatabase& database, const QString& serverId);

bool removeServerProfile(const QSqlDatabase& database, const QString& serverId);

} // namespace crate::db
