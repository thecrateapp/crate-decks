#include "library/crateserver/crateservertrackmodel.h"

#include <QAction>
#include <QFutureWatcher>
#include <QHash>
#include <QMimeData>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QtConcurrentRun>
#include <cmath>

#include "crate/cache/audiocache.h"
#include "crate/library/crateloadcoordinator.h"
#include "crate/ui/delegates/delegateroles.h"
#include "crate/ui/delegates/delegatestyle.h"
#include "crate/ui/delegates/statedelegates.h"
#include "library/dao/trackschema.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/duration.h"
#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::CrateServerTrackModel");

// SQLITE_BUSY and SQLITE_LOCKED: a write of the sync is under way.
bool isBusy(const QSqlError& error) {
    bool ok = false;
    const int code = error.nativeErrorCode().toInt(&ok) & 0xff;
    return ok && (code == 5 || code == 6);
}

constexpr int kReadAttempts = 4;
constexpr unsigned long kReadRetryMillis = 25;
/// How often the Cache column repaints while tracks download.
constexpr int kRepaintIntervalMs = 100;

QString trackKey(const QString& serverId, const QString& entityUid) {
    return serverId + QLatin1Char('\n') + entityUid.toLower();
}

const QString& fieldOf(int column) {
    static const QString kFields[CrateServerTrackModel::kColumnCount] = {
            LIBRARYTABLE_ID,
            PLAYLISTTRACKSTABLE_POSITION,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_DURATION,
            CrateServerTrackModel::kEnergyField,
            LIBRARYTABLE_GENRE,
            CrateServerTrackModel::kProfileField,
            CrateServerTrackModel::kCacheField,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_FILETYPE,
            LIBRARYTABLE_SAMPLERATE,
    };
    static const QString kNone;
    return column >= 0 && column < CrateServerTrackModel::kColumnCount ? kFields[column] : kNone;
}

/// The SQL that sorts by `column`; empty for a column that does not sort.
QString sortExpression(int column) {
    switch (column) {
    case CrateServerTrackModel::kPositionColumn:
        return QStringLiteral("ct.position");
    case CrateServerTrackModel::kTitleColumn:
        return QStringLiteral("t.title COLLATE NOCASE");
    case CrateServerTrackModel::kArtistColumn:
        return QStringLiteral("t.artist COLLATE NOCASE");
    case CrateServerTrackModel::kAlbumColumn:
        return QStringLiteral("t.album COLLATE NOCASE");
    case CrateServerTrackModel::kGenreColumn:
        return QStringLiteral("t.genre COLLATE NOCASE");
    case CrateServerTrackModel::kBpmColumn:
        return QStringLiteral("t.bpm");
    case CrateServerTrackModel::kKeyColumn:
        // 1A, 1B, 2A ... 12B
        return QStringLiteral("CAST(t.camelot AS INTEGER), t.camelot");
    case CrateServerTrackModel::kDurationColumn:
        return QStringLiteral("t.duration_ms");
    case CrateServerTrackModel::kEnergyColumn:
        return QStringLiteral("t.energy");
    case CrateServerTrackModel::kProfileColumn:
        return QStringLiteral("t.profile_quality");
    case CrateServerTrackModel::kBitrateColumn:
        return QStringLiteral("t.bitrate");
    case CrateServerTrackModel::kFileTypeColumn:
        return QStringLiteral("t.format");
    case CrateServerTrackModel::kSampleRateColumn:
        return QStringLiteral("t.sample_rate");
    default:
        return QString();
    }
}

/// What the worker needs to read the rows.
struct Request {
    CrateServerTrackModel::Source source;
    qint64 containerId = 0;
    QString serverId;
    QStringList terms;
    QString orderBy;
};

QString escapeLike(QString term) {
    term.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    term.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    term.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return QLatin1Char('%') + term + QLatin1Char('%');
}

template<typename T>
std::optional<T> optionalOf(const QVariant& value) {
    return value.isNull() ? std::nullopt : std::optional<T>(value.value<T>());
}

QVector<CrateServerTrackModel::Row> readRows(const QSqlDatabase& database, const Request& request) {
    const bool container = request.source == CrateServerTrackModel::Source::Container;
    QString sql = QStringLiteral(
            "SELECT t.id, %1, t.entity_uid, t.title, t.artist, t.album, t.bpm, t.camelot, "
            "t.duration_ms, t.energy, t.genre, t.profile_quality, t.bitrate, t.format, "
            "t.sample_rate, t.server_id, t.byte_length, t.source_revision FROM ")
                          .arg(container ? QStringLiteral("ct.position + 1")
                                         : QStringLiteral("0"));
    if (container) {
        sql += QStringLiteral(
                "crateserver_container_tracks ct "
                "JOIN crateserver_containers c ON c.id = ct.container_id "
                "JOIN crateserver_tracks t ON t.server_id = c.server_id "
                "AND t.entity_uid = ct.entity_uid WHERE ct.container_id = :container");
    } else {
        sql += QStringLiteral("crateserver_tracks t WHERE t.server_id = :server");
    }
    for (int i = 0; i < request.terms.size(); ++i) {
        sql += QStringLiteral(
                " AND (t.title LIKE :term%1 ESCAPE '\\' OR t.artist LIKE :term%1 ESCAPE '\\' "
                "OR t.album LIKE :term%1 ESCAPE '\\' OR t.genre LIKE :term%1 ESCAPE '\\')")
                       .arg(i);
    }
    sql += QStringLiteral(" ORDER BY ") + request.orderBy;

    for (int attempt = 1;; ++attempt) {
        QSqlQuery query(database);
        query.setForwardOnly(true);
        query.prepare(sql);
        if (container) {
            query.bindValue(QStringLiteral(":container"), request.containerId);
        } else {
            query.bindValue(QStringLiteral(":server"), request.serverId);
        }
        for (int i = 0; i < request.terms.size(); ++i) {
            query.bindValue(QStringLiteral(":term%1").arg(i), escapeLike(request.terms[i]));
        }
        if (!query.exec()) {
            if (isBusy(query.lastError()) && attempt < kReadAttempts) {
                QThread::msleep(kReadRetryMillis * attempt);
                continue;
            }
            kLogger.warning() << "Cannot read the Crate tracks:" << query.lastError().text();
            return {};
        }
        QVector<CrateServerTrackModel::Row> rows;
        while (query.next()) {
            CrateServerTrackModel::Row row;
            row.id = query.value(0).toLongLong();
            row.position = query.value(1).toInt();
            row.entityUid = query.value(2).toString();
            row.title = query.value(3).toString();
            row.artist = query.value(4).toString();
            row.album = query.value(5).toString();
            row.bpm = optionalOf<double>(query.value(6));
            row.camelot = query.value(7).toString();
            row.durationMs = optionalOf<qint64>(query.value(8));
            row.energy = optionalOf<double>(query.value(9));
            row.genre = query.value(10).toString();
            row.profileQuality = query.value(11).toString();
            row.bitrate = optionalOf<qint64>(query.value(12));
            row.format = query.value(13).toString();
            row.sampleRate = optionalOf<qint64>(query.value(14));
            row.serverId = query.value(15).toString();
            row.byteLength = optionalOf<qint64>(query.value(16));
            row.sourceRevision = query.value(17).toString();
            rows.append(row);
        }
        return rows;
    }
}

/// The server's profile quality as the delegates show it: a partial or
/// legacy profile is usable but not complete; a failed analysis or none at
/// all is no profile.
delegates::ProfileQuality profileQualityOf(const QString& quality) {
    if (quality == QLatin1String("full")) {
        return delegates::ProfileQuality::Full;
    }
    if (quality == QLatin1String("partial") || quality == QLatin1String("legacy")) {
        return delegates::ProfileQuality::Partial;
    }
    if (quality.isEmpty() || quality == QLatin1String("unavailable")) {
        return delegates::ProfileQuality::Missing;
    }
    return delegates::ProfileQuality::Unknown;
}

/// The mockups' energy level, 1 to 10, from the server's 0 to 1.
QString energyLevel(double energy) {
    return QString::number(std::clamp(static_cast<int>(std::lround(energy * 10)), 1, 10));
}

} // namespace

const QString CrateServerTrackModel::kEnergyField = QStringLiteral("crate_energy");
const QString CrateServerTrackModel::kProfileField = QStringLiteral("crate_profile");
const QString CrateServerTrackModel::kCacheField = QStringLiteral("crate_cache");

CrateServerTrackModel::CrateServerTrackModel(Source source,
        const QSqlDatabase& database,
        mixxx::DbConnectionPoolPtr pDbConnectionPool,
        QObject* pParent)
        : QAbstractTableModel(pParent),
          TrackModel(database,
                  source == Source::Container ? "crateserver_container"
                                              : "crateserver_library"),
          m_source(source),
          m_pDbConnectionPool(std::move(pDbConnectionPool)),
          m_sortColumn(source == Source::Container ? kPositionColumn : kArtistColumn) {
    m_pool.setMaxThreadCount(1);
    setDefaultSort(m_sortColumn, Qt::AscendingOrder);
    m_repaintTimer.setSingleShot(true);
    m_repaintTimer.setInterval(kRepaintIntervalMs);
    connect(&m_repaintTimer,
            &QTimer::timeout,
            this,
            &CrateServerTrackModel::repaintChangedTracks);
}

void CrateServerTrackModel::setLoadCoordinator(CrateLoadCoordinator* pCoordinator) {
    if (m_pCoordinator && m_pCoordinator->cache()) {
        disconnect(m_pCoordinator->cache(), nullptr, this, nullptr);
    }
    m_pCoordinator = pCoordinator;
    if (m_pCoordinator && m_pCoordinator->cache()) {
        connect(m_pCoordinator->cache(),
                &cache::AudioCache::trackChanged,
                this,
                &CrateServerTrackModel::onTrackChanged);
        // The cache knows its files once its index is read
        connect(m_pCoordinator->cache(),
                &cache::AudioCache::loaded,
                this,
                &CrateServerTrackModel::repaintCacheColumn);
    }
    repaintCacheColumn();
}

void CrateServerTrackModel::repaintCacheColumn() {
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, kCacheColumn), index(rowCount() - 1, kCacheColumn));
    }
}

void CrateServerTrackModel::onTrackChanged(const QString& serverId, const QString& entityUid) {
    m_changedTracks.insert(trackKey(serverId, entityUid));
    if (!m_repaintTimer.isActive()) {
        m_repaintTimer.start();
    }
}

void CrateServerTrackModel::repaintChangedTracks() {
    const QSet<QString> changed = std::exchange(m_changedTracks, {});
    for (int row = 0; row < m_rows.size(); ++row) {
        if (changed.contains(trackKey(m_rows[row].serverId, m_rows[row].entityUid))) {
            const QModelIndex cell = index(row, kCacheColumn);
            emit dataChanged(cell, cell);
        }
    }
}

std::optional<QString> CrateServerTrackModel::cachedPathOf(const Row& row) const {
    if (!m_pCoordinator || !m_pCoordinator->cache()) {
        return std::nullopt;
    }
    return m_pCoordinator->cache()->cachedPath(row.serverId, row.entityUid);
}

CrateServerTrackModel::~CrateServerTrackModel() {
    m_pool.waitForDone();
}

void CrateServerTrackModel::showContainer(qint64 containerId) {
    DEBUG_ASSERT(m_source == Source::Container);
    m_containerId = containerId;
    load();
}

void CrateServerTrackModel::showLibrary(const QString& serverId) {
    DEBUG_ASSERT(m_source == Source::Library);
    m_serverId = serverId;
    load();
}

void CrateServerTrackModel::select() {
    load();
}

void CrateServerTrackModel::load() {
    const quint64 generation = ++m_generation;
    Request request;
    request.source = m_source;
    request.containerId = m_containerId;
    request.serverId = m_serverId;
    if (m_filterBySearch) {
        request.terms = m_search.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }
    QString expression = sortExpression(m_sortColumn);
    if (expression.isEmpty() ||
            (m_source == Source::Library && m_sortColumn == kPositionColumn)) {
        expression = sortExpression(m_source == Source::Container ? kPositionColumn
                                                                  : kArtistColumn);
    }
    const QString direction = m_sortOrder == Qt::AscendingOrder ? QStringLiteral(" ASC")
                                                                : QStringLiteral(" DESC");
    // Missing values last, then the column, then a stable order
    const QString first = expression.section(QLatin1Char(','), 0, 0)
                                  .remove(QStringLiteral(" COLLATE NOCASE"));
    request.orderBy = QStringLiteral("(%1 IS NULL), ").arg(first) +
            expression.split(QLatin1Char(',')).join(direction + QLatin1Char(',')) + direction +
            (m_source == Source::Container ? QStringLiteral(", ct.position")
                                           : QStringLiteral(", t.id"));

    m_loading = true;
    // The same rows in another order or with new values keep the selection
    const QString rowsKey = modelKey(false);
    auto* pWatcher = new QFutureWatcher<QVector<Row>>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher, generation, rowsKey] {
        pWatcher->deleteLater();
        if (generation != m_generation) {
            return;
        }
        QVector<Row> rows = pWatcher->result();
        if (rowsKey == m_rowsKey && rows.size() == m_rows.size()) {
            swapRowsKeepingSelection(std::move(rows));
        } else {
            beginResetModel();
            m_rows = std::move(rows);
            m_rowsKey = rowsKey;
            endResetModel();
        }
        m_loading = false;
        emit loaded();
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_pDbConnectionPool, request]() {
                const mixxx::DbConnectionPooler pooler(pPool);
                return readRows(mixxx::DbConnectionPooled(pPool), request);
            }));
}

void CrateServerTrackModel::swapRowsKeepingSelection(QVector<Row>&& rows) {
    // A row is its track at its place: a playlist may list a track twice
    const auto keyOf = [](const Row& row) {
        return std::pair(row.id, row.position);
    };
    // Only rows move: the horizontal header keeps its sections and widths
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    QHash<std::pair<qint64, int>, int> newRowOf;
    newRowOf.reserve(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        newRowOf.insert(keyOf(rows[row]), row);
    }
    const QModelIndexList from = persistentIndexList();
    QModelIndexList to;
    to.reserve(from.size());
    for (const QModelIndex& index : from) {
        const int row = index.row() < m_rows.size()
                ? newRowOf.value(keyOf(m_rows[index.row()]), -1)
                : -1;
        to.append(row < 0 ? QModelIndex() : createIndex(row, index.column()));
    }
    m_rows = std::move(rows);
    changePersistentIndexList(from, to);
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

int CrateServerTrackModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int CrateServerTrackModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : kColumnCount;
}

QVariant CrateServerTrackModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows[index.row()];
    const int column = index.column();
    switch (role) {
    case delegates::CamelotRole:
        return column == kKeyColumn ? QVariant(row.camelot) : QVariant();
    case delegates::ProfileQualityRole:
        return column == kProfileColumn
                ? QVariant(static_cast<int>(profileQualityOf(row.profileQuality)))
                : QVariant();
    case delegates::CacheStateRole:
    case delegates::CacheProgressRole: {
        if (column != kCacheColumn) {
            return QVariant();
        }
        cache::TrackState state;
        if (m_pCoordinator && m_pCoordinator->cache()) {
            state = m_pCoordinator->cache()->trackState(row.serverId, row.entityUid);
        }
        if (role == delegates::CacheProgressRole) {
            return state.bytesTotal > 0 ? static_cast<double>(state.bytesReceived) /
                            static_cast<double>(state.bytesTotal)
                                        : 0.0;
        }
        switch (state.kind) {
        case cache::TrackState::Kind::Ready:
            return static_cast<int>(delegates::CacheState::Ready);
        case cache::TrackState::Kind::Queued:
        case cache::TrackState::Kind::Downloading:
            return static_cast<int>(delegates::CacheState::Downloading);
        case cache::TrackState::Kind::Failed:
            return static_cast<int>(delegates::CacheState::Failed);
        case cache::TrackState::Kind::None:
            break;
        }
        return static_cast<int>(delegates::CacheState::None);
    }
    case Qt::TextAlignmentRole:
        switch (column) {
        case kPositionColumn:
        case kBpmColumn:
        case kDurationColumn:
        case kEnergyColumn:
        case kBitrateColumn:
        case kSampleRateColumn:
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
        }
    case Qt::ToolTipRole:
    case Qt::DisplayRole:
    case Qt::EditRole:
        break;
    default:
        return QVariant();
    }

    switch (column) {
    case kIdColumn:
        return row.id;
    case kPositionColumn:
        return row.position;
    case kTitleColumn:
        return row.title;
    case kArtistColumn:
        return row.artist;
    case kAlbumColumn:
        return row.album;
    case kBpmColumn:
        return row.bpm ? QVariant(QString::number(*row.bpm, 'f', m_bpmPrecision)) : QVariant();
    case kKeyColumn:
        return row.camelot;
    case kDurationColumn:
        return row.durationMs
                ? QVariant(mixxx::Duration::formatTime(static_cast<double>(*row.durationMs) /
                          mixxx::Duration::kMillisPerSecond))
                : QVariant();
    case kEnergyColumn:
        return row.energy ? QVariant(energyLevel(*row.energy)) : QVariant();
    case kGenreColumn:
        return row.genre;
    case kProfileColumn:
        // The delegate paints it; the text is for other skins and tooltips
        return row.profileQuality;
    case kCacheColumn:
        return QVariant();
    case kBitrateColumn:
        return row.bitrate ? QVariant(*row.bitrate) : QVariant();
    case kFileTypeColumn:
        return row.format;
    case kSampleRateColumn:
        return row.sampleRate ? QVariant(*row.sampleRate) : QVariant();
    default:
        return QVariant();
    }
}

QVariant CrateServerTrackModel::headerData(
        int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || section < 0 || section >= kColumnCount) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (role) {
    case TrackModel::kHeaderNameRole:
        return fieldOf(section);
    case TrackModel::kHeaderWidthRole:
        return 0;
    case Qt::TextAlignmentRole:
        return data(index(0, section), Qt::TextAlignmentRole);
    case Qt::DisplayRole:
        break;
    default:
        return QVariant();
    }
    switch (section) {
    case kIdColumn:
        return QStringLiteral("Id");
    case kPositionColumn:
        return QStringLiteral("#");
    case kTitleColumn:
        return tr("Title");
    case kArtistColumn:
        return tr("Artist");
    case kBpmColumn:
        return tr("BPM");
    case kKeyColumn:
        return tr("Key");
    case kDurationColumn:
        return tr("Duration");
    case kEnergyColumn:
        //: Energy, as short as the mockups' column
        return tr("Nrg");
    case kGenreColumn:
        return tr("Genre");
    case kProfileColumn:
        //: Crate's Smart Mix profile of the track
        return tr("Profile");
    case kCacheColumn:
        //: Whether the track's audio is in Crate Decks' cache
        return tr("Cache");
    case kAlbumColumn:
        return tr("Album");
    case kBitrateColumn:
        return tr("Bitrate");
    case kFileTypeColumn:
        return tr("Type");
    case kSampleRateColumn:
        return tr("Samplerate");
    default:
        return QVariant();
    }
}

Qt::ItemFlags CrateServerTrackModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    const Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    // Only a cached track drags onto a deck or another list
    if (index.row() < m_rows.size() && cachedPathOf(m_rows[index.row()])) {
        return flags | Qt::ItemIsDragEnabled;
    }
    return flags;
}

QStringList CrateServerTrackModel::mimeTypes() const {
    return {QStringLiteral("text/uri-list")};
}

QMimeData* CrateServerTrackModel::mimeData(const QModelIndexList& indexes) const {
    QList<QUrl> urls;
    QSet<int> rows;
    for (const QModelIndex& index : indexes) {
        if (rows.contains(index.row())) {
            continue;
        }
        rows.insert(index.row());
        const QUrl url = getTrackUrl(index);
        if (url.isValid()) {
            urls.append(url);
        }
    }
    if (urls.isEmpty()) {
        return nullptr;
    }
    auto* pMimeData = new QMimeData();
    pMimeData->setUrls(urls);
    return pMimeData;
}

void CrateServerTrackModel::sort(int column, Qt::SortOrder order) {
    if (!isColumnSortable(column)) {
        return;
    }
    if (column == m_sortColumn && order == m_sortOrder && !m_rows.isEmpty()) {
        return;
    }
    m_sortColumn = column;
    m_sortOrder = order;
    load();
}

TrackPointer CrateServerTrackModel::getTrack(const QModelIndex& index) const {
    // Never downloads: it also runs when rows are selected and menus open
    if (!m_pCoordinator || !index.isValid() || index.row() >= m_rows.size()) {
        return TrackPointer();
    }
    const Row& row = m_rows[index.row()];
    return m_pCoordinator->cachedTrack(row.serverId, row.entityUid);
}

TrackPointer CrateServerTrackModel::getTrackByRef(const TrackRef& trackRef) const {
    Q_UNUSED(trackRef);
    return TrackPointer();
}

bool CrateServerTrackModel::requestTrackLoad(
        const QModelIndex& index, const QString& group, bool play) {
    if (!m_pCoordinator || !index.isValid() || index.row() >= m_rows.size()) {
        return false;
    }
    const Row& row = m_rows[index.row()];
    if (cachedPathOf(row)) {
        // A cached track loads at once, as a local one
        return false;
    }
    m_pCoordinator->requestLoad(trackRef(index.row()), group, play);
    return true;
}

QList<QAction*> CrateServerTrackModel::trackMenuActions(
        const QModelIndexList& indices, QObject* pParent) {
    if (!m_pCoordinator) {
        return {};
    }
    QList<CrateTrackRef> uncached;
    QList<CrateTrackRef> cached;
    for (const QModelIndex& index : indices) {
        if (!index.isValid() || index.row() >= m_rows.size()) {
            continue;
        }
        const CrateTrackRef track = trackRef(index.row());
        (cachedPathOf(m_rows[index.row()]) ? cached : uncached).append(track);
    }
    //: Downloads the selected Crate tracks into Crate Decks' audio cache
    auto* pDownload = new QAction(tr("Download"), pParent);
    pDownload->setEnabled(!uncached.isEmpty());
    connect(pDownload,
            &QAction::triggered,
            this,
            [pCoordinator = m_pCoordinator, uncached] {
                if (pCoordinator) {
                    pCoordinator->download(uncached, cache::Priority::Prefetch);
                }
            });
    //: Deletes the selected tracks' files from Crate Decks' audio cache
    auto* pRemove = new QAction(tr("Remove from cache"), pParent);
    pRemove->setEnabled(!cached.isEmpty());
    connect(pRemove,
            &QAction::triggered,
            this,
            [pCoordinator = m_pCoordinator, cached] {
                if (!pCoordinator) {
                    return;
                }
                for (const CrateTrackRef& track : cached) {
                    pCoordinator->removeFromCache(track.serverId, track.entityUid);
                }
            });
    return {pDownload, pRemove};
}

QUrl CrateServerTrackModel::getTrackUrl(const QModelIndex& index) const {
    const QString location = getTrackLocation(index);
    return location.isEmpty() ? QUrl() : QUrl::fromLocalFile(location);
}

QString CrateServerTrackModel::getTrackLocation(const QModelIndex& index) const {
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return QString();
    }
    return cachedPathOf(m_rows[index.row()]).value_or(QString());
}

TrackId CrateServerTrackModel::getTrackId(const QModelIndex& index) const {
    // The rows' ids are Crate's, not Mixxx's: only a cached track has one in
    // Mixxx's library
    const TrackPointer pTrack = getTrack(index);
    return pTrack ? pTrack->getId() : TrackId();
}

QList<TrackId> CrateServerTrackModel::getTrackIdsForAutoDJ(const QModelIndexList& indices) {
    // Uncached tracks too (F4.C-PR3 decision 1): Auto DJ waits for their
    // files when their turn comes
    if (!m_pCoordinator) {
        return {};
    }
    QList<CrateQueuedTrack> tracks;
    tracks.reserve(indices.size());
    for (const QModelIndex& index : indices) {
        if (!index.isValid() || index.row() >= m_rows.size()) {
            continue;
        }
        const Row& row = m_rows[index.row()];
        CrateQueuedTrack queued;
        queued.track = trackRef(index.row());
        queued.tags.title = row.title;
        queued.tags.artist = row.artist;
        queued.tags.album = row.album;
        queued.tags.genre = row.genre;
        queued.tags.durationMs = row.durationMs;
        tracks.append(queued);
    }
    return m_pCoordinator->trackIdsForAutoDj(tracks);
}

CoverInfo CrateServerTrackModel::getCoverInfo(const QModelIndex& index) const {
    Q_UNUSED(index);
    return CoverInfo();
}

const QVector<int> CrateServerTrackModel::getTrackRows(TrackId trackId) const {
    Q_UNUSED(trackId);
    return {};
}

void CrateServerTrackModel::search(const QString& searchText) {
    if (m_searchHandler && m_searchHandler(searchText)) {
        return;
    }
    if (searchText == m_search && m_filterBySearch) {
        return;
    }
    m_search = searchText;
    m_filterBySearch = true;
    load();
}

const QString CrateServerTrackModel::currentSearch() const {
    return m_search;
}

bool CrateServerTrackModel::isColumnInternal(int column) {
    return column == kIdColumn ||
            (column == kPositionColumn && m_source == Source::Library);
}

bool CrateServerTrackModel::isColumnHiddenByDefault(int column) {
    return column == kAlbumColumn || column == kBitrateColumn || column == kFileTypeColumn ||
            column == kSampleRateColumn;
}

bool CrateServerTrackModel::isColumnSortable(int column) const {
    return !sortExpression(column).isEmpty() &&
            !(column == kPositionColumn && m_source == Source::Library);
}

QAbstractItemDelegate* CrateServerTrackModel::delegateForColumn(
        const int column, QObject* pParent) {
    // The CrateDecks skin's; another skin shows the text
    const delegates::DelegateStyle& style = delegates::DelegateStyle::current();
    if (!style.isValid()) {
        return nullptr;
    }
    switch (column) {
    case kProfileColumn:
        return new delegates::ProfileQualityDelegate(style, pParent);
    case kCacheColumn:
        return new delegates::CacheStateDelegate(style, pParent);
    default:
        // The skin gives Mixxx's columns its own delegates
        return nullptr;
    }
}

TrackModel::Capabilities CrateServerTrackModel::getCapabilities() const {
    if (!m_pCoordinator) {
        return Capability::Sorting;
    }
    // Uncached tracks load through requestTrackLoad() and go to Auto DJ
    // through getTrackIdsForAutoDJ()
    return Capability::Sorting | Capability::LoadToDeck | Capability::LoadToSampler |
            Capability::LoadToPreviewDeck | Capability::AddToAutoDJ;
}

TrackModel::SortColumnId CrateServerTrackModel::sortColumnIdFromColumnIndex(int column) const {
    switch (column) {
    case kPositionColumn:
        return m_source == Source::Container ? SortColumnId::Position : SortColumnId::Invalid;
    case kTitleColumn:
        return SortColumnId::Title;
    case kArtistColumn:
        return SortColumnId::Artist;
    case kAlbumColumn:
        return SortColumnId::Album;
    case kGenreColumn:
        return SortColumnId::Genre;
    case kBpmColumn:
        return SortColumnId::Bpm;
    case kKeyColumn:
        return SortColumnId::Key;
    case kDurationColumn:
        return SortColumnId::Duration;
    case kBitrateColumn:
        return SortColumnId::BitRate;
    case kFileTypeColumn:
        return SortColumnId::FileType;
    case kSampleRateColumn:
        return SortColumnId::SampleRate;
    default:
        return SortColumnId::Invalid;
    }
}

int CrateServerTrackModel::columnIndexFromSortColumnId(SortColumnId sortColumn) const {
    for (int column = 0; column < kColumnCount; ++column) {
        if (sortColumn != SortColumnId::Invalid &&
                sortColumnIdFromColumnIndex(column) == sortColumn) {
            return column;
        }
    }
    return -1;
}

int CrateServerTrackModel::fieldIndex(const QString& fieldName) const {
    for (int column = 0; column < kColumnCount; ++column) {
        if (fieldOf(column) == fieldName) {
            return column;
        }
    }
    return -1;
}

QString CrateServerTrackModel::modelKey(bool noSearch) const {
    QString key = m_source == Source::Container
            ? QStringLiteral("crateserver_container:%1").arg(m_containerId)
            : QStringLiteral("crateserver_library:%1").arg(m_serverId);
    if (!noSearch && !m_search.isEmpty()) {
        key += QLatin1Char('#') + m_search;
    }
    return key;
}

bool CrateServerTrackModel::updateTrackGenre(Track* pTrack, const QString& genre) const {
    Q_UNUSED(pTrack);
    Q_UNUSED(genre);
    return false;
}

#if defined(__EXTRA_METADATA__)
bool CrateServerTrackModel::updateTrackMood(Track* pTrack, const QString& mood) const {
    Q_UNUSED(pTrack);
    Q_UNUSED(mood);
    return false;
}
#endif

QString CrateServerTrackModel::entityUid(int row) const {
    return row >= 0 && row < m_rows.size() ? m_rows[row].entityUid : QString();
}

CrateTrackRef CrateServerTrackModel::trackRef(int row) const {
    CrateTrackRef track;
    if (row < 0 || row >= m_rows.size()) {
        return track;
    }
    const Row& stored = m_rows[row];
    track.serverId = stored.serverId;
    track.entityUid = stored.entityUid;
    track.hints.format = stored.format;
    track.hints.byteLength = stored.byteLength;
    track.hints.sourceRevision = stored.sourceRevision;
    return track;
}

} // namespace crate::library

#include "moc_crateservertrackmodel.cpp"
