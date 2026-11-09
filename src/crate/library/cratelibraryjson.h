#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <optional>

#include "crate/client/crateresult.h"

/// Every field of the S3 library contract that Decks reads is parsed here and
/// nowhere else (server `docs/technical/crate-decks.md`, "Library navigation
/// (S3)").
namespace crate::library {

/// The sidebar section a container kind belongs to.
enum class Section {
    Playlists,
    Crates,
    Genres,
    Moods,
    Recent,
    Likes,
};

/// The section of a container `kind`, or nullopt for a kind this client does
/// not know: the contract says clients ignore those, since new kinds are
/// additive. The server serves `playlist`, `crate`, `genre`, `mood`, `recent`
/// and `likes`.
std::optional<Section> sectionOf(const QString& kind);

/// A node of `GET /api/decks/containers`.
struct RemoteContainer {
    /// As the server spells it: it goes back verbatim in the tracks path.
    QString kind;
    /// Unique within its kind. Sent as a string; a JSON integer is accepted.
    QString remoteId;
    QString name;
    /// Empty for a top-level node. The server has no folders yet, so it is
    /// always empty today.
    QString parentRemoteId;
    /// Only when the server keeps it precomputed (playlists and genres). It
    /// counts every entry, also those without a local track that the tracks
    /// endpoint leaves out.
    std::optional<qint64> trackCount;
    /// Changes with the container's membership or order, not with its
    /// tracks' metadata or profiles. A mood's never changes.
    QString etag;
};

/// The answer of `GET /api/decks/containers`.
struct ContainerTree {
    /// In the server's order, without the nodes of unknown kinds.
    QList<RemoteContainer> containers;
    /// `library.track_count`: the tracks of the whole library, also those
    /// without an entity uid that no container lists. Not a container. Absent
    /// from servers older than it.
    std::optional<qint64> libraryTrackCount;
};

/// Parses `{"library": {"track_count": N}, "containers": [...]}`. A node
/// without `id` or `kind`, a field of the wrong type or a negative count is
/// a Decode error.
Result<ContainerTree> parseContainerTree(const QByteArray& json);

/// A track of a container page, with the DJ fields.
struct RemoteTrack {
    /// The library track: the key for the stream and mix-profile endpoints.
    QString entityUid;
    QString globalTrackUid;
    QString title;
    QString artist;
    QString album;
    std::optional<qint64> durationMs;
    std::optional<double> bpm;
    QString camelot;
    /// Global energy of the Smart Mix profile, 0 to 1.
    std::optional<double> energy;
    QString genre;
    QString format;
    /// kbit/s.
    std::optional<qint64> bitrate;
    std::optional<qint64> sampleRate;
    std::optional<qint64> bitDepth;
    std::optional<qint64> byteLength;
    /// Revision of the audio file; empty when it is not playable.
    QString sourceRevision;
    /// Empty when the track has no current Smart Mix profile.
    QString profileRevision;
    /// `full`, `partial`, `legacy` or `unavailable` (the analysis failed);
    /// empty when there is no current profile.
    QString profileQuality;
    std::optional<bool> playable;
    /// Relative to the server, e.g. `/api/vdj/albums/412/cover?size=512`.
    QString coverUrl;
};

/// A page of `GET /api/decks/containers/{kind}/{id}/tracks`.
struct TrackPage {
    QList<RemoteTrack> tracks;
    /// Empty on the last page.
    QString nextCursor;
};

/// Parses a page: `{"container": {...}, "tracks": [...], "next_cursor": ...}`.
/// A track without `entity_uid`, or a field of the wrong type, is a Decode
/// error.
Result<TrackPage> parseTrackPage(const QByteArray& json);

/// `/api/decks/containers/<kind>/<remoteId>/tracks`, each part
/// percent-encoded.
QString containerTracksPath(const QString& kind, const QString& remoteId);

/// The path of the container tree.
QString containerTreePath();

} // namespace crate::library
