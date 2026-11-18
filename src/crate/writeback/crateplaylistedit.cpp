#include "crate/writeback/crateplaylistedit.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <cstdlib>

#include "crate/client/crateclient.h"
#include "util/logger.h"

namespace crate::writeback {

namespace {

const mixxx::Logger kLogger("crate::writeback::CratePlaylistEdit");

QString playlistPath(qint64 playlistId) {
    return QStringLiteral("/api/playlists/%1").arg(playlistId);
}

QStringList lowercase(const QStringList& uids) {
    QStringList lower;
    lower.reserve(uids.size());
    for (const QString& uid : uids) {
        lower.append(uid.toLower());
    }
    return lower;
}

/// The index of `row` in `uids`: where it was if its track is still there,
/// else the nearest place of its track that `used` does not hold; -1 if the
/// track is gone.
int find(const CratePlaylistEdit::Row& row, const QStringList& uids, QSet<int>* pUsed) {
    const QString uid = row.entityUid.toLower();
    int found = -1;
    if (row.index >= 0 && row.index < uids.size() && uids[row.index] == uid &&
            !pUsed->contains(row.index)) {
        found = row.index;
    } else {
        for (int i = 0; i < uids.size(); ++i) {
            if (uids[i] == uid && !pUsed->contains(i) &&
                    (found < 0 || std::abs(i - row.index) < std::abs(found - row.index))) {
                found = i;
            }
        }
    }
    if (found >= 0) {
        pUsed->insert(found);
    }
    return found;
}

} // namespace

struct CratePlaylistEdit::Edit {
    enum class Kind {
        Add,
        Remove,
        Move,
        Reorder,
        Export,
    };
    Kind kind = Kind::Add;
    QString serverId;
    qint64 playlistId = 0;
    QString name;
    QStringList entityUids;
    QList<Row> rows;
    std::optional<Row> before;
    int rejected = 0;
    /// 1 once the playlist was synced again after a difference.
    int attempt = 0;
    Done done;
};

CratePlaylistEdit::CrateTracks CratePlaylistEdit::crateTracksOf(const QString& serverId,
        const QStringList& locations,
        const std::function<std::optional<cache::TrackKey>(const QString& location)>&
                trackOfFile) {
    CrateTracks tracks;
    for (const QString& location : locations) {
        const std::optional<cache::TrackKey> track =
                trackOfFile ? trackOfFile(location) : std::nullopt;
        if (track && track->serverId == serverId && !track->entityUid.isEmpty()) {
            tracks.entityUids.append(track->entityUid);
        } else {
            ++tracks.rejected;
        }
    }
    return tracks;
}

CratePlaylistEdit::CratePlaylistEdit(Options options, QObject* pParent)
        : QObject(pParent),
          m_options(std::move(options)) {
}

CratePlaylistEdit::~CratePlaylistEdit() = default;

void CratePlaylistEdit::add(const QString& serverId,
        qint64 playlistId,
        const QStringList& entityUids,
        const Done& done) {
    auto pEdit = std::make_shared<Edit>();
    pEdit->kind = Edit::Kind::Add;
    pEdit->serverId = serverId;
    pEdit->playlistId = playlistId;
    pEdit->entityUids = entityUids;
    pEdit->done = done;
    if (entityUids.isEmpty()) {
        fail(pEdit, tr("Only Crate tracks of this server can go into its playlists."));
        return;
    }
    run(pEdit);
}

void CratePlaylistEdit::remove(
        const QString& serverId, qint64 playlistId, const QList<Row>& rows, const Done& done) {
    auto pEdit = std::make_shared<Edit>();
    pEdit->kind = Edit::Kind::Remove;
    pEdit->serverId = serverId;
    pEdit->playlistId = playlistId;
    pEdit->rows = rows;
    pEdit->done = done;
    run(pEdit);
}

void CratePlaylistEdit::move(const QString& serverId,
        qint64 playlistId,
        const QList<Row>& rows,
        const std::optional<Row>& before,
        const Done& done) {
    auto pEdit = std::make_shared<Edit>();
    pEdit->kind = Edit::Kind::Move;
    pEdit->serverId = serverId;
    pEdit->playlistId = playlistId;
    pEdit->rows = rows;
    pEdit->before = before;
    pEdit->done = done;
    run(pEdit);
}

void CratePlaylistEdit::reorder(const QString& serverId,
        qint64 playlistId,
        const QStringList& entityUids,
        const Done& done) {
    auto pEdit = std::make_shared<Edit>();
    pEdit->kind = Edit::Kind::Reorder;
    pEdit->serverId = serverId;
    pEdit->playlistId = playlistId;
    pEdit->entityUids = lowercase(entityUids);
    pEdit->done = done;
    run(pEdit);
}

void CratePlaylistEdit::exportTracks(const QString& serverId,
        const QString& name,
        const QStringList& entityUids,
        int rejected,
        const Done& done) {
    auto pEdit = std::make_shared<Edit>();
    pEdit->kind = Edit::Kind::Export;
    pEdit->serverId = serverId;
    pEdit->name = name;
    pEdit->entityUids = entityUids;
    pEdit->rejected = rejected;
    pEdit->done = done;
    if (entityUids.isEmpty()) {
        fail(pEdit, tr("The playlist has no Crate tracks of this server."));
        return;
    }
    Client* pClient = m_options.clientFor ? m_options.clientFor(serverId) : nullptr;
    if (!pClient) {
        fail(pEdit, tr("Sign in to the Crate server to export the playlist."));
        return;
    }
    Reply* pReply = pClient->post(QStringLiteral("/api/playlists"),
            QJsonDocument(QJsonObject{{QStringLiteral("name"), name}}));
    connect(pReply, &Reply::finished, this, [this, pEdit](const JsonResult& result) {
        if (!result.ok()) {
            fail(pEdit,
                    tr("Crate did not create the playlist: %1").arg(result.error().message));
            return;
        }
        pEdit->playlistId = result.value().object().value(QStringLiteral("id")).toInteger();
        if (pEdit->playlistId <= 0) {
            fail(pEdit, tr("Crate did not create the playlist."));
            return;
        }
        // A new playlist: nothing to compare with
        write(pEdit, {});
    });
}

void CratePlaylistEdit::run(const std::shared_ptr<Edit>& pEdit) {
    if (!m_options.readStored) {
        compare(pEdit, QStringList());
        return;
    }
    m_options.readStored(pEdit->serverId,
            pEdit->playlistId,
            [this, pEdit](const std::optional<QStringList>& stored) {
                // A playlist Decks never synced differs from the server's
                compare(pEdit,
                        stored ? lowercase(*stored) : QStringList{QStringLiteral("\n")});
            });
}

void CratePlaylistEdit::compare(const std::shared_ptr<Edit>& pEdit, const QStringList& stored) {
    Client* pClient = m_options.clientFor ? m_options.clientFor(pEdit->serverId) : nullptr;
    if (!pClient) {
        fail(pEdit, tr("Sign in to the Crate server to edit its playlists."));
        return;
    }
    Reply* pReply = pClient->get(playlistPath(pEdit->playlistId));
    connect(pReply, &Reply::finished, this, [this, pEdit, stored](const JsonResult& result) {
        if (!result.ok()) {
            fail(pEdit, tr("Cannot read the playlist from Crate: %1").arg(result.error().message));
            return;
        }
        const QJsonObject playlist = result.value().object();
        if (!playlist.value(QStringLiteral("can_edit")).toBool()) {
            fail(pEdit, tr("You cannot edit this playlist on Crate."));
            return;
        }
        QList<ServerRow> rows;
        const QJsonArray tracks = playlist.value(QStringLiteral("tracks")).toArray();
        for (const QJsonValue& value : tracks) {
            const QJsonObject track = value.toObject();
            ServerRow row;
            row.id = track.value(QStringLiteral("id")).toInteger();
            row.position = track.value(QStringLiteral("position")).toInt();
            row.entityUid = track.value(QStringLiteral("track_entity_uid")).toString().toLower();
            rows.append(row);
        }
        std::stable_sort(rows.begin(),
                rows.end(),
                [](const ServerRow& left, const ServerRow& right) {
                    return left.position < right.position;
                });
        // Decks lists the rows with a local track only
        QStringList onServer;
        for (const ServerRow& row : std::as_const(rows)) {
            if (!row.entityUid.isEmpty()) {
                onServer.append(row.entityUid);
            }
        }
        if (onServer == stored) {
            write(pEdit, rows);
            return;
        }
        if (pEdit->attempt > 0 || !m_options.resync) {
            fail(pEdit,
                    tr("The playlist changed on Crate while you edited it. "
                       "Check it and try again."),
                    true);
            return;
        }
        // Changed on the server: sync it and try once more
        kLogger.info() << "The Crate playlist" << pEdit->playlistId
                       << "changed on the server; syncing it again";
        pEdit->attempt = 1;
        m_options.resync(pEdit->serverId, pEdit->playlistId, [this, pEdit](bool ok) {
            if (!ok) {
                fail(pEdit, tr("Cannot sync the playlist from Crate."));
                return;
            }
            run(pEdit);
        });
    });
}

void CratePlaylistEdit::write(const std::shared_ptr<Edit>& pEdit, const QList<ServerRow>& rows) {
    Client* pClient = m_options.clientFor ? m_options.clientFor(pEdit->serverId) : nullptr;
    if (!pClient) {
        fail(pEdit, tr("Sign in to the Crate server to edit its playlists."));
        return;
    }
    // The rows Decks lists, by their index in its copy
    QList<int> entityRows;
    QStringList uids;
    for (int i = 0; i < rows.size(); ++i) {
        if (!rows[i].entityUid.isEmpty()) {
            entityRows.append(i);
            uids.append(rows[i].entityUid);
        }
    }
    const auto onWritten = [this, pEdit](const JsonResult& result) {
        if (!result.ok()) {
            fail(pEdit,
                    tr("Crate did not take the change to the playlist: %1")
                            .arg(result.error().message));
            return;
        }
        succeed(pEdit);
    };

    switch (pEdit->kind) {
    case Edit::Kind::Add:
    case Edit::Kind::Export: {
        QJsonArray tracks;
        for (const QString& uid : std::as_const(pEdit->entityUids)) {
            tracks.append(QJsonObject{{QStringLiteral("entity_uid"), uid}});
        }
        connect(pClient->post(playlistPath(pEdit->playlistId) + QStringLiteral("/tracks"),
                        QJsonDocument(QJsonObject{{QStringLiteral("tracks"), tracks}})),
                &Reply::finished,
                this,
                onWritten);
        return;
    }
    case Edit::Kind::Remove: {
        QSet<int> used;
        QList<int> positions;
        for (const Row& row : std::as_const(pEdit->rows)) {
            const int index = find(row, uids, &used);
            if (index >= 0) {
                positions.append(rows[entityRows[index]].position);
            }
        }
        // The last first, so that the others keep their positions
        std::sort(positions.begin(), positions.end(), std::greater<>());
        removeNext(pEdit, positions);
        return;
    }
    case Edit::Kind::Move: {
        QSet<int> used;
        QList<Row> moving = pEdit->rows;
        std::sort(moving.begin(), moving.end(), [](const Row& left, const Row& right) {
            return left.index < right.index;
        });
        QList<int> moved;
        for (const Row& row : std::as_const(moving)) {
            const int index = find(row, uids, &used);
            if (index >= 0) {
                moved.append(index);
            }
        }
        int before = -1;
        if (pEdit->before) {
            QSet<int> anchorUsed;
            before = find(*pEdit->before, uids, &anchorUsed);
            // Dropped onto a moved row: before the next row that stays
            while (before >= 0 && moved.contains(before)) {
                before = before + 1 < uids.size() ? before + 1 : -1;
            }
        }
        QList<int> order;
        for (int i = 0; i < uids.size(); ++i) {
            if (i == before) {
                order += moved;
            }
            if (!moved.contains(i)) {
                order.append(i);
            }
        }
        if (before < 0) {
            order += moved;
        }
        postOrder(pEdit, pClient, rows, entityRows, order);
        return;
    }
    case Edit::Kind::Reorder: {
        // The tracks of the new order take, in that order, the places they
        // hold now; a track Decks did not have keeps its own
        QSet<int> used;
        QList<int> matched;
        for (int i = 0; i < pEdit->entityUids.size(); ++i) {
            const int index = find(Row{i, pEdit->entityUids[i]}, uids, &used);
            if (index >= 0) {
                matched.append(index);
            }
        }
        QList<int> places = matched;
        std::sort(places.begin(), places.end());
        QList<int> order;
        order.reserve(uids.size());
        for (int i = 0; i < uids.size(); ++i) {
            order.append(i);
        }
        for (int i = 0; i < places.size(); ++i) {
            order[places[i]] = matched[i];
        }
        postOrder(pEdit, pClient, rows, entityRows, order);
        return;
    }
    }
}

void CratePlaylistEdit::postOrder(const std::shared_ptr<Edit>& pEdit,
        Client* pClient,
        const QList<ServerRow>& rows,
        const QList<int>& entityRows,
        const QList<int>& order) {
    // Every row id: the ones Decks lists in their new order, the others
    // where they were
    QList<qint64> ids;
    ids.reserve(rows.size());
    for (const ServerRow& row : rows) {
        ids.append(row.id);
    }
    for (int i = 0; i < order.size(); ++i) {
        ids[entityRows[i]] = rows[entityRows[order[i]]].id;
    }
    QJsonArray trackIds;
    for (const qint64 id : std::as_const(ids)) {
        trackIds.append(id);
    }
    connect(pClient->post(playlistPath(pEdit->playlistId) + QStringLiteral("/reorder"),
                    QJsonDocument(QJsonObject{{QStringLiteral("track_ids"), trackIds}})),
            &Reply::finished,
            this,
            [this, pEdit](const JsonResult& result) {
                if (!result.ok()) {
                    fail(pEdit,
                            tr("Crate did not take the change to the playlist: %1")
                                    .arg(result.error().message));
                    return;
                }
                succeed(pEdit);
            });
}

void CratePlaylistEdit::removeNext(const std::shared_ptr<Edit>& pEdit, QList<int> positions) {
    if (positions.isEmpty()) {
        succeed(pEdit);
        return;
    }
    Client* pClient = m_options.clientFor ? m_options.clientFor(pEdit->serverId) : nullptr;
    if (!pClient) {
        fail(pEdit, tr("Sign in to the Crate server to edit its playlists."));
        return;
    }
    const int position = positions.takeFirst();
    Reply* pReply = pClient->del(
            playlistPath(pEdit->playlistId) + QStringLiteral("/tracks/%1").arg(position));
    connect(pReply, &Reply::finished, this, [this, pEdit, positions](const JsonResult& result) {
        if (!result.ok()) {
            fail(pEdit,
                    tr("Crate did not take the change to the playlist: %1")
                            .arg(result.error().message));
            return;
        }
        removeNext(pEdit, positions);
    });
}

void CratePlaylistEdit::succeed(const std::shared_ptr<Edit>& pEdit) {
    if (m_options.resync) {
        // A new playlist shows in the tree; an edited one shows its tracks
        m_options.resync(pEdit->serverId,
                pEdit->kind == Edit::Kind::Export ? 0 : pEdit->playlistId,
                [](bool) {});
    }
    Outcome outcome;
    outcome.ok = true;
    outcome.playlistId = pEdit->playlistId;
    outcome.rejected = pEdit->rejected;
    if (pEdit->done) {
        pEdit->done(outcome);
    }
}

void CratePlaylistEdit::fail(
        const std::shared_ptr<Edit>& pEdit, const QString& message, bool conflict) {
    kLogger.warning() << "Playlist edit failed:" << message;
    // A failed edit leaves Decks' copy as the server has it
    if (m_options.resync && pEdit->playlistId > 0 && conflict) {
        m_options.resync(pEdit->serverId, pEdit->playlistId, [](bool) {});
    }
    Outcome outcome;
    outcome.conflict = conflict;
    outcome.playlistId = pEdit->playlistId;
    outcome.rejected = pEdit->rejected;
    outcome.message = message;
    emit warning(message);
    if (pEdit->done) {
        pEdit->done(outcome);
    }
}

} // namespace crate::writeback

#include "moc_crateplaylistedit.cpp"
