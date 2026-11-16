#include "crate/recs/compatiblesmodel.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <algorithm>
#include <cmath>

#include "moc_compatiblesmodel.cpp"
#include "util/logger.h"

namespace crate::recs {

namespace {

const mixxx::Logger kLogger("crate::recs::CompatiblesModel");

using Row = library::CrateServerTrackModel::Row;
using library::WriteStatus;

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

QVariant nullable(const QString& value) {
    return value.isEmpty() ? QVariant() : QVariant(value);
}

template<typename T>
QVariant nullable(const std::optional<T>& value) {
    return value ? QVariant::fromValue(*value) : QVariant();
}

template<typename T>
std::optional<T> optionalOf(const QVariant& value) {
    return value.isNull() ? std::nullopt : std::optional<T>(value.value<T>());
}

/// The energy column's level, 1 to 10, of the server's 0 to 1.
int energyLevel(double energy) {
    return std::clamp(static_cast<int>(std::lround(energy * 10)), 1, 10);
}

/// The order of the Harmonic column: the closest relationship first.
int harmonicRank(const QString& relationship) {
    static const QStringList kOrder{QStringLiteral("same"),
            QStringLiteral("adjacent"),
            QStringLiteral("relative"),
            QStringLiteral("incompatible")};
    const qsizetype rank = kOrder.indexOf(relationship);
    return rank < 0 ? static_cast<int>(kOrder.size()) : static_cast<int>(rank);
}

/// Writes what the answer carries of each track, keeping what only a
/// container sync brings (format, size, revisions, profile) and `playable`,
/// which the answer does not carry.
bool upsertTracks(const QSqlDatabase& database,
        const QString& serverId,
        const CompatibleTracks& compatibles,
        const QString& now,
        QSqlError* pError) {
    QSqlQuery upsert(database);
    upsert.prepare(QStringLiteral(
            "INSERT INTO crateserver_tracks (server_id, entity_uid, title, artist, "
            "album, duration_ms, bpm, camelot, energy, genre, cover_url, updated_at) "
            "VALUES (:server, :uid, :title, :artist, :album, :duration, :bpm, "
            ":camelot, :energy, :genre, :cover, :now) "
            "ON CONFLICT(server_id, entity_uid) DO UPDATE SET "
            "title = excluded.title, artist = excluded.artist, "
            "album = excluded.album, duration_ms = excluded.duration_ms, "
            "bpm = excluded.bpm, camelot = excluded.camelot, "
            "energy = excluded.energy, genre = excluded.genre, "
            "cover_url = excluded.cover_url, updated_at = excluded.updated_at"));
    for (const CompatibleTrack& item : compatibles.items) {
        const library::RemoteTrack& track = item.track;
        upsert.bindValue(QStringLiteral(":server"), serverId);
        upsert.bindValue(QStringLiteral(":uid"), track.entityUid);
        upsert.bindValue(QStringLiteral(":title"), text(track.title));
        upsert.bindValue(QStringLiteral(":artist"), text(track.artist));
        upsert.bindValue(QStringLiteral(":album"), text(track.album));
        upsert.bindValue(QStringLiteral(":duration"), nullable(track.durationMs));
        upsert.bindValue(QStringLiteral(":bpm"), nullable(track.bpm));
        upsert.bindValue(QStringLiteral(":camelot"), nullable(track.camelot));
        upsert.bindValue(QStringLiteral(":energy"), nullable(track.energy));
        upsert.bindValue(QStringLiteral(":genre"), nullable(track.genre));
        upsert.bindValue(QStringLiteral(":cover"), nullable(track.coverUrl));
        upsert.bindValue(QStringLiteral(":now"), now);
        if (!upsert.exec()) {
            *pError = upsert.lastError();
            return false;
        }
    }
    return true;
}

/// The catalogue's rows of the uids, by lowercase uid.
bool readRows(const QSqlDatabase& database,
        const QString& serverId,
        const QStringList& uids,
        QHash<QString, Row>* pRows,
        QSqlError* pError) {
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT id, entity_uid, title, artist, album, bpm, camelot, duration_ms, "
            "energy, genre, profile_quality, bitrate, format, sample_rate, byte_length, "
            "source_revision FROM crateserver_tracks "
            "WHERE server_id = :server AND entity_uid = :uid"));
    for (const QString& uid : uids) {
        query.bindValue(QStringLiteral(":server"), serverId);
        query.bindValue(QStringLiteral(":uid"), uid);
        if (!query.exec()) {
            *pError = query.lastError();
            return false;
        }
        if (!query.next()) {
            continue;
        }
        Row row;
        row.id = query.value(0).toLongLong();
        row.serverId = serverId;
        row.entityUid = query.value(1).toString();
        row.title = query.value(2).toString();
        row.artist = query.value(3).toString();
        row.album = query.value(4).toString();
        row.bpm = optionalOf<double>(query.value(5));
        row.camelot = query.value(6).toString();
        row.durationMs = optionalOf<qint64>(query.value(7));
        row.energy = optionalOf<double>(query.value(8));
        row.genre = query.value(9).toString();
        row.profileQuality = query.value(10).toString();
        row.bitrate = optionalOf<qint64>(query.value(11));
        row.format = query.value(12).toString();
        row.sampleRate = optionalOf<qint64>(query.value(13));
        row.byteLength = optionalOf<qint64>(query.value(14));
        row.sourceRevision = query.value(15).toString();
        pRows->insert(uid.toLower(), row);
        query.finish();
    }
    return true;
}

} // namespace

Result<StoredCompatibles> storeCompatibles(const QSqlDatabase& database,
        const QString& serverId,
        const QString& seedEntityUid,
        const CompatibleTracks& compatibles,
        const QString& now,
        std::optional<double> seedBpm,
        library::WriteStatus* pStatus) {
    QSqlDatabase db = database;
    QSqlError error;
    QHash<QString, Row> rows;
    QStringList uids;
    for (const CompatibleTrack& item : compatibles.items) {
        uids.append(item.track.entityUid);
    }
    const auto fail = [&](const QSqlError& sqlError) -> Result<StoredCompatibles> {
        db.rollback();
        const WriteStatus status = statusOf(sqlError);
        if (pStatus) {
            *pStatus = status;
        }
        if (status == WriteStatus::Failed) {
            kLogger.warning() << "Cannot store the compatible tracks:" << sqlError.text();
        }
        return Error::storage(QStringLiteral("Cannot store the compatible tracks"));
    };
    if (!db.transaction()) {
        return fail(db.lastError());
    }
    QHash<QString, Row> seed;
    if (!upsertTracks(database, serverId, compatibles, now, &error) ||
            !readRows(database, serverId, uids, &rows, &error) ||
            !readRows(database, serverId, {seedEntityUid}, &seed, &error)) {
        return fail(error);
    }
    if (!db.commit()) {
        return fail(db.lastError());
    }
    if (pStatus) {
        *pStatus = WriteStatus::Ok;
    }

    const Row seedRow = seed.value(seedEntityUid.toLower());
    const std::optional<double> followedBpm = seedRow.bpm ? seedRow.bpm : seedBpm;
    StoredCompatibles stored;
    stored.seedCamelot = seedRow.camelot.toUpper();
    stored.rows.reserve(compatibles.items.size());
    for (const CompatibleTrack& item : compatibles.items) {
        const QString key = item.track.entityUid.toLower();
        const auto it = rows.constFind(key);
        if (it == rows.cend() || stored.scores.contains(key)) {
            continue;
        }
        Row row = it.value();
        row.position = static_cast<int>(stored.rows.size()) + 1;
        CompatibleScores scores;
        scores.score = item.score;
        scores.harmonicRelationship = item.harmonicRelationship;
        if (row.bpm && followedBpm) {
            scores.bpmDelta = *row.bpm - *followedBpm;
        }
        if (row.energy && seedRow.energy) {
            scores.energyDelta = energyLevel(*row.energy) - energyLevel(*seedRow.energy);
        }
        stored.scores.insert(key, scores);
        stored.rows.append(row);
    }
    return stored;
}

const QString CompatiblesModel::kScoreField = QStringLiteral("crate_score");
const QString CompatiblesModel::kHarmonicField = QStringLiteral("crate_harmonic");
const QString CompatiblesModel::kBpmDeltaField = QStringLiteral("crate_bpm_delta");
const QString CompatiblesModel::kEnergyDeltaField = QStringLiteral("crate_energy_delta");

CompatiblesModel::CompatiblesModel(const QSqlDatabase& database,
        mixxx::DbConnectionPoolPtr pDbConnectionPool,
        QObject* pParent)
        : CrateServerTrackModel(
                  database, std::move(pDbConnectionPool), "crateserver_compatibles", pParent) {
}

void CompatiblesModel::setResults(const QString& serverId,
        const QString& seedEntityUid,
        const StoredCompatibles& results) {
    m_seedEntityUid = seedEntityUid;
    m_seedCamelot = results.seedCamelot;
    m_scores = results.scores;
    m_keys.clear();
    for (const Row& row : results.rows) {
        const QString key = row.camelot.toUpper();
        if (!key.isEmpty() && !m_keys.contains(key)) {
            m_keys.append(key);
        }
    }
    setRows(serverId, seedEntityUid.toLower(), results.rows);
}

void CompatiblesModel::clear() {
    m_seedEntityUid.clear();
    m_seedCamelot.clear();
    m_scores.clear();
    m_keys.clear();
    setRows(QString(), QString(), {});
}

void CompatiblesModel::setKeyFilter(const QString& camelot) {
    const QString key = camelot.trimmed().toUpper();
    if (key == m_keyFilter) {
        return;
    }
    m_keyFilter = key;
    refilter();
}

std::optional<CompatibleScores> CompatiblesModel::scoresOf(const QString& entityUid) const {
    const auto it = m_scores.constFind(entityUid.toLower());
    if (it == m_scores.cend()) {
        return std::nullopt;
    }
    return it.value();
}

const CompatibleScores* CompatiblesModel::scoresOfRow(const Row& row) const {
    const auto it = m_scores.constFind(row.entityUid.toLower());
    return it == m_scores.cend() ? nullptr : &it.value();
}

QString CompatiblesModel::harmonicLabel(const QString& relationship) {
    if (relationship == QLatin1String("same")) {
        //: Harmonic relation of a compatible track: the same key
        return tr("Same key");
    }
    if (relationship == QLatin1String("adjacent")) {
        //: Harmonic relation of a compatible track: one step on the Camelot wheel
        return tr("Adjacent");
    }
    if (relationship == QLatin1String("relative")) {
        //: Harmonic relation of a compatible track: the relative major or minor
        return tr("Relative");
    }
    if (relationship == QLatin1String("incompatible")) {
        //: Harmonic relation of a compatible track: the keys clash
        return tr("Clash");
    }
    return QString();
}

int CompatiblesModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : kCompatiblesColumnCount;
}

QVariant CompatiblesModel::data(const QModelIndex& index, int role) const {
    const int column = index.column();
    if (column < kColumnCount) {
        return CrateServerTrackModel::data(index, role);
    }
    const Row* pRow = index.isValid() ? rowAt(index.row()) : nullptr;
    if (!pRow) {
        return QVariant();
    }
    if (role == Qt::TextAlignmentRole) {
        return column == kHarmonicColumn
                ? QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter)
                : QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole && role != Qt::EditRole) {
        return QVariant();
    }
    const CompatibleScores* pScores = scoresOfRow(*pRow);
    if (!pScores) {
        return QVariant();
    }
    switch (column) {
    case kScoreColumn:
        return QString::number(std::lround(pScores->score * 100));
    case kHarmonicColumn:
        return harmonicLabel(pScores->harmonicRelationship);
    case kBpmDeltaColumn: {
        if (!pScores->bpmDelta) {
            return QVariant();
        }
        const double delta = *pScores->bpmDelta;
        if (std::abs(delta) < 0.05) {
            return QStringLiteral("0.0");
        }
        return QString::asprintf("%+.1f", delta);
    }
    case kEnergyDeltaColumn: {
        if (!pScores->energyDelta) {
            return QVariant();
        }
        const int delta = *pScores->energyDelta;
        return delta == 0 ? QStringLiteral("0") : QString::asprintf("%+d", delta);
    }
    default:
        return QVariant();
    }
}

QVariant CompatiblesModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || section < kColumnCount ||
            section >= kCompatiblesColumnCount) {
        return CrateServerTrackModel::headerData(section, orientation, role);
    }
    switch (role) {
    case TrackModel::kHeaderNameRole:
        switch (section) {
        case kScoreColumn:
            return kScoreField;
        case kHarmonicColumn:
            return kHarmonicField;
        case kBpmDeltaColumn:
            return kBpmDeltaField;
        default:
            return kEnergyDeltaField;
        }
    case TrackModel::kHeaderWidthRole:
        return 0;
    case Qt::TextAlignmentRole:
        return section == kHarmonicColumn ? QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter)
                                          : QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
    case Qt::DisplayRole:
        break;
    default:
        return QVariant();
    }
    switch (section) {
    case kScoreColumn:
        //: How well a track follows the one on air, 0 to 100
        return tr("Score");
    case kHarmonicColumn:
        //: How the key of a compatible track relates to the one on air
        return tr("Harmonic");
    case kBpmDeltaColumn:
        //: The BPM of a compatible track minus the one on air's
        return tr("Δ BPM");
    default:
        //: The energy level of a compatible track minus the one on air's
        return tr("Δ Nrg");
    }
}

int CompatiblesModel::fieldIndex(const QString& fieldName) const {
    if (fieldName == kScoreField) {
        return kScoreColumn;
    }
    if (fieldName == kHarmonicField) {
        return kHarmonicColumn;
    }
    if (fieldName == kBpmDeltaField) {
        return kBpmDeltaColumn;
    }
    if (fieldName == kEnergyDeltaField) {
        return kEnergyDeltaColumn;
    }
    return CrateServerTrackModel::fieldIndex(fieldName);
}

bool CompatiblesModel::isColumnSortable(int column) const {
    if (column >= kColumnCount) {
        return column < kCompatiblesColumnCount;
    }
    return CrateServerTrackModel::isColumnSortable(column);
}

bool CompatiblesModel::acceptsRow(const Row& row) const {
    return m_keyFilter.isEmpty() || row.camelot.toUpper() == m_keyFilter;
}

std::optional<bool> CompatiblesModel::lessThan(
        int column, const Row& left, const Row& right) const {
    if (column < kColumnCount) {
        return std::nullopt;
    }
    const CompatibleScores* pLeft = scoresOfRow(left);
    const CompatibleScores* pRight = scoresOfRow(right);
    if (!pLeft || !pRight) {
        return std::nullopt;
    }
    const bool ascending = sortOrder() == Qt::AscendingOrder;
    const auto ordered = [ascending](auto a, auto b) -> std::optional<bool> {
        if (a == b) {
            return std::nullopt;
        }
        return ascending ? a < b : b < a;
    };
    // Missing values last
    const auto orderedOptional = [&ordered](const auto& a, const auto& b) -> std::optional<bool> {
        if (a.has_value() != b.has_value()) {
            return a.has_value();
        }
        return a ? ordered(*a, *b) : std::nullopt;
    };
    switch (column) {
    case kScoreColumn:
        return ordered(pLeft->score, pRight->score);
    case kHarmonicColumn:
        return ordered(harmonicRank(pLeft->harmonicRelationship),
                harmonicRank(pRight->harmonicRelationship));
    case kBpmDeltaColumn: {
        // The closest tempo first
        const auto distance = [](const std::optional<double>& delta) {
            return delta ? std::optional<double>(std::abs(*delta)) : std::nullopt;
        };
        return orderedOptional(distance(pLeft->bpmDelta), distance(pRight->bpmDelta));
    }
    case kEnergyDeltaColumn:
        return orderedOptional(pLeft->energyDelta, pRight->energyDelta);
    default:
        return std::nullopt;
    }
}

} // namespace crate::recs
