#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <chrono>
#include <functional>

#include "crate/cache/audiocache.h"

namespace crate {

class CratePreferences;

/// The title and artist of a Crate track, from the library's tables.
struct CacheTrackTitle {
    QString title;
    QString artist;
};

/// The downloads of the audio cache, a row each: on their way first, then
/// waiting, the most urgent first. Columns: title, artist, what the track is
/// for (deck, Auto DJ, prefetch or offline set) and its progress, which
/// also has crate::delegates::CacheStateRole and CacheProgressRole.
class CacheDownloadsModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    enum Column {
        Title = 0,
        Artist,
        Purpose,
        Progress,
        ColumnCount,
    };

    explicit CacheDownloadsModel(QObject* pParent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override;

    const QList<cache::DownloadInfo>& downloads() const {
        return m_downloads;
    }

  private:
    friend class CacheStatusModel;
    void setDownloads(const QList<cache::DownloadInfo>& downloads);
    void setTitles(const QHash<QString, CacheTrackTitle>* pTitles);
    void titlesChanged();

    QList<cache::DownloadInfo> m_downloads;
    const QHash<QString, CacheTrackTitle>* m_pTitles = nullptr;
};

/// The offline sets the cache keeps (the tracks pinned for
/// "offline-set:<kind>:<id>"), a row each. Columns: the set's name, its
/// tracks that are complete of all, their size and a state for
/// crate::delegates::CacheStateDelegate.
class CacheOfflineSetsModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    enum Column {
        Name = 0,
        Tracks,
        Size,
        State,
        ColumnCount,
    };

    explicit CacheOfflineSetsModel(QObject* pParent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override;

  private:
    friend class CacheStatusModel;
    struct Row {
        cache::PinnedSet set;
        QString name;
    };
    void setRows(const QList<Row>& rows);

    QList<Row> m_rows;
};

/// What the cache view shows (plan F4.C-PR4): the cache's use and limit,
/// its downloads and its offline sets. A model without widgets (D11) that
/// follows the cache: the changes that come together, such as the chunks of
/// a download, are read once per Options::refreshDelay.
class CacheStatusModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(qint64 usedBytes READ usedBytes NOTIFY changed)
    Q_PROPERTY(qint64 limitBytes READ limitBytes WRITE setLimitBytes NOTIFY changed)
    Q_PROPERTY(qint64 pinnedBytes READ pinnedBytes NOTIFY changed)
    Q_PROPERTY(qint64 inUseBytes READ inUseBytes NOTIFY changed)
    Q_PROPERTY(int readyFiles READ readyFiles NOTIFY changed)
    Q_PROPERTY(int activeDownloads READ activeDownloads NOTIFY changed)
    Q_PROPERTY(int queuedDownloads READ queuedDownloads NOTIFY changed)
  public:
    using TitlesReady = std::function<void(const QHash<QString, CacheTrackTitle>& titles)>;

    struct Options {
        /// Not owned; setSources() can give it later.
        cache::AudioCache* pCache = nullptr;
        /// Not owned. When there is one, a new limit goes to the
        /// preferences, which keep it and pass it on to the cache.
        CratePreferences* pPreferences = nullptr;
        /// Looks up the titles of tracks off the main thread, then calls
        /// `ready` on the main thread with those it found, by
        /// "<server>/<uid>". Optional.
        std::function<void(const QList<cache::TrackKey>& tracks, TitlesReady ready)>
                lookUpTitles;
        /// The name of an offline set, from its server and pin reason.
        /// Optional.
        std::function<QString(const QString& serverId, const QString& reason)> offlineSetName;
        std::chrono::milliseconds refreshDelay{100};
    };

    static const QString kOfflineSetPrefix;

    explicit CacheStatusModel(Options options, QObject* pParent = nullptr);
    ~CacheStatusModel() override;

    qint64 usedBytes() const {
        return m_usage.readyBytes;
    }
    qint64 limitBytes() const {
        return m_limitBytes;
    }
    void setLimitBytes(qint64 limitBytes);
    /// Kept offline (offline sets).
    qint64 pinnedBytes() const {
        return m_usage.pinnedBytes;
    }
    /// Kept only while a player or the Auto DJ queue has them.
    qint64 inUseBytes() const {
        return m_usage.inUseBytes;
    }
    int readyFiles() const {
        return m_usage.readyFiles;
    }
    int activeDownloads() const {
        return m_usage.activeDownloads;
    }
    int queuedDownloads() const {
        return m_usage.queuedDownloads;
    }

    CacheDownloadsModel* downloads() const {
        return m_pDownloads;
    }
    CacheOfflineSetsModel* offlineSets() const {
        return m_pOfflineSets;
    }

    /// The cache to show, and the preferences its limit goes to, once there
    /// are some.
    void setSources(cache::AudioCache* pCache, CratePreferences* pPreferences);

    /// Reads the cache now.
    void refresh();

  signals:
    void changed();

  private:
    void scheduleRefresh();
    void lookUpMissingTitles();

    QPointer<cache::AudioCache> m_pCache;
    QPointer<CratePreferences> m_pPreferences;
    const Options m_options;
    CacheDownloadsModel* m_pDownloads;
    CacheOfflineSetsModel* m_pOfflineSets;
    QTimer m_refreshTimer;
    cache::Usage m_usage;
    qint64 m_limitBytes = 0;
    /// Titles by "<server>/<uid>", and the tracks asked for.
    QHash<QString, CacheTrackTitle> m_titles;
    QSet<QString> m_titlesAsked;
};

/// "18.4 GB", "512.0 MB", "12 KB": bytes in decimal units with one decimal, as the
/// mockups write sizes.
QString formatCacheBytes(qint64 bytes);

} // namespace crate
