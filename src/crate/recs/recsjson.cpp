#include "crate/recs/recsjson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>
#include <algorithm>
#include <cmath>

#include "crate/client/cratejson.h"

namespace crate::recs {

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

std::optional<qint64> millisecondsOf(std::optional<double> seconds) {
    if (!seconds) {
        return std::nullopt;
    }
    return std::llround(*seconds * 1000.0);
}

/// A track of Radio or of a path (snake_case, as the library rows).
RecommendedTrack recommendedTrackOf(ObjectReader* pReader) {
    RecommendedTrack track;
    // The shaped radio and paths send `entity_uid`; the enriched answers
    // `track_entity_uid`
    track.entityUid = pReader->optionalId(u"entity_uid");
    const QString enriched = pReader->optionalId(u"track_entity_uid");
    if (track.entityUid.isEmpty()) {
        track.entityUid = enriched;
    }
    track.trackId = pReader->optionalInteger(u"track_id").value_or(0);
    track.title = pReader->optionalString(u"title");
    track.artist = pReader->optionalString(u"artist");
    track.album = pReader->optionalString(u"album");
    track.durationMs = millisecondsOf(pReader->optionalDouble(u"duration"));
    track.format = pReader->optionalString(u"format");
    return track;
}

} // namespace

QString contextName(CompatibleContext context) {
    switch (context) {
    case CompatibleContext::Beatmatch:
        return QStringLiteral("beatmatch");
    case CompatibleContext::Smooth:
        return QStringLiteral("smooth");
    case CompatibleContext::Auto:
        break;
    }
    return QStringLiteral("auto");
}

Result<CompatibleTracks> parseCompatibleTracks(const QByteArray& json) {
    const Result<QJsonObject> root = parseObject(json);
    if (!root.ok()) {
        return root.error();
    }
    ObjectReader rootReader(root.value());
    CompatibleTracks compatibles;
    compatibles.seedEntityUid = rootReader.optionalId(u"seedTrackEntityUid");
    compatibles.context = rootReader.optionalString(u"context");
    const QJsonArray items = rootReader.requiredArray(u"items");
    if (!rootReader.ok()) {
        return rootReader.error();
    }
    compatibles.items.reserve(items.size());
    for (qsizetype i = 0; i < items.size(); ++i) {
        const QString path = itemPath("items", i);
        if (!items[i].isObject()) {
            return Error::decode(path + QStringLiteral(" is not an object"));
        }
        ObjectReader reader(items[i].toObject(), path);
        CompatibleTrack item;
        item.track.entityUid = reader.requiredId(u"trackEntityUid");
        item.trackId = reader.optionalInteger(u"trackId").value_or(0);
        item.track.title = reader.optionalString(u"title");
        item.track.artist = reader.optionalString(u"artist");
        item.track.album = reader.optionalString(u"album");
        item.track.durationMs = millisecondsOf(reader.optionalDouble(u"duration"));
        item.track.bpm = reader.optionalDouble(u"bpm");
        item.track.camelot = reader.optionalString(u"camelot");
        item.track.energy = reader.optionalDouble(u"energy");
        item.track.genre = reader.optionalString(u"genre");
        item.track.coverUrl = reader.optionalString(u"coverUrl");
        item.score = reader.requiredDouble(u"score");
        item.confidence = reader.optionalDouble(u"confidence").value_or(0);
        if (const std::optional<QJsonObject> breakdown = reader.optionalObject(u"scoreBreakdown")) {
            ObjectReader breakdownReader(*breakdown, path + QStringLiteral(".scoreBreakdown"));
            item.harmonicRelationship = breakdownReader.optionalString(u"harmonicRelationship");
            if (!breakdownReader.ok()) {
                return breakdownReader.error();
            }
        }
        if (!reader.ok()) {
            return reader.error();
        }
        compatibles.items.append(item);
    }
    return compatibles;
}

QString compatiblePath(const QString& entityUid) {
    return QStringLiteral("/api/tracks/by-entity/%1/compatible")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(entityUid)));
}

Result<RadioBatch> parseRadioBatch(const QByteArray& json) {
    const Result<QJsonObject> root = parseObject(json);
    if (!root.ok()) {
        return root.error();
    }
    ObjectReader rootReader(root.value());
    RadioBatch batch;
    batch.sessionId = rootReader.requiredId(u"session_id");
    batch.seedLabel = rootReader.optionalString(u"seed_label");
    if (const auto index = rootReader.optionalInteger(u"batch_index")) {
        batch.batchIndex = static_cast<int>(*index);
    }
    batch.status = rootReader.optionalString(u"status", QStringLiteral("ok"));
    batch.exhausted = rootReader.optionalBool(u"exhausted", false);
    const QJsonArray tracks = rootReader.requiredArray(u"tracks");
    if (!rootReader.ok()) {
        return rootReader.error();
    }
    for (qsizetype i = 0; i < tracks.size(); ++i) {
        const QString path = itemPath("tracks", i);
        if (!tracks[i].isObject()) {
            return Error::decode(path + QStringLiteral(" is not an object"));
        }
        ObjectReader reader(tracks[i].toObject(), path);
        const RecommendedTrack track = recommendedTrackOf(&reader);
        if (!reader.ok()) {
            return reader.error();
        }
        if (!track.entityUid.isEmpty()) {
            batch.tracks.append(track);
        }
    }
    return batch;
}

Result<MusicPath> parseMusicPath(const QByteArray& json) {
    const Result<QJsonObject> root = parseObject(json);
    if (!root.ok()) {
        return root.error();
    }
    ObjectReader rootReader(root.value());
    MusicPath path;
    path.name = rootReader.optionalString(u"name");
    path.partial = rootReader.optionalBool(u"is_partial", false);
    const QJsonArray tracks = rootReader.requiredArray(u"tracks");
    if (!rootReader.ok()) {
        return rootReader.error();
    }
    QList<std::pair<qint64, RecommendedTrack>> steps;
    for (qsizetype i = 0; i < tracks.size(); ++i) {
        const QString itemName = itemPath("tracks", i);
        if (!tracks[i].isObject()) {
            return Error::decode(itemName + QStringLiteral(" is not an object"));
        }
        ObjectReader reader(tracks[i].toObject(), itemName);
        const qint64 step = reader.optionalInteger(u"step").value_or(i);
        const RecommendedTrack track = recommendedTrackOf(&reader);
        if (!reader.ok()) {
            return reader.error();
        }
        if (!track.entityUid.isEmpty()) {
            steps.append({step, track});
        }
    }
    std::stable_sort(steps.begin(), steps.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    for (const auto& [step, track] : std::as_const(steps)) {
        Q_UNUSED(step);
        path.tracks.append(track);
    }
    return path;
}

} // namespace crate::recs
