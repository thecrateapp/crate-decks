#pragma once

#include <QAbstractTableModel>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVector>
#include <functional>
#include <optional>

#include "library/trackmodel.h"
#include "util/db/dbconnectionpool.h"

namespace crate::library {

class CrateLoadCoordinator;
struct CrateTrackRef;

/// The tracks of a Crate container, or every track Decks has synced from a
/// server, for Mixxx's track table (plan F3.C-PR2).
///
/// Its own table model on purpose, not a BaseSqlTableModel: Mixxx's
/// BaseTrackTableModel makes data() and delegateForColumn() final, so a
/// subclass cannot hand the Crate delegates their roles (Camelot, profile
/// quality, cache state). The rows are read, filtered and sorted on a worker
/// thread and swapped in at once: the main thread never waits for SQLite
/// (design §9).
///
/// Read-only. A track whose file is in the audio cache is a track of Mixxx's
/// library (getTrack(), getTrackId(), dragging); any other loads into a deck
/// through the asynchronous hook (requestTrackLoad(), plan F4.C-PR3), which
/// downloads it first, and the Cache column shows its download. The track
/// menu adds "Download" and "Remove from cache".
class CrateServerTrackModel : public QAbstractTableModel, public TrackModel {
    Q_OBJECT
  public:
    enum Column {
        kIdColumn,
        kPositionColumn,
        kTitleColumn,
        kArtistColumn,
        kBpmColumn,
        kKeyColumn,
        kDurationColumn,
        kEnergyColumn,
        kGenreColumn,
        kProfileColumn,
        kCacheColumn,
        kAlbumColumn,
        kBitrateColumn,
        kFileTypeColumn,
        kSampleRateColumn,
        kColumnCount,
    };

    enum class Source {
        /// One container, in its order, with its positions.
        Container,
        /// Every track of a server that Decks has synced.
        Library,
    };

    /// Field names, as TrackModel::kHeaderNameRole gives them: Mixxx's for
    /// the columns Mixxx has, so the skin styles them as its own.
    static const QString kEnergyField;
    static const QString kProfileField;
    static const QString kCacheField;

    CrateServerTrackModel(Source source,
            const QSqlDatabase& database,
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            QObject* pParent = nullptr);
    ~CrateServerTrackModel() override;

    void showContainer(qint64 containerId);
    void showLibrary(const QString& serverId);
    qint64 containerId() const {
        return m_containerId;
    }
    const QString& serverId() const {
        return m_serverId;
    }
    /// Decimals of the BPM column (Mixxx's library preference).
    void setBpmPrecision(int decimals) {
        m_bpmPrecision = decimals;
    }
    bool isLoading() const {
        return m_loading;
    }

    /// Where the rows' tracks come from and go to; without one, no track
    /// loads. Not owned.
    void setLoadCoordinator(CrateLoadCoordinator* pCoordinator);

    /// Called first when the user searches, with the search box's text: true
    /// when it takes the search (the Crate section's DJ search, F3.C-PR3), so
    /// the rows are not filtered here. Without one, or when it returns false,
    /// the rows are filtered locally by every word.
    using SearchHandler = std::function<bool(const QString& text)>;
    void setSearchHandler(SearchHandler handler) {
        m_searchHandler = std::move(handler);
    }
    /// Shows `text` as the current search without filtering the rows: the
    /// query whose results the model shows.
    void setSearchText(const QString& text) {
        m_search = text;
        m_filterBySearch = false;
    }

    // QAbstractItemModel
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    void sort(int column, Qt::SortOrder order) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

    // TrackModel
    TrackPointer getTrack(const QModelIndex& index) const override;
    TrackPointer getTrackByRef(const TrackRef& trackRef) const override;
    bool requestTrackLoad(const QModelIndex& index, const QString& group, bool play) override;
    QList<QAction*> trackMenuActions(const QModelIndexList& indices, QObject* pParent) override;
    QUrl getTrackUrl(const QModelIndex& index) const override;
    QString getTrackLocation(const QModelIndex& index) const override;
    TrackId getTrackId(const QModelIndex& index) const override;
    CoverInfo getCoverInfo(const QModelIndex& index) const override;
    const QVector<int> getTrackRows(TrackId trackId) const override;
    void search(const QString& searchText) override;
    const QString currentSearch() const override;
    bool isColumnInternal(int column) override;
    bool isColumnHiddenByDefault(int column) override;
    bool isColumnSortable(int column) const override;
    QAbstractItemDelegate* delegateForColumn(const int column, QObject* pParent) override;
    Capabilities getCapabilities() const override;
    SortColumnId sortColumnIdFromColumnIndex(int column) const override;
    int columnIndexFromSortColumnId(SortColumnId sortColumn) const override;
    int fieldIndex(const QString& fieldName) const override;
    void select() override;
    QString modelKey(bool noSearch) const override;
    bool updateTrackGenre(Track* pTrack, const QString& genre) const override;
#if defined(__EXTRA_METADATA__)
    bool updateTrackMood(Track* pTrack, const QString& mood) const override;
#endif

    /// The entity uid of the track in `row`.
    QString entityUid(int row) const;
    /// The track of `row`, with what the library knows of its file.
    CrateTrackRef trackRef(int row) const;

    struct Row {
        qint64 id = 0;
        int position = 0;
        QString serverId;
        QString entityUid;
        QString title;
        QString artist;
        QString album;
        std::optional<double> bpm;
        QString camelot;
        std::optional<qint64> durationMs;
        std::optional<double> energy;
        QString genre;
        QString profileQuality;
        std::optional<qint64> bitrate;
        QString format;
        std::optional<qint64> sampleRate;
        std::optional<qint64> byteLength;
        QString sourceRevision;
    };

  signals:
    /// New rows are in.
    void loaded();

  private:
    void load();
    void swapRowsKeepingSelection(QVector<Row>&& rows);
    /// The cached file of a row's track, if any.
    std::optional<QString> cachedPathOf(const Row& row) const;
    /// Repaints the Cache column of the tracks whose state changed.
    void onTrackChanged(const QString& serverId, const QString& entityUid);
    void repaintChangedTracks();
    void repaintCacheColumn();

    const Source m_source;
    const mixxx::DbConnectionPoolPtr m_pDbConnectionPool;
    QThreadPool m_pool;
    qint64 m_containerId = 0;
    QString m_serverId;
    QString m_search;
    SearchHandler m_searchHandler;
    /// Whether m_search filters the rows, or only names their query.
    bool m_filterBySearch = true;
    int m_sortColumn;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    int m_bpmPrecision = 2;
    /// Bumped on every load, so an older one that ends later is dropped.
    quint64 m_generation = 0;
    bool m_loading = false;
    QVector<Row> m_rows;
    /// modelKey() of the rows shown.
    QString m_rowsKey;
    QPointer<CrateLoadCoordinator> m_pCoordinator;
    /// Tracks whose cache state changed since the last repaint, by
    /// "<server>\n<uid>"; repainted at most every kRepaintIntervalMs.
    QSet<QString> m_changedTracks;
    QTimer m_repaintTimer;
};

} // namespace crate::library
