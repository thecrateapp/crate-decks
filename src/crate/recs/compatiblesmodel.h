#pragma once

#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

#include "crate/client/crateresult.h"
#include "crate/library/cratelibrarystore.h"
#include "crate/recs/harmony.h"
#include "crate/recs/recsjson.h"
#include "library/crateserver/crateservertrackmodel.h"

namespace crate::recs {

/// What the Compatibles view shows of a track besides the library's columns.
struct CompatibleScores {
    /// 0 to 1, the server's ranking.
    double score = 0;
    /// same, adjacent, relative, incompatible or unknown.
    QString harmonicRelationship;
    /// The track's BPM minus the followed track's, when both are known.
    std::optional<double> bpmDelta;
    /// The track's energy level minus the followed track's, in the levels of
    /// the energy column (1 to 10).
    std::optional<int> energyDelta;
};

/// What the Compatibles view's band says of the results (GU1 mockup
/// library-compatibles, "Master key"): how many there are, how many mix in
/// key, how many are cached, and how many in each relation to the followed
/// track's key.
struct CompatiblesSummary {
    int results = 0;
    int harmonic = 0;
    int cached = 0;
    int same = 0;
    /// ±1
    int step = 0;
    int relative = 0;
    /// +7
    int energy = 0;
    /// Any other key, or none
    int other = 0;
};

/// The compatible tracks as the catalogue holds them now, with their scores.
struct StoredCompatibles {
    /// In the server's order; `position` is the rank, from 1.
    QVector<library::CrateServerTrackModel::Row> rows;
    /// By lowercase entity uid.
    QHash<QString, CompatibleScores> scores;
    /// The followed track's key as the catalogue has it; empty if unknown.
    QString seedCamelot;
};

/// Stores what an answer of `compatible` carries of each track in
/// `crateserver_tracks` (title, artist, album, duration, BPM, Camelot key,
/// energy, genre and cover; a track synced from a container keeps its
/// format, size, revisions and profile quality), in one transaction, and
/// reads the rows back with everything the catalogue knows of them. The
/// deltas are against the followed track (`seedEntityUid`) as the catalogue
/// has it; `seedBpm` stands in for its BPM when the catalogue has none. Runs
/// off the main thread. Busy when the database is.
Result<StoredCompatibles> storeCompatibles(const QSqlDatabase& database,
        const QString& serverId,
        const QString& seedEntityUid,
        const CompatibleTracks& compatibles,
        const QString& now,
        std::optional<double> seedBpm = std::nullopt,
        library::WriteStatus* pStatus = nullptr);

/// The tracks of the Compatibles view (plan F5.C-PR1, design §7.5): an
/// in-memory model over `crateserver_tracks`, so that its rows load into
/// decks, download, drag and go to Auto DJ as any Crate track, with four
/// columns more: Score, Harmony (harmonyOf() of the two Camelot keys), Δ BPM
/// and Δ energy. The rows keep
/// the server's ranking until the user sorts them. A key filter (the Camelot
/// wheel's click) shows only the tracks in one key.
class CompatiblesModel : public library::CrateServerTrackModel {
    Q_OBJECT
  public:
    enum CompatiblesColumn {
        kScoreColumn = kColumnCount,
        kHarmonicColumn,
        kBpmDeltaColumn,
        kEnergyDeltaColumn,
        kCompatiblesColumnCount,
    };

    /// A tempo this far from the followed track's, in BPM, shows in the
    /// warning tone (GU1 mockup library-compatibles).
    static constexpr double kBpmDeltaWarning = 3.0;

    static const QString kScoreField;
    static const QString kHarmonicField;
    static const QString kBpmDeltaField;
    static const QString kEnergyDeltaField;

    CompatiblesModel(const QSqlDatabase& database,
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            QObject* pParent = nullptr);

    /// Shows the compatible tracks of `seedEntityUid`.
    void setResults(const QString& serverId,
            const QString& seedEntityUid,
            const StoredCompatibles& results);
    /// Shows nothing.
    void clear();
    const QString& seedEntityUid() const {
        return m_seedEntityUid;
    }
    /// The followed track's Camelot key ("8A"); empty if unknown.
    const QString& seedCamelot() const {
        return m_seedCamelot;
    }

    /// Shows only the tracks in `camelot` ("8A"); empty shows them all.
    void setKeyFilter(const QString& camelot);
    const QString& keyFilter() const {
        return m_keyFilter;
    }
    /// The Camelot keys of every result, filtered or not, best first, once
    /// each.
    const QStringList& keysInResults() const {
        return m_keys;
    }
    /// The scores of a track of the results.
    std::optional<CompatibleScores> scoresOf(const QString& entityUid) const;
    /// Every result, filtered or not, in the server's order.
    const QVector<Row>& results() const {
        return m_results;
    }
    /// The relation of a result's key to the followed track's: from the two
    /// Camelot keys, or the server's relationship without them.
    Harmony harmonyOfRow(const Row& row) const;
    /// The counts of the band, over every result.
    CompatiblesSummary summary() const;
    /// Where the audio of the best `count` results is, best first.
    QVector<CacheCell> bestCache(int count) const;

    /// What the Harmonic column says of a relationship.
    static QString harmonicLabel(const QString& relationship);

    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override;
    int fieldIndex(const QString& fieldName) const override;
    bool isColumnSortable(int column) const override;

  protected:
    bool acceptsRow(const Row& row) const override;
    std::optional<bool> lessThan(int column, const Row& left, const Row& right) const override;

  private:
    const CompatibleScores* scoresOfRow(const Row& row) const;

    QString m_seedEntityUid;
    QString m_seedCamelot;
    QHash<QString, CompatibleScores> m_scores;
    QVector<Row> m_results;
    QStringList m_keys;
    QString m_keyFilter;
};

} // namespace crate::recs
