#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <optional>

#include "crate/library/cratelibraryjson.h"

/// Reads and writes the `crateserver_` library tables (design §6). The writes
/// run off the main thread (plan "Antes de F3"); each one is a single
/// transaction.
namespace crate::library {

/// One page of the last sync of a container. Its ETag changes with any field
/// of the page, so it is sent back as If-None-Match when the page is asked
/// again with the same cursor.
struct PageState {
    /// The cursor the page was asked with; empty for the first page.
    QString cursor;
    QString etag;
    int trackCount = 0;

    bool operator==(const PageState& other) const = default;
};

/// A row of `crateserver_containers`.
struct StoredContainer {
    qint64 id = 0;
    QString serverId;
    QString kind;
    QString remoteId;
    QString name;
    /// 0 for a top-level container.
    qint64 parentId = 0;
    /// Order in the server's tree.
    int position = 0;
    /// The server's count, when it has one (RemoteContainer::trackCount).
    std::optional<qint64> trackCount;
    /// The tree's ETag of the container.
    QString etag;
    /// The tree ETag that the stored tracks match.
    QString syncedEtag;
    QList<PageState> pages;
    /// Set by the server's invalidation events.
    bool dirty = false;
    /// When the tracks were last synced; empty if never.
    QString fetchedAt;

    bool needsSync() const {
        return dirty || fetchedAt.isEmpty() || etag.isEmpty() || etag != syncedEtag;
    }
    /// The number of stored tracks, once synced.
    std::optional<qint64> syncedTrackCount() const;
};

enum class WriteStatus {
    Ok,
    /// The database is busy or locked: try again later.
    Busy,
    Failed,
};

/// What a server's invalidation event affects.
struct Invalidation {
    enum class Scope {
        /// The tree: containers added, renamed or removed.
        Tree,
        /// One container, by kind and remote id.
        Container,
        /// Every container of a kind.
        Kind,
        /// Everything: the tree and every container.
        Everything,
    };
    Scope scope = Scope::Everything;
    QString kind;
    QString remoteId;
};

/// The containers of a server, in tree order.
QList<StoredContainer> loadContainers(const QSqlDatabase& database, const QString& serverId);
std::optional<StoredContainer> loadContainer(const QSqlDatabase& database, qint64 containerId);

/// Replaces the tree of a server with `tree`: existing containers keep their
/// id, synced tracks and ETags; the ones the server no longer lists go, with
/// their tracks, and so do the catalogue tracks no container lists any more
/// (unless Mixxx's library has them). The library's track count is replaced
/// too, also by none when the tree has none.
WriteStatus storeContainerTree(
        const QSqlDatabase& database, const QString& serverId, const ContainerTree& tree);
/// The same for a tree without a library count.
WriteStatus storeContainerTree(const QSqlDatabase& database,
        const QString& serverId,
        const QList<RemoteContainer>& containers);

/// The library's track count the server's tree gave last, if any.
std::optional<qint64> loadLibraryTrackCount(
        const QSqlDatabase& database, const QString& serverId);

/// Adds the tracks to the catalogue or updates them. Never touches
/// `mixxx_track_id`.
WriteStatus storeTracks(const QSqlDatabase& database,
        const QString& serverId,
        const QList<RemoteTrack>& tracks,
        const QString& now);

/// The entity uids at positions [first, first + count) of a container.
QStringList loadContainerTrackUids(
        const QSqlDatabase& database, qint64 containerId, int first, int count);

/// What a finished sync records on its container.
struct SyncRecord {
    QList<PageState> pages;
    QString syncedEtag;
    QString fetchedAt;
};

/// Replaces the tracks of a container with `entityUids`, in order, and
/// records the sync, in one transaction.
WriteStatus replaceContainerTracks(const QSqlDatabase& database,
        qint64 containerId,
        const QStringList& entityUids,
        const SyncRecord& record);

/// Records a sync that found nothing new: the stored tracks stay.
WriteStatus markContainerSynced(
        const QSqlDatabase& database, qint64 containerId, const SyncRecord& record);

/// Marks the containers `invalidation` affects as dirty, and appends their ids
/// to `pMarked`. A Tree invalidation marks none: the tree's ETags tell which
/// containers changed.
WriteStatus markDirty(const QSqlDatabase& database,
        const QString& serverId,
        const Invalidation& invalidation,
        QList<qint64>* pMarked);

} // namespace crate::library
