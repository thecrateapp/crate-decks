#include "crate/library/cratelibraryjson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>

#include "crate/client/cratejson.h"

namespace crate::library {

namespace {

using json::ObjectReader;

Result<QJsonObject> parseObject(const QByteArray& json) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return Error::decode(
                QStringLiteral("The answer is not JSON: %1").arg(parseError.errorString()));
    }
    return json::requireObject(document);
}

QString itemPath(const char* array, qsizetype index) {
    return QStringLiteral("%1[%2]").arg(QLatin1String(array)).arg(index);
}

} // namespace

std::optional<Section> sectionOf(const QString& kind) {
    // As the server spells them: the kind goes back verbatim in the tracks
    // path, which takes only these.
    if (kind == QLatin1String("playlist")) {
        return Section::Playlists;
    }
    if (kind == QLatin1String("crate")) {
        return Section::Crates;
    }
    if (kind == QLatin1String("genre")) {
        return Section::Genres;
    }
    if (kind == QLatin1String("mood")) {
        return Section::Moods;
    }
    if (kind == QLatin1String("recent")) {
        return Section::Recent;
    }
    if (kind == QLatin1String("likes")) {
        return Section::Likes;
    }
    return std::nullopt;
}

Result<ContainerTree> parseContainerTree(const QByteArray& json) {
    const Result<QJsonObject> root = parseObject(json);
    if (!root.ok()) {
        return root.error();
    }
    ObjectReader rootReader(root.value());
    const QJsonArray nodes = rootReader.requiredArray(u"containers");
    const std::optional<QJsonObject> library = rootReader.optionalObject(u"library");
    if (!rootReader.ok()) {
        return rootReader.error();
    }
    ContainerTree tree;
    if (library) {
        ObjectReader libraryReader(*library, QStringLiteral("library"));
        tree.libraryTrackCount = libraryReader.optionalInteger(u"track_count");
        if (!libraryReader.ok()) {
            return libraryReader.error();
        }
        if (tree.libraryTrackCount.value_or(0) < 0) {
            return Error::decode(QStringLiteral("library.track_count is negative"));
        }
    }
    QList<RemoteContainer>& containers = tree.containers;
    containers.reserve(nodes.size());
    for (qsizetype i = 0; i < nodes.size(); ++i) {
        const QString path = itemPath("containers", i);
        if (!nodes[i].isObject()) {
            return Error::decode(path + QStringLiteral(" is not an object"));
        }
        ObjectReader reader(nodes[i].toObject(), path);
        RemoteContainer container;
        container.remoteId = reader.requiredId(u"id");
        container.kind = reader.requiredString(u"kind");
        container.name = reader.optionalString(u"name");
        container.parentRemoteId = reader.optionalId(u"parent_id");
        container.trackCount = reader.optionalInteger(u"track_count");
        container.etag = reader.optionalString(u"etag");
        if (!reader.ok()) {
            return reader.error();
        }
        if (container.remoteId.isEmpty()) {
            return Error::decode(path + QStringLiteral(".id is empty"));
        }
        if (sectionOf(container.kind)) {
            containers.append(container);
        }
    }
    return tree;
}

Result<TrackPage> parseTrackPage(const QByteArray& json) {
    const Result<QJsonObject> root = parseObject(json);
    if (!root.ok()) {
        return root.error();
    }
    ObjectReader rootReader(root.value());
    const QJsonArray items = rootReader.requiredArray(u"tracks");
    TrackPage page;
    page.nextCursor = rootReader.optionalString(u"next_cursor");
    if (!rootReader.ok()) {
        return rootReader.error();
    }
    page.tracks.reserve(items.size());
    for (qsizetype i = 0; i < items.size(); ++i) {
        const QString path = itemPath("tracks", i);
        if (!items[i].isObject()) {
            return Error::decode(path + QStringLiteral(" is not an object"));
        }
        ObjectReader reader(items[i].toObject(), path);
        RemoteTrack track;
        track.entityUid = reader.requiredString(u"entity_uid");
        track.globalTrackUid = reader.optionalString(u"global_track_uid");
        track.title = reader.optionalString(u"title");
        track.artist = reader.optionalString(u"artist");
        track.album = reader.optionalString(u"album");
        track.durationMs = reader.optionalInteger(u"duration_ms");
        track.bpm = reader.optionalDouble(u"bpm");
        track.camelot = reader.optionalString(u"camelot");
        track.energy = reader.optionalDouble(u"energy");
        track.genre = reader.optionalString(u"genre");
        track.format = reader.optionalString(u"format");
        track.bitrate = reader.optionalInteger(u"bitrate");
        track.sampleRate = reader.optionalInteger(u"sample_rate");
        track.bitDepth = reader.optionalInteger(u"bit_depth");
        track.byteLength = reader.optionalInteger(u"byte_length");
        track.sourceRevision = reader.optionalId(u"source_revision");
        track.profileRevision = reader.optionalId(u"profile_revision");
        track.profileQuality = reader.optionalString(u"profile_quality");
        const QJsonValue playable = items[i].toObject().value(QLatin1String("playable"));
        if (!playable.isUndefined() && !playable.isNull()) {
            track.playable = reader.requiredBool(u"playable");
        }
        track.coverUrl = reader.optionalString(u"cover_url");
        if (!reader.ok()) {
            return reader.error();
        }
        if (track.entityUid.isEmpty()) {
            return Error::decode(path + QStringLiteral(".entity_uid is empty"));
        }
        page.tracks.append(track);
    }
    return page;
}

QString containerTracksPath(const QString& kind, const QString& remoteId) {
    return QStringLiteral("/api/decks/containers/%1/%2/tracks")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(kind)),
                    QString::fromLatin1(QUrl::toPercentEncoding(remoteId)));
}

QString containerTreePath() {
    return QStringLiteral("/api/decks/containers");
}

} // namespace crate::library
