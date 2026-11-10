#include "crate/library/cratelibrarystore.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::LibraryStore");

// SQLITE_BUSY and SQLITE_LOCKED; the extended codes keep them in the low byte.
constexpr int kSqliteBusy = 5;
constexpr int kSqliteLocked = 6;

WriteStatus statusOf(const QSqlError& error) {
    bool ok = false;
    const int code = error.nativeErrorCode().toInt(&ok);
    if (ok && ((code & 0xff) == kSqliteBusy || (code & 0xff) == kSqliteLocked)) {
        return WriteStatus::Busy;
    }
    return WriteStatus::Failed;
}

/// A null QString binds as SQL NULL; the text columns are NOT NULL.
QString text(const QString& value) {
    return value.isNull() ? QLatin1String("") : value;
}

/// NULL for an empty string, for the nullable text columns.
QVariant nullable(const QString& value) {
    return value.isEmpty() ? QVariant() : QVariant(value);
}

template<typename T>
QVariant nullable(const std::optional<T>& value) {
    return value ? QVariant::fromValue(*value) : QVariant();
}

bool exec(QSqlQuery& query, QSqlError* pError) {
    if (query.exec()) {
        return true;
    }
    *pError = query.lastError();
    return false;
}

/// Runs `body` in a transaction. `body` returns false, after setting the
/// error, to roll back.
template<typename Body>
WriteStatus inTransaction(const QSqlDatabase& database, const char* what, Body&& body) {
    QSqlDatabase db = database;
    if (!db.transaction()) {
        const QSqlError error = db.lastError();
        kLogger.debug() << "Cannot start a transaction to" << what << ":" << error.text();
        return statusOf(error);
    }
    QSqlError error;
    if (!body(&error)) {
        db.rollback();
        const WriteStatus status = statusOf(error);
        if (status == WriteStatus::Failed) {
            kLogger.warning() << "Cannot" << what << ":" << error.text();
        }
        return status;
    }
    if (!db.commit()) {
        error = db.lastError();
        db.rollback();
        return statusOf(error);
    }
    return WriteStatus::Ok;
}

QString pagesToJson(const QList<PageState>& pages) {
    QJsonArray array;
    for (const PageState& page : pages) {
        array.append(QJsonObject{
                {QStringLiteral("cursor"), page.cursor},
                {QStringLiteral("etag"), page.etag},
                {QStringLiteral("count"), page.trackCount},
        });
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QList<PageState> pagesFromJson(const QString& json) {
    QList<PageState> pages;
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).array();
    pages.reserve(array.size());
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        pages.append({object.value(QStringLiteral("cursor")).toString(),
                object.value(QStringLiteral("etag")).toString(),
                object.value(QStringLiteral("count")).toInt()});
    }
    return pages;
}

const QString kContainerColumns = QStringLiteral(
        "id, server_id, kind, remote_id, name, parent_id, position, track_count, "
        "etag, synced_etag, pages, dirty, fetched_at");

StoredContainer containerOf(const QSqlQuery& query) {
    StoredContainer container;
    container.id = query.value(0).toLongLong();
    container.serverId = query.value(1).toString();
    container.kind = query.value(2).toString();
    container.remoteId = query.value(3).toString();
    container.name = query.value(4).toString();
    container.parentId = query.value(5).toLongLong();
    container.position = query.value(6).toInt();
    if (!query.value(7).isNull()) {
        container.trackCount = query.value(7).toLongLong();
    }
    container.etag = query.value(8).toString();
    container.syncedEtag = query.value(9).toString();
    container.pages = pagesFromJson(query.value(10).toString());
    container.dirty = query.value(11).toBool();
    container.fetchedAt = query.value(12).toString();
    return container;
}

QString keyOf(const QString& kind, const QString& remoteId) {
    return kind + QLatin1Char('\n') + remoteId;
}

} // namespace

std::optional<qint64> StoredContainer::syncedTrackCount() const {
    if (fetchedAt.isEmpty()) {
        return std::nullopt;
    }
    qint64 count = 0;
    for (const PageState& page : pages) {
        count += page.trackCount;
    }
    return count;
}

QList<StoredContainer> loadContainers(const QSqlDatabase& database, const QString& serverId) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "SELECT %1 FROM crateserver_containers "
            "WHERE server_id = :server ORDER BY position, id")
                    .arg(kContainerColumns));
    query.bindValue(QStringLiteral(":server"), serverId);
    QList<StoredContainer> containers;
    if (!query.exec()) {
        kLogger.warning() << "Cannot load the Crate containers:" << query.lastError().text();
        return containers;
    }
    while (query.next()) {
        containers.append(containerOf(query));
    }
    return containers;
}

std::optional<StoredContainer> loadContainer(const QSqlDatabase& database, qint64 containerId) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT %1 FROM crateserver_containers WHERE id = :id")
                    .arg(kContainerColumns));
    query.bindValue(QStringLiteral(":id"), containerId);
    if (!query.exec()) {
        kLogger.warning() << "Cannot load a Crate container:" << query.lastError().text();
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return containerOf(query);
}

WriteStatus storeContainerTree(
        const QSqlDatabase& database, const QString& serverId, const ContainerTree& tree) {
    const QList<RemoteContainer>& containers = tree.containers;
    return inTransaction(database, "store the Crate containers", [&](QSqlError* pError) {
        QSqlQuery library(database);
        library.prepare(QStringLiteral(
                "INSERT INTO crateserver_libraries (server_id, track_count) "
                "VALUES (:server, :count) "
                "ON CONFLICT(server_id) DO UPDATE SET track_count = excluded.track_count"));
        library.bindValue(QStringLiteral(":server"), serverId);
        library.bindValue(QStringLiteral(":count"), nullable(tree.libraryTrackCount));
        if (!exec(library, pError)) {
            return false;
        }

        QSqlQuery upsert(database);
        upsert.prepare(QStringLiteral(
                "INSERT INTO crateserver_containers "
                "(server_id, kind, remote_id, name, position, track_count, etag) "
                "VALUES (:server, :kind, :remote, :name, :position, :count, :etag) "
                "ON CONFLICT(server_id, kind, remote_id) DO UPDATE SET "
                "name = excluded.name, position = excluded.position, "
                "track_count = excluded.track_count, etag = excluded.etag"));
        QSet<QString> listed;
        for (int i = 0; i < containers.size(); ++i) {
            const RemoteContainer& container = containers[i];
            upsert.bindValue(QStringLiteral(":server"), serverId);
            upsert.bindValue(QStringLiteral(":kind"), container.kind);
            upsert.bindValue(QStringLiteral(":remote"), container.remoteId);
            upsert.bindValue(QStringLiteral(":name"), text(container.name));
            upsert.bindValue(QStringLiteral(":position"), i);
            upsert.bindValue(QStringLiteral(":count"), nullable(container.trackCount));
            upsert.bindValue(QStringLiteral(":etag"), text(container.etag));
            if (!exec(upsert, pError)) {
                return false;
            }
            listed.insert(keyOf(container.kind, container.remoteId));
        }

        QSqlQuery ids(database);
        ids.prepare(QStringLiteral(
                "SELECT id, kind, remote_id FROM crateserver_containers WHERE "
                "server_id = :server"));
        ids.bindValue(QStringLiteral(":server"), serverId);
        if (!exec(ids, pError)) {
            return false;
        }
        QHash<QString, qint64> idOf;
        QList<qint64> removed;
        while (ids.next()) {
            const QString key = keyOf(ids.value(1).toString(), ids.value(2).toString());
            if (listed.contains(key)) {
                idOf.insert(key, ids.value(0).toLongLong());
            } else {
                removed.append(ids.value(0).toLongLong());
            }
        }
        ids.finish();

        // Parents are resolved once every node has its id, so a child listed
        // before its parent still finds it.
        QSqlQuery parent(database);
        parent.prepare(QStringLiteral(
                "UPDATE crateserver_containers SET parent_id = :parent WHERE id = :id"));
        for (const RemoteContainer& container : containers) {
            const qint64 parentId = container.parentRemoteId.isEmpty()
                    ? 0
                    : idOf.value(keyOf(container.kind, container.parentRemoteId), 0);
            parent.bindValue(QStringLiteral(":parent"),
                    parentId > 0 ? QVariant(parentId) : QVariant());
            parent.bindValue(QStringLiteral(":id"),
                    idOf.value(keyOf(container.kind, container.remoteId)));
            if (!exec(parent, pError)) {
                return false;
            }
        }

        QSqlQuery removeTracks(database);
        removeTracks.prepare(QStringLiteral(
                "DELETE FROM crateserver_container_tracks WHERE container_id = :id"));
        QSqlQuery removeContainer(database);
        removeContainer.prepare(
                QStringLiteral("DELETE FROM crateserver_containers WHERE id = :id"));
        for (const qint64 id : std::as_const(removed)) {
            removeTracks.bindValue(QStringLiteral(":id"), id);
            removeContainer.bindValue(QStringLiteral(":id"), id);
            if (!exec(removeTracks, pError) || !exec(removeContainer, pError)) {
                return false;
            }
        }

        // NOT IN builds one temporary index; a correlated NOT EXISTS would
        // scan the container tracks once per catalogue track.
        QSqlQuery orphans(database);
        orphans.prepare(QStringLiteral(
                "DELETE FROM crateserver_tracks WHERE server_id = :server "
                "AND mixxx_track_id IS NULL AND entity_uid NOT IN ("
                "SELECT ct.entity_uid FROM crateserver_container_tracks ct "
                "JOIN crateserver_containers c ON c.id = ct.container_id "
                "WHERE c.server_id = :server2)"));
        orphans.bindValue(QStringLiteral(":server"), serverId);
        orphans.bindValue(QStringLiteral(":server2"), serverId);
        return exec(orphans, pError);
    });
}

WriteStatus storeContainerTree(const QSqlDatabase& database,
        const QString& serverId,
        const QList<RemoteContainer>& containers) {
    return storeContainerTree(database, serverId, ContainerTree{containers, std::nullopt});
}

std::optional<qint64> loadLibraryTrackCount(
        const QSqlDatabase& database, const QString& serverId) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "SELECT track_count FROM crateserver_libraries WHERE server_id = :server"));
    query.bindValue(QStringLiteral(":server"), serverId);
    if (!query.exec()) {
        kLogger.warning() << "Cannot load the Crate library count:" << query.lastError().text();
        return std::nullopt;
    }
    if (!query.next() || query.isNull(0)) {
        return std::nullopt;
    }
    return query.value(0).toLongLong();
}

WriteStatus storeTracks(const QSqlDatabase& database,
        const QString& serverId,
        const QList<RemoteTrack>& tracks,
        const QString& now) {
    if (tracks.isEmpty()) {
        return WriteStatus::Ok;
    }
    return inTransaction(database, "store the Crate tracks", [&](QSqlError* pError) {
        QSqlQuery upsert(database);
        upsert.prepare(QStringLiteral(
                "INSERT INTO crateserver_tracks (server_id, entity_uid, global_track_uid, "
                "title, artist, album, duration_ms, bpm, camelot, energy, genre, format, "
                "bitrate, sample_rate, bit_depth, byte_length, source_revision, "
                "profile_revision, profile_quality, playable, cover_url, updated_at) "
                "VALUES (:server, :uid, :global, :title, :artist, :album, :duration, :bpm, "
                ":camelot, :energy, :genre, :format, :bitrate, :rate, :depth, :bytes, "
                ":source, :profile, :quality, :playable, :cover, :now) "
                "ON CONFLICT(server_id, entity_uid) DO UPDATE SET "
                "global_track_uid = excluded.global_track_uid, title = excluded.title, "
                "artist = excluded.artist, album = excluded.album, "
                "duration_ms = excluded.duration_ms, bpm = excluded.bpm, "
                "camelot = excluded.camelot, energy = excluded.energy, "
                "genre = excluded.genre, format = excluded.format, "
                "bitrate = excluded.bitrate, sample_rate = excluded.sample_rate, "
                "bit_depth = excluded.bit_depth, byte_length = excluded.byte_length, "
                "source_revision = excluded.source_revision, "
                "profile_revision = excluded.profile_revision, "
                "profile_quality = excluded.profile_quality, "
                "playable = excluded.playable, cover_url = excluded.cover_url, "
                "updated_at = excluded.updated_at"));
        for (const RemoteTrack& track : tracks) {
            upsert.bindValue(QStringLiteral(":server"), serverId);
            upsert.bindValue(QStringLiteral(":uid"), track.entityUid);
            upsert.bindValue(QStringLiteral(":global"), nullable(track.globalTrackUid));
            upsert.bindValue(QStringLiteral(":title"), text(track.title));
            upsert.bindValue(QStringLiteral(":artist"), text(track.artist));
            upsert.bindValue(QStringLiteral(":album"), text(track.album));
            upsert.bindValue(QStringLiteral(":duration"), nullable(track.durationMs));
            upsert.bindValue(QStringLiteral(":bpm"), nullable(track.bpm));
            upsert.bindValue(QStringLiteral(":camelot"), nullable(track.camelot));
            upsert.bindValue(QStringLiteral(":energy"), nullable(track.energy));
            upsert.bindValue(QStringLiteral(":genre"), nullable(track.genre));
            upsert.bindValue(QStringLiteral(":format"), nullable(track.format));
            upsert.bindValue(QStringLiteral(":bitrate"), nullable(track.bitrate));
            upsert.bindValue(QStringLiteral(":rate"), nullable(track.sampleRate));
            upsert.bindValue(QStringLiteral(":depth"), nullable(track.bitDepth));
            upsert.bindValue(QStringLiteral(":bytes"), nullable(track.byteLength));
            upsert.bindValue(QStringLiteral(":source"), nullable(track.sourceRevision));
            upsert.bindValue(QStringLiteral(":profile"), nullable(track.profileRevision));
            upsert.bindValue(QStringLiteral(":quality"), nullable(track.profileQuality));
            upsert.bindValue(QStringLiteral(":playable"), nullable(track.playable));
            upsert.bindValue(QStringLiteral(":cover"), nullable(track.coverUrl));
            upsert.bindValue(QStringLiteral(":now"), now);
            if (!exec(upsert, pError)) {
                return false;
            }
        }
        return true;
    });
}

QStringList loadContainerTrackUids(
        const QSqlDatabase& database, qint64 containerId, int first, int count) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "SELECT entity_uid FROM crateserver_container_tracks "
            "WHERE container_id = :container AND position >= :first AND position < :end "
            "ORDER BY position"));
    query.bindValue(QStringLiteral(":container"), containerId);
    query.bindValue(QStringLiteral(":first"), first);
    query.bindValue(QStringLiteral(":end"), first + count);
    QStringList uids;
    if (!query.exec()) {
        kLogger.warning() << "Cannot load the tracks of a Crate container:"
                          << query.lastError().text();
        return uids;
    }
    uids.reserve(count);
    while (query.next()) {
        uids.append(query.value(0).toString());
    }
    return uids;
}

namespace {

/// Sets `*pFound` to whether the container is still there.
bool recordSync(const QSqlDatabase& database,
        qint64 containerId,
        const SyncRecord& record,
        QSqlError* pError,
        bool* pFound = nullptr) {
    QSqlQuery update(database);
    update.prepare(QStringLiteral(
            "UPDATE crateserver_containers SET pages = :pages, synced_etag = :etag, "
            "dirty = 0, fetched_at = :fetched WHERE id = :id"));
    update.bindValue(QStringLiteral(":pages"), pagesToJson(record.pages));
    update.bindValue(QStringLiteral(":etag"), text(record.syncedEtag));
    update.bindValue(QStringLiteral(":fetched"), record.fetchedAt);
    update.bindValue(QStringLiteral(":id"), containerId);
    if (!exec(update, pError)) {
        return false;
    }
    if (pFound) {
        *pFound = update.numRowsAffected() > 0;
    }
    return true;
}

} // namespace

WriteStatus replaceContainerTracks(const QSqlDatabase& database,
        qint64 containerId,
        const QStringList& entityUids,
        const SyncRecord& record) {
    return inTransaction(database,
            "replace the tracks of a Crate container",
            [&](QSqlError* pError) {
                // A container the tree dropped while it was syncing gets no
                // tracks.
                bool found = false;
                if (!recordSync(
                            database, containerId, record, pError, &found)) {
                    return false;
                }
                if (!found) {
                    return true;
                }
                QSqlQuery remove(database);
                remove.prepare(QStringLiteral(
                        "DELETE FROM crateserver_container_tracks WHERE "
                        "container_id = :id"));
                remove.bindValue(QStringLiteral(":id"), containerId);
                if (!exec(remove, pError)) {
                    return false;
                }
                QSqlQuery insert(database);
                insert.prepare(QStringLiteral(
                        "INSERT INTO crateserver_container_tracks "
                        "(container_id, position, entity_uid) "
                        "VALUES (:id, :position, :uid)"));
                for (int i = 0; i < entityUids.size(); ++i) {
                    insert.bindValue(QStringLiteral(":id"), containerId);
                    insert.bindValue(QStringLiteral(":position"), i);
                    insert.bindValue(QStringLiteral(":uid"), entityUids[i]);
                    if (!exec(insert, pError)) {
                        return false;
                    }
                }
                return true;
            });
}

WriteStatus markContainerSynced(
        const QSqlDatabase& database, qint64 containerId, const SyncRecord& record) {
    return inTransaction(database, "record the sync of a Crate container", [&](QSqlError* pError) {
        return recordSync(database, containerId, record, pError);
    });
}

WriteStatus markDirty(const QSqlDatabase& database,
        const QString& serverId,
        const Invalidation& invalidation,
        QList<qint64>* pMarked) {
    QString where;
    switch (invalidation.scope) {
    case Invalidation::Scope::Tree:
        return WriteStatus::Ok;
    case Invalidation::Scope::Container:
        where = QStringLiteral("server_id = :server AND kind = :kind AND remote_id = :remote");
        break;
    case Invalidation::Scope::Kind:
        where = QStringLiteral("server_id = :server AND kind = :kind");
        break;
    case Invalidation::Scope::Everything:
        where = QStringLiteral("server_id = :server");
        break;
    }
    const auto bind = [&](QSqlQuery* pQuery) {
        pQuery->bindValue(QStringLiteral(":server"), serverId);
        if (where.contains(QLatin1String(":kind"))) {
            pQuery->bindValue(QStringLiteral(":kind"), invalidation.kind);
        }
        if (where.contains(QLatin1String(":remote"))) {
            pQuery->bindValue(QStringLiteral(":remote"), invalidation.remoteId);
        }
    };
    QList<qint64> marked;
    const WriteStatus status = inTransaction(database,
            "mark Crate containers dirty",
            [&](QSqlError* pError) {
                marked.clear();
                QSqlQuery select(database);
                select.prepare(QStringLiteral("SELECT id FROM crateserver_containers WHERE ") +
                        where + QStringLiteral(" ORDER BY position, id"));
                bind(&select);
                if (!exec(select, pError)) {
                    return false;
                }
                while (select.next()) {
                    marked.append(select.value(0).toLongLong());
                }
                select.finish();
                QSqlQuery update(database);
                update.prepare(
                        QStringLiteral("UPDATE crateserver_containers SET dirty = 1 WHERE ") +
                        where);
                bind(&update);
                return exec(update, pError);
            });
    if (status == WriteStatus::Ok && pMarked) {
        pMarked->append(marked);
    }
    return status;
}

} // namespace crate::library
