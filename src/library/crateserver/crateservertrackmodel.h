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

class QAction;

namespace crate::library {

class CrateLoadCoordinator;
struct CrateTrackRef;

/// The tracks of a Crate container, or every track Decks has synced from a
/// server, for upstream's track table (plan F3.C-PR2).
///
/// Its own table model on purpose, not a BaseSqlTableModel: upstream's
/// BaseTrackTableModel makes data() and delegateForColumn() final, so a
/// subclass cannot hand the Crate delegates their roles (Camelot, profile
/// quality, cache state). The rows are read, filtered and sorted on a worker
/// thread and swapped in at once: the main thread never waits for SQLite
/// (design §9).
///
/// Read-only. A track whose file is in the audio cache is a track of upstream's
/// library (getTrack(), getTrackId(), dragging); any other loads into a deck
/// through the asynchronous hook (requestTrackLoad(), plan F4.C-PR3), which
/// downloads it first, and the Cache column shows its download. "Add to Auto
/// DJ" takes both (getTrackIdsForAutoDJ()): an uncached track gets a library
/// track for its future file, which Auto DJ waits for. The track menu adds
/// "Download" and "Remove from cache".
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
        /// Whether the user likes the track on Crate (plan F7.C-PR2).
        kLikedColumn,
        kColumnCount,
    };

    enum class Source {
        /// One container, in its order, with its positions.
        Container,
        /// Every track of a server that Decks has synced.
        Library,
        /// Rows given with setRows(), such as the recommendations of F5:
        /// filtered and sorted in memory, never read from the tables.
        Memory,
    };

    /// Field names, as TrackModel::kHeaderNameRole gives them: upstream's for
    /// the columns upstream has, so the skin styles them as its own.
    static const QString kEnergyField;
    static const QString kProfileField;
    static const QString kCacheField;
    static const QString kLikedField;

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
    /// Decimals of the BPM column (upstream's library preference).
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
    bool canRequestTrackLoad(const QModelIndex& index) const override;
    bool requestTrackLoad(const QModelIndex& index, const QString& group, bool play) override;
    QList<QAction*> trackMenuActions(const QModelIndexList& indices, QObject* pParent) override;
    QUrl getTrackUrl(const QModelIndex& index) const override;
    QString getTrackLocation(const QModelIndex& index) const override;
    TrackId getTrackId(const QModelIndex& index) const override;
    QList<TrackId> getTrackIdsForAutoDJ(const QModelIndexList& indices) override;
    CoverInfo getCoverInfo(const QModelIndex& index) const override;
    const QVector<int> getTrackRows(TrackId trackId) const override;
    void search(const QString& searchText) override;
    const QString currentSearch() const override;
    bool isColumnInternal(int column) override;
    bool isColumnHiddenByDefault(int column) override;
    bool isColumnSortable(int column) const override;
    QAbstractItemDelegate* delegateForColumn(const int column, QObject* pParent) override;
    Capabilities getCapabilities() const override;
    void removeTracks(const QModelIndexList& indices) override;
    void moveTrack(const QModelIndex& sourceIndex, const QModelIndex& destIndex) override;
    int addTracks(const QModelIndex& index, const QList<QString>& locations) override;
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

    /// Source::Memory: shows `rows` of the server `serverId`, in their order
    /// until the user sorts them. `key` names them for modelKey(), such as
    /// the track they are recommended for.
    void setRows(const QString& serverId, const QString& key, QVector<Row> rows);

    /// More entries for the track menu of the selected rows, after Download
    /// and Remove from cache (such as Radio's "More like this"): the rows'
    /// tracks and their "Artist - Title", in the same order.
    using MenuActions = std::function<QList<QAction*>(const QList<CrateTrackRef>& tracks,
            const QStringList& labels,
            QObject* pParent)>;
    void setMenuActions(MenuActions actions) {
        m_menuActions = std::move(actions);
    }

    /// Whether the user likes a track, for the Liked column; without one,
    /// the column is empty.
    using LikedOf = std::function<bool(const QString& serverId, const QString& entityUid)>;
    void setLikedOf(LikedOf likedOf) {
        m_likedOf = std::move(likedOf);
    }
    /// The likes of `serverId` changed: the Liked column repaints.
    void likesChanged(const QString& serverId);

    /// The edits of a Crate playlist shown here (plan F7.C-PR3). With one,
    /// the track table can remove its rows, reorder them by dragging (while
    /// sorted by position and not filtered) and take dropped tracks.
    struct PlaylistEditor {
        /// Each row's index in the playlist, from 0, and its track.
        std::function<void(const QList<std::pair<int, QString>>& rows)> remove;
        /// Every track of the playlist, in the new order.
        std::function<void(const QStringList& entityUids)> reorder;
        /// The tracks of these files go at the end.
        std::function<void(const QStringList& locations)> add;
    };
    /// nullopt makes the rows read-only again.
    void setPlaylistEditor(std::optional<PlaylistEditor> editor);
    bool isPlaylistEditable() const {
        return m_playlistEditor.has_value();
    }

  signals:
    /// New rows are in.
    void loaded();

  protected:
    /// Source::Memory: the TrackModel settings of a subclass with its own
    /// columns, so that its header state is not another model's.
    CrateServerTrackModel(const QSqlDatabase& database,
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            const char* settingsNamespace,
            QObject* pParent);

    /// The row shown at `row`; nullptr out of range.
    const Row* rowAt(int row) const;
    /// Source::Memory: whether `row` is shown. A subclass filters with it;
    /// call refilter() when the answer changes.
    virtual bool acceptsRow(const Row& row) const {
        Q_UNUSED(row);
        return true;
    }
    /// Source::Memory: the order of a column of a subclass; the base
    /// columns sort by their values.
    virtual std::optional<bool> lessThan(int column, const Row& left, const Row& right) const {
        Q_UNUSED(column);
        Q_UNUSED(left);
        Q_UNUSED(right);
        return std::nullopt;
    }
    void refilter();
    int sortColumn() const {
        return m_sortColumn;
    }
    Qt::SortOrder sortOrder() const {
        return m_sortOrder;
    }

  private:
    CrateServerTrackModel(Source source,
            const QSqlDatabase& database,
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            const char* settingsNamespace,
            QObject* pParent);

    void load();
    /// Whether a drag in the table may reorder the playlist.
    bool canReorder() const;
    /// Source::Memory: filters and sorts m_memoryRows into the rows shown.
    void loadFromMemory();
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
    /// Source::Memory: every row given, in its order.
    QVector<Row> m_memoryRows;
    QString m_memoryKey;
    MenuActions m_menuActions;
    LikedOf m_likedOf;
    std::optional<PlaylistEditor> m_playlistEditor;
    /// A local reorder is sent once the view has made all its moves.
    bool m_reorderPending = false;
    /// modelKey() of the rows shown.
    QString m_rowsKey;
    QPointer<CrateLoadCoordinator> m_pCoordinator;
    /// Tracks whose cache state changed since the last repaint, by
    /// "<server>\n<uid>"; repainted at most every kRepaintIntervalMs.
    QSet<QString> m_changedTracks;
    QTimer m_repaintTimer;
};

} // namespace crate::library
