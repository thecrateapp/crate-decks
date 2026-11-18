#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

#include "crate/cache/audiocache.h"

namespace crate {

class Client;

namespace writeback {

/// Edits the playlists of a Crate server from Decks (plan F7.C-PR3): adds
/// Crate tracks, removes and reorders them, and exports a list of Crate
/// tracks as a new playlist.
///
/// The server has no ETag for a playlist, so before writing, each edit reads
/// the playlist (`GET /api/playlists/{id}`) and compares its tracks with
/// Decks' copy, the playlist's container as the library sync stored it. If
/// they differ, the playlist is synced again and the edit tried once more
/// against the new copy, its rows found again by their tracks; if it still
/// differs, or the server refuses the edit, warning() tells the user and
/// nothing more is written. After a write the playlist is synced again.
///
/// - Add: `POST /api/playlists/{id}/tracks` with the tracks' entity uids, at
///   the end.
/// - Remove: `DELETE /api/playlists/{id}/tracks/{position}`, one request per
///   track, from the last position to the first so that the others keep
///   theirs.
/// - Move and reorder: `POST /api/playlists/{id}/reorder` with every row id
///   of the playlist (`tracks[].id`) in the new order, as the server only
///   takes the whole list. Rows Decks does not list (a
///   track without a local file on the server) keep their places.
/// - Export: `POST /api/playlists` with the name, then add.
///
/// Only Crate tracks of the playlist's server go in: crateTracksOf() leaves
/// out the rest and counts them.
class CratePlaylistEdit : public QObject {
    Q_OBJECT
  public:
    using ReadStored = std::function<void(const QString& serverId,
            qint64 playlistId,
            const std::function<void(std::optional<QStringList> entityUids)>& done)>;
    using Resync = std::function<void(
            const QString& serverId, qint64 playlistId, const std::function<void(bool ok)>& done)>;

    struct Options {
        /// The Client of a server; nullptr while it has none.
        std::function<Client*(const QString& serverId)> clientFor;
        /// The entity uids of the playlist as Decks stored it, in order;
        /// nullopt if Decks has not synced it.
        ReadStored readStored;
        /// Syncs the playlist again from the server, or the server's tree
        /// when `playlistId` is 0, then calls `done`.
        Resync resync;
    };

    /// A row of Decks' copy of a playlist: its index, from 0, and its track.
    struct Row {
        int index = 0;
        QString entityUid;
    };

    struct Outcome {
        bool ok = false;
        /// The playlist kept changing on the server.
        bool conflict = false;
        /// Tracks left out because they are not the server's Crate tracks.
        int rejected = 0;
        /// The playlist edited or created.
        qint64 playlistId = 0;
        /// Why it failed, for the user.
        QString message;
    };
    using Done = std::function<void(const Outcome& outcome)>;

    /// The tracks of `serverId` among the files at `locations`.
    struct CrateTracks {
        QStringList entityUids;
        int rejected = 0;
    };
    static CrateTracks crateTracksOf(const QString& serverId,
            const QStringList& locations,
            const std::function<std::optional<cache::TrackKey>(const QString& location)>&
                    trackOfFile);

    explicit CratePlaylistEdit(Options options, QObject* pParent = nullptr);
    ~CratePlaylistEdit() override;

    void add(const QString& serverId,
            qint64 playlistId,
            const QStringList& entityUids,
            const Done& done = nullptr);
    void remove(const QString& serverId,
            qint64 playlistId,
            const QList<Row>& rows,
            const Done& done = nullptr);
    /// Moves `rows` before `before`, or to the end without it, keeping
    /// their order.
    void move(const QString& serverId,
            qint64 playlistId,
            const QList<Row>& rows,
            const std::optional<Row>& before,
            const Done& done = nullptr);
    /// Puts the playlist in the order `entityUids`, every track of Decks' copy
    /// in its new place (a drag in the track table). If the playlist changed
    /// meanwhile, the tracks of `entityUids` it still has take that order
    /// among the places they hold now, and the others stay where they are.
    void reorder(const QString& serverId,
            qint64 playlistId,
            const QStringList& entityUids,
            const Done& done = nullptr);
    /// Creates the playlist `name` with `entityUids`. `rejected` is how many
    /// tracks crateTracksOf() left out, for the Outcome.
    void exportTracks(const QString& serverId,
            const QString& name,
            const QStringList& entityUids,
            int rejected,
            const Done& done = nullptr);

  signals:
    /// An edit failed: a message for the user.
    void warning(const QString& message);

  private:
    struct Edit;
    /// A row of the playlist on the server.
    struct ServerRow {
        qint64 id = 0;
        int position = 0;
        /// Lowercase; empty for a track without a local file.
        QString entityUid;
    };

    void run(const std::shared_ptr<Edit>& pEdit);
    void compare(const std::shared_ptr<Edit>& pEdit, const QStringList& stored);
    void write(const std::shared_ptr<Edit>& pEdit, const QList<ServerRow>& rows);
    void removeNext(const std::shared_ptr<Edit>& pEdit, QList<int> positions);
    /// Sends the new order: `order[i]` is the index, among the rows Decks
    /// lists (`entityRows`), of the one that goes to place `i`.
    void postOrder(const std::shared_ptr<Edit>& pEdit,
            Client* pClient,
            const QList<ServerRow>& rows,
            const QList<int>& entityRows,
            const QList<int>& order);
    void succeed(const std::shared_ptr<Edit>& pEdit);
    void fail(const std::shared_ptr<Edit>& pEdit, const QString& message, bool conflict = false);

    Options m_options;
};

} // namespace writeback
} // namespace crate
