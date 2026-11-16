#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <optional>

#include "crate/client/crateresult.h"
#include "crate/library/cratelibraryjson.h"

/// Every field of the recommendation endpoints that Decks reads (plan F5) is
/// parsed here and nowhere else: Compatibles, Radio and Music Paths.
namespace crate::recs {

/// The `context` of the compatible tracks: how the DJ means to mix them.
enum class CompatibleContext {
    Auto,
    Beatmatch,
    Smooth,
};

/// "auto", "beatmatch" or "smooth".
QString contextName(CompatibleContext context);

/// One item of `GET /api/tracks/by-entity/{uid}/compatible`.
struct CompatibleTrack {
    /// What the item carries of the track: entity uid, title, artist, album,
    /// duration, BPM, Camelot key, energy, genre and cover. No format,
    /// bitrate, profile or `playable`.
    library::RemoteTrack track;
    qint64 trackId = 0;
    /// 0 to 1.
    double score = 0;
    double confidence = 0;
    /// `scoreBreakdown.harmonicRelationship`: same, adjacent, relative,
    /// incompatible or unknown.
    QString harmonicRelationship;
};

/// The answer of `GET /api/tracks/by-entity/{uid}/compatible`.
struct CompatibleTracks {
    QString seedEntityUid;
    QString context;
    /// Best first, as the server ranks them.
    QList<CompatibleTrack> items;
};

/// Parses `{"seedTrackEntityUid": ..., "items": [...]}` (camelCase; the
/// server leaves out the fields it has no value for). An item without
/// `trackEntityUid` or `score`, or a field of the wrong type, is a Decode
/// error.
Result<CompatibleTracks> parseCompatibleTracks(const QByteArray& json);

/// `/api/tracks/by-entity/<uid>/compatible`, the uid percent-encoded.
QString compatiblePath(const QString& entityUid);

/// A track of Radio or of a Music Path: a library track with what queueing
/// it needs.
struct RecommendedTrack {
    QString entityUid;
    /// The numeric id: `radio/feedback` takes it.
    qint64 trackId = 0;
    QString title;
    QString artist;
    QString album;
    std::optional<qint64> durationMs;
    /// For the file name of its download.
    QString format;
};

/// A batch of `POST /api/radio/start` or `POST /api/radio/next`.
struct RadioBatch {
    QString sessionId;
    QString seedLabel;
    std::optional<int> batchIndex;
    /// ok, retryable_empty, exhausted or auth_required.
    QString status;
    bool exhausted = false;
    /// Tracks without an entity uid (catalogue-only ones) are left out.
    QList<RecommendedTrack> tracks;
};

/// Parses a radio batch. The shaped radio sends the uid as `entity_uid` and
/// `track_entity_uid` as null; both are read.
Result<RadioBatch> parseRadioBatch(const QByteArray& json);

/// The answer of `POST /api/paths/preview` (and `POST /api/paths`).
struct MusicPath {
    QString name;
    bool partial = false;
    /// From the origin to the destination, both included.
    QList<RecommendedTrack> tracks;
};

/// Parses a path: its tracks in step order. A track without an entity uid
/// is left out.
Result<MusicPath> parseMusicPath(const QByteArray& json);

} // namespace crate::recs
