#pragma once

#include <QString>
#include <optional>

#include "crate/analysis/mixprofile.h"
#include "proto/keys.pb.h"
#include "track/beatsimporter.h"
#include "track/track_decl.h"

namespace crate::analysis {

/// Which parts of Crate's analysis replace upstream's own (plan F4.C-PR2). The
/// defaults are "beats and key from upstream": F0 Q3–Q5 measured Crate's grid
/// 30–350 ms away from upstream's and its key matching upstream's in 3 of 10
/// tracks, and the server does not announce the S8 analyzer yet.
struct Policy {
    /// Crate's beat grid, when it passes checkBeatGrid(). Otherwise upstream
    /// analyses the beats.
    bool crateBeats = false;
    /// Crate's key instead of upstream's key detector.
    bool crateKey = false;
    /// ReplayGain from the profile's integrated loudness, when the track has
    /// none yet.
    bool gainFromLufs = false;
};

/// The Intro and Outro cues Decks set from a profile, in milliseconds, so a
/// later revision can tell them from cues the user moved. ProfileStore keeps
/// it per track.
struct AppliedCues {
    struct Range {
        std::optional<double> startMs;
        std::optional<double> endMs;

        bool operator==(const Range& other) const = default;
    };

    QString profileRevision;
    std::optional<Range> intro;
    std::optional<Range> outro;

    bool operator==(const AppliedCues& other) const = default;
};

/// The outcome of the grid's quality check.
struct GridCheck {
    bool passed = false;
    /// Why it failed, for the log.
    QString reason;
    /// The quantised BPM the grid is imported at.
    double bpm = 0.0;
    /// The first beat of the imported grid: on a downbeat when the profile
    /// has one, so upstream's bar markers line up with Crate's.
    double firstBeatMs = 0.0;
    /// The largest distance of a beat from the constant grid.
    double maxDeviationMs = 0.0;
    /// How far the beats move away from the grid from start to end.
    double driftMs = 0.0;
};

/// Limits of checkBeatGrid().
inline constexpr int kGridMinBeats = 16;
inline constexpr double kGridMaxDeviationMs = 10.0;
inline constexpr double kGridMaxDriftMs = 5.0;

/// The quality check of Crate's grid (plan F4.C-PR2, point 2): a quantised
/// BPM (whole or half), at least kGridMinBeats beats, none further than
/// kGridMaxDeviationMs from a constant grid at that BPM, and a drift of at
/// most kGridMaxDriftMs over the track.
GridCheck checkBeatGrid(const MixProfile& profile);

/// Builds Crate's constant grid once the real sample rate is known, the way
/// the Serato and Rekordbox importers do, so the frames match the file
/// whatever the profile was measured on. Its sub-version is
/// `crate-mix-profile-<profileRevision>`.
class CrateBeatsImporter : public mixxx::BeatsImporter {
  public:
    CrateBeatsImporter(GridCheck grid, QString profileRevision);

    bool isEmpty() const override;
    mixxx::BeatsPointer importBeatsAndApplyTimingOffset(const QString& filePath,
            const QString& fileType,
            const mixxx::audio::StreamInfo& streamInfo) override;

    static QString subVersion(const QString& profileRevision);

  private:
    const GridCheck m_grid;
    const QString m_profileRevision;
    bool m_imported = false;
};

/// The key of a Camelot code (1A to 12B, case and spaces ignored);
/// INVALID for anything else.
mixxx::track::io::key::ChromaticKey keyFromCamelot(const QString& camelot);

/// What apply() did.
struct ImportOutcome {
    /// False when the profile is not `full`: the track is left for upstream's
    /// own analysis and nothing else is set.
    bool usedProfile = false;
    bool beatsImported = false;
    /// Why Crate's grid was not used, when the policy asked for it.
    QString beatsSkipped;
    bool keySet = false;
    bool gainSet = false;
    /// Set when Intro and Outro were (re)placed from this profile: the caller
    /// stores it (ProfileStore::recordAppliedCues) for the next revision.
    std::optional<AppliedCues> appliedCues;
};

/// Applies a profile to a track before it is loaded (design D6, plan
/// F4.C-PR2). On the main thread: Track requires it.
///
/// 1. Only `full` profiles; any other quality leaves the track to upstream.
/// 2. Beats only with Policy::crateBeats and a grid that passes
///    checkBeatGrid(), through CrateBeatsImporter, with the BPM locked. A
///    BPM the user locked is never replaced; one Decks locked for an older
///    revision is.
/// 3. The key only with Policy::crateKey.
/// 4. Intro and Outro from the profile when the track has none, or when they
///    are the ones `previous` records and the revision changed. Cues the
///    user set or moved stay. N60dBSound and the main cue stay upstream's: its
///    silence analysis still runs and keeps the Intro and Outro it finds.
/// 5. ReplayGain from the loudness only with Policy::gainFromLufs.
///
/// A track upstream has not opened yet gets its beats and cues when the file is
/// opened, at the file's sample rate.
class AnalysisImporter {
  public:
    static ImportOutcome apply(const TrackPointer& pTrack,
            const MixProfile& profile,
            const Policy& policy,
            const std::optional<AppliedCues>& previous = std::nullopt);
};

} // namespace crate::analysis
