#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <optional>

#include "audio/types.h"
#include "crate/writeback/trackdatasync.h"
#include "proto/keys.pb.h"
#include "track/beats.h"

namespace crate::writeback {

/// The sub-version of the beats of a correction applied from Crate. Crate's
/// analysis never replaces a locked grid of another sub-version.
inline const QString kCorrectionBeatsSubVersion = QStringLiteral("crate-user-correction");
/// The grid format of the contract, the mix profiles' own.
inline const QString kBeatGridFormat = QStringLiteral("delta-ms-v1");

/// A user's correction of a track's BPM, beat grid and key, as the server
/// keeps it (S7). A value that is not there was not corrected: the
/// profile's applies.
struct Correction {
    /// 30 to 300.
    std::optional<double> bpm;
    /// Absolute beat positions in whole milliseconds, strictly increasing,
    /// the first after 0 (`delta-ms-v1`); empty for none.
    QVector<qint64> beatGridMs;
    /// A Camelot code, 1A to 12B; empty for none.
    QString key;

    bool isEmpty() const {
        return !bpm && beatGridMs.isEmpty() && key.isEmpty();
    }
    bool operator==(const Correction& other) const = default;
};

/// A correction of the server; a value the contract does not allow is
/// read as not corrected.
Correction correctionFromJson(const QJsonObject& json);
/// The correction's values for a `PUT`, `null` for those not corrected.
QJsonObject toJson(const Correction& correction);

/// The track's beats as a `delta-ms-v1` grid: every beat from the start of
/// the file to the end of the track, rounded to whole milliseconds. Empty
/// without beats or a duration.
QVector<qint64> beatGridOf(const TrackPointer& pTrack);
/// The beats of a grid: a constant grid when every beat is within 2 ms of
/// one (at `bpm` when given), as the grids Decks sends are; a beat map
/// otherwise. nullptr for fewer than two beats.
mixxx::BeatsPointer beatsOfGrid(mixxx::audio::SampleRate sampleRate,
        const QVector<qint64>& beatGridMs,
        std::optional<double> bpm);
/// Puts the correction on the track: its grid (or BPM) with the BPM locked,
/// and its key as the user's. Returns false without a sample rate.
bool applyCorrection(const TrackPointer& pTrack, const Correction& correction);

/// Keeps the user's BPM, grid and key corrections of Crate tracks in sync
/// (plan F8.C-PR2, contract S7 `/api/decks/corrections`).
///
/// - **On load** a correction newer than the last one Decks synced is put
///   on the track over Crate's profile: its grid locked under
///   kCorrectionBeatsSubVersion, which Crate's analysis does not replace,
///   and its key as the user's, which it does not either.
/// - **On edit** nothing is sent by itself. A change of the grid (which
///   needs the BPM unlocked) or a key the user typed, once the track has
///   been left alone for kDebounce, is offered: correctionOffered(), and
///   offer() for the track menu's "Save correction to Crate". Changes of
///   the analysis are not edits.
/// - save() sends the stored correction with the corrected values of the
///   track; clear() sends one with no values. Both go through the Outbox,
///   and a 409 resolves as for the cues.
class CrateCorrections : public TrackDataSync {
    Q_OBJECT
  public:
    /// What the user corrected on this computer and has not saved.
    struct Offer {
        bool grid = false;
        bool key = false;
    };

    explicit CrateCorrections(Options options, QObject* pParent = nullptr);

    std::optional<Offer> offer(const QString& serverId, const QString& entityUid) const;
    /// Whether the server has a correction of the track with a value.
    bool hasCorrection(const QString& serverId, const QString& entityUid) const;
    /// Saves the offered correction to Crate. Returns false when there is
    /// nothing to save.
    bool save(const QString& serverId, const QString& entityUid);
    /// Clears the track's correction on Crate. The track keeps its grid and
    /// key here. Returns false when there is none.
    bool clear(const QString& serverId, const QString& entityUid);

  signals:
    /// The user corrected the track: Decks offers to save it to Crate.
    void correctionOffered(const QString& serverId, const QString& entityUid);

  protected:
    bool reconcile(const QString& key,
            const TrackPointer& pTrack,
            qint64 serverRevision,
            const QJsonObject& serverEntry) override;
    QJsonObject contentOf(const QJsonObject& serverEntry) const override;
    QJsonObject applyServerContent(
            const TrackPointer& pTrack, const QJsonObject& content) override;
    QJsonObject contentToResend(const QString& key, const QJsonObject& sent) override;
    void trackChanged(const QString& key, const TrackPointer& pTrack, Change change) override;
    void absorb(const QString& key, const TrackPointer& pTrack) override;

  private:
    struct Snapshot {
        mixxx::BeatsPointer pBeats;
        mixxx::track::io::key::ChromaticKey key = mixxx::track::io::key::INVALID;
    };

    void takeSnapshot(const QString& key, const TrackPointer& pTrack);
    void offerNow(const QString& key);

    QHash<QString, Snapshot> m_snapshots;
    /// Edits waiting for their debounce.
    QHash<QString, Offer> m_dirty;
    QHash<QString, Offer> m_offers;
};

} // namespace crate::writeback
