#include "crate/ui/cachestatusmodel.h"

#include <algorithm>

#include "crate/cratepreferences.h"
#include "crate/ui/delegates/delegateroles.h"
#include "moc_cachestatusmodel.cpp"

namespace crate {

namespace {

QString keyOf(const QString& serverId, const QString& entityUid) {
    return serverId + QLatin1Char('/') + entityUid;
}

QString purposeOf(cache::Priority priority) {
    switch (priority) {
    case cache::Priority::Deck:
        return CacheDownloadsModel::tr("Deck");
    case cache::Priority::AutoDj:
        return CacheDownloadsModel::tr("Auto DJ");
    case cache::Priority::Prefetch:
        return CacheDownloadsModel::tr("Prefetch");
    case cache::Priority::OfflineSet:
        return CacheDownloadsModel::tr("Offline set");
    }
    return QString();
}

double fraction(qint64 part, qint64 whole) {
    return whole > 0 ? std::clamp(static_cast<double>(part) / static_cast<double>(whole), 0.0, 1.0)
                     : 0.0;
}

bool sameTracks(const QList<cache::DownloadInfo>& a, const QList<cache::DownloadInfo>& b) {
    return std::equal(a.cbegin(),
            a.cend(),
            b.cbegin(),
            b.cend(),
            [](const cache::DownloadInfo& x, const cache::DownloadInfo& y) {
                return x.serverId == y.serverId && x.entityUid == y.entityUid;
            });
}

} // namespace

QString formatCacheBytes(qint64 bytes) {
    constexpr double kKilo = 1000.0;
    const double value = static_cast<double>(std::max<qint64>(bytes, 0));
    if (value < kKilo) {
        return QStringLiteral("%1 B").arg(QString::number(value, 'f', 0));
    }
    if (value < kKilo * kKilo) {
        return QStringLiteral("%1 KB").arg(QString::number(value / kKilo, 'f', 0));
    }
    if (value < kKilo * kKilo * kKilo) {
        return QStringLiteral("%1 MB").arg(QString::number(value / (kKilo * kKilo), 'f', 1));
    }
    return QStringLiteral("%1 GB").arg(QString::number(value / (kKilo * kKilo * kKilo), 'f', 1));
}

const QString CacheStatusModel::kOfflineSetPrefix = QStringLiteral("offline-set:");

CacheDownloadsModel::CacheDownloadsModel(QObject* pParent)
        : QAbstractTableModel(pParent) {
}

int CacheDownloadsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_downloads.size());
}

int CacheDownloadsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant CacheDownloadsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_downloads.size()) {
        return QVariant();
    }
    const cache::DownloadInfo& download = m_downloads[index.row()];
    const CacheTrackTitle title = m_pTitles
            ? m_pTitles->value(keyOf(download.serverId, download.entityUid))
            : CacheTrackTitle();
    switch (index.column()) {
    case Title:
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
            // A track only the DJ search found has no row in the library
            return title.title.isEmpty() ? download.entityUid : title.title;
        }
        break;
    case Artist:
        if (role == Qt::DisplayRole) {
            return title.artist;
        }
        break;
    case Purpose:
        if (role == Qt::DisplayRole) {
            return purposeOf(download.priority);
        }
        break;
    case Progress:
        switch (role) {
        case Qt::DisplayRole:
            if (!download.active) {
                return tr("Queued");
            }
            if (download.bytesTotal > 0) {
                return tr("%1 of %2").arg(formatCacheBytes(download.bytesReceived),
                        formatCacheBytes(download.bytesTotal));
            }
            return formatCacheBytes(download.bytesReceived);
        case delegates::CacheStateRole:
            return static_cast<int>(delegates::CacheState::Downloading);
        case delegates::CacheProgressRole:
            return fraction(download.bytesReceived, download.bytesTotal);
        default:
            break;
        }
        break;
    default:
        break;
    }
    return QVariant();
}

QVariant CacheDownloadsModel::headerData(
        int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QVariant();
    }
    switch (section) {
    case Title:
        return tr("Title");
    case Artist:
        return tr("Artist");
    case Purpose:
        return tr("For");
    case Progress:
        return tr("Progress");
    default:
        return QVariant();
    }
}

void CacheDownloadsModel::setDownloads(const QList<cache::DownloadInfo>& downloads) {
    if (sameTracks(downloads, m_downloads)) {
        m_downloads = downloads;
        if (!m_downloads.isEmpty()) {
            emit dataChanged(index(0, Purpose),
                    index(static_cast<int>(m_downloads.size()) - 1, Progress));
        }
        return;
    }
    beginResetModel();
    m_downloads = downloads;
    endResetModel();
}

void CacheDownloadsModel::setTitles(const QHash<QString, CacheTrackTitle>* pTitles) {
    m_pTitles = pTitles;
}

void CacheDownloadsModel::titlesChanged() {
    if (!m_downloads.isEmpty()) {
        emit dataChanged(index(0, Title), index(static_cast<int>(m_downloads.size()) - 1, Artist));
    }
}

CacheOfflineSetsModel::CacheOfflineSetsModel(QObject* pParent)
        : QAbstractTableModel(pParent) {
}

int CacheOfflineSetsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int CacheOfflineSetsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant CacheOfflineSetsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows[index.row()];
    switch (index.column()) {
    case Name:
        if (role == Qt::DisplayRole) {
            return row.name;
        }
        break;
    case Tracks:
        if (role == Qt::DisplayRole) {
            return tr("%1 of %2 tracks").arg(row.set.readyTracks).arg(row.set.tracks);
        }
        break;
    case Size:
        if (role == Qt::DisplayRole) {
            return formatCacheBytes(row.set.readyBytes);
        }
        break;
    case State:
        switch (role) {
        case delegates::CacheStateRole:
            return static_cast<int>(row.set.readyTracks >= row.set.tracks
                            ? delegates::CacheState::Ready
                            : delegates::CacheState::Downloading);
        case delegates::CacheProgressRole:
            return fraction(row.set.readyTracks, row.set.tracks);
        default:
            break;
        }
        break;
    default:
        break;
    }
    return QVariant();
}

QVariant CacheOfflineSetsModel::headerData(
        int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QVariant();
    }
    switch (section) {
    case Name:
        return tr("Set");
    case Tracks:
        return tr("Tracks");
    case Size:
        return tr("Size");
    case State:
        return tr("Cache");
    default:
        return QVariant();
    }
}

void CacheOfflineSetsModel::setRows(const QList<Row>& rows) {
    const bool sameSets = std::equal(rows.cbegin(),
            rows.cend(),
            m_rows.cbegin(),
            m_rows.cend(),
            [](const Row& a, const Row& b) {
                return a.set.serverId == b.set.serverId && a.set.reason == b.set.reason;
            });
    if (sameSets) {
        m_rows = rows;
        if (!m_rows.isEmpty()) {
            emit dataChanged(index(0, Name), index(static_cast<int>(m_rows.size()) - 1, State));
        }
        return;
    }
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

CacheStatusModel::CacheStatusModel(Options options, QObject* pParent)
        : QObject(pParent),
          m_pCache(options.pCache),
          m_pPreferences(options.pPreferences),
          m_options(std::move(options)),
          m_pDownloads(new CacheDownloadsModel(this)),
          m_pOfflineSets(new CacheOfflineSetsModel(this)) {
    m_pDownloads->setTitles(&m_titles);
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(m_options.refreshDelay);
    connect(&m_refreshTimer, &QTimer::timeout, this, &CacheStatusModel::refresh);
    cache::AudioCache* const pCache = m_pCache;
    m_pCache = nullptr;
    setSources(pCache, m_pPreferences);
}

void CacheStatusModel::setSources(cache::AudioCache* pCache, CratePreferences* pPreferences) {
    m_pPreferences = pPreferences;
    if (pCache != m_pCache) {
        if (m_pCache) {
            disconnect(m_pCache, nullptr, this, nullptr);
        }
        m_pCache = pCache;
        if (m_pCache) {
            connect(m_pCache,
                    &cache::AudioCache::usageChanged,
                    this,
                    &CacheStatusModel::scheduleRefresh);
            connect(m_pCache,
                    &cache::AudioCache::loaded,
                    this,
                    &CacheStatusModel::scheduleRefresh);
            connect(m_pCache,
                    &cache::AudioCache::trackChanged,
                    this,
                    &CacheStatusModel::scheduleRefresh);
        }
    }
    refresh();
}

CacheStatusModel::~CacheStatusModel() = default;

void CacheStatusModel::scheduleRefresh() {
    if (!m_refreshTimer.isActive()) {
        m_refreshTimer.start();
    }
}

void CacheStatusModel::setLimitBytes(qint64 limitBytes) {
    if (m_pPreferences) {
        // The cache follows the preferences
        m_pPreferences->setCacheLimitBytes(limitBytes);
    } else if (m_pCache) {
        m_pCache->setLimitBytes(limitBytes);
    }
    refresh();
}

void CacheStatusModel::refresh() {
    m_refreshTimer.stop();
    if (!m_pCache) {
        m_usage = cache::Usage();
        m_limitBytes = 0;
        m_pDownloads->setDownloads({});
        m_pOfflineSets->setRows({});
        emit changed();
        return;
    }
    m_usage = m_pCache->usage();
    m_limitBytes = m_pCache->limitBytes();
    m_pDownloads->setDownloads(m_pCache->downloads());

    QList<CacheOfflineSetsModel::Row> rows;
    const QList<cache::PinnedSet> sets = m_pCache->pinnedSets(kOfflineSetPrefix);
    for (const cache::PinnedSet& set : sets) {
        QString name = m_options.offlineSetName
                ? m_options.offlineSetName(set.serverId, set.reason)
                : QString();
        if (name.isEmpty()) {
            name = set.reason.mid(kOfflineSetPrefix.size());
        }
        rows.append({set, name});
    }
    m_pOfflineSets->setRows(rows);
    lookUpMissingTitles();
    emit changed();
}

void CacheStatusModel::lookUpMissingTitles() {
    if (!m_options.lookUpTitles) {
        return;
    }
    QList<cache::TrackKey> missing;
    for (const cache::DownloadInfo& download : m_pDownloads->downloads()) {
        const QString key = keyOf(download.serverId, download.entityUid);
        if (!m_titlesAsked.contains(key)) {
            m_titlesAsked.insert(key);
            missing.append({download.serverId, download.entityUid});
        }
    }
    if (missing.isEmpty()) {
        return;
    }
    m_options.lookUpTitles(missing,
            [self = QPointer<CacheStatusModel>(this)](
                    const QHash<QString, CacheTrackTitle>& titles) {
                if (!self || titles.isEmpty()) {
                    return;
                }
                self->m_titles.insert(titles);
                self->m_pDownloads->titlesChanged();
            });
}

} // namespace crate
