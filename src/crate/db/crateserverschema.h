#pragma once

#include <QSqlDatabase>

namespace crate::db {

/// Creates the tables of the Crate integration in the Mixxx database if they
/// do not exist yet, in one transaction. Safe to call at every start.
///
/// The tables carry the `crateserver_` prefix and are never part of
/// res/schema.xml; no Mixxx table is touched (AGENTS.md §5.2). Later phases
/// add their tables here.
///
/// - `crateserver_servers`: the servers this installation signed in to. `id`
///   is a random UUID that also names the server's keychain entry.
///
/// Returns false, after rolling back, if any statement fails.
bool createCrateServerSchema(const QSqlDatabase& database);

} // namespace crate::db
