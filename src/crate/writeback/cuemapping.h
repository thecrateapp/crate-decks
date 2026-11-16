#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>
#include <optional>

#include "track/track_decl.h"

namespace crate::writeback {

/// The cue types of the Decks contract (S7, `cue_type`).
namespace cuetype {
inline const QString kMain = QStringLiteral("main");
inline const QString kIntro = QStringLiteral("intro");
inline const QString kOutro = QStringLiteral("outro");
inline const QString kHotcue = QStringLiteral("hotcue");
inline const QString kLoop = QStringLiteral("loop");
inline const QString kJump = QStringLiteral("jump");
} // namespace cuetype

/// Hot cue slots of the contract: upstream's hot cue indexes 0 to 35,
/// shared by hot cues, saved loops and saved jumps.
inline constexpr int kSyncedHotcueSlots = 36;
/// The longest label the server takes.
inline constexpr int kSyncedLabelMaxLength = 200;
/// The latest time the server takes, 24 h in milliseconds.
inline constexpr double kSyncedCueMaxMs = 86400000.0;

/// One cue as the server keeps it (plan F8.C-PR1), in the canonical form
/// both sides compare: times rounded to 0.001 ms, as the server rounds them,
/// the colour as lowercase `#rrggbb` and a blank label as none.
struct SyncedCue {
    QString type;
    int slot = 0;
    std::optional<double> startMs;
    std::optional<double> endMs;
    /// Empty for none.
    QString label;
    /// `#rrggbb`, lowercase; empty for none.
    QString color;

    bool operator==(const SyncedCue& other) const = default;
};
using SyncedCues = QList<SyncedCue>;

/// The track's cues the contract can express, in canonical order: main,
/// intro and outro, then the hot cue slots in order. Needs the track's
/// sample rate; without it the list is empty.
///
/// Upstream cues it leaves out, because the contract has no place for
/// them: N60dBSound (upstream's silence analysis, never the user's), the
/// unused Beat type, the temporary loop (a loop without a hot cue), hot
/// cue indexes above 35, cues before the start of the file or after 24 h,
/// and any cue that breaks a rule of its type (such as a loop that ends
/// before it starts). A second cue of a type or slot is left out too.
/// Colours and labels are kept only for hot cues, loops and jumps:
/// upstream shows none for main, intro and outro.
SyncedCues syncedCuesOf(const TrackPointer& pTrack);

/// Replaces the track's cues with `cues`, in one change of its cue list:
/// every hot cue, loop and jump of slots 0-35 goes, and main, intro and
/// outro go only when `cues` has one of that type, since Decks and upstream
/// place those by themselves (Crate's profile and upstream's silence
/// analysis). The temporary loop, N60dBSound and hot cues above slot 35
/// stay. Returns false, changing nothing, without the sample rate.
bool applySyncedCues(const TrackPointer& pTrack, const SyncedCues& cues);

/// The `cues` of a cue set as the server sends them, in canonical order.
/// A cue that is not one is left out with a log line.
SyncedCues syncedCuesFromJson(const QJsonArray& json);
/// `cues` for a cue set `PUT`.
QJsonArray toJson(const SyncedCues& cues);

/// Main, intro and outro: the cues Decks and upstream place by themselves.
SyncedCues automaticCues(const SyncedCues& cues);
/// The hot cues, loops and jumps of `cues` with the automatic cues of
/// `automatic`.
SyncedCues withAutomaticCues(const SyncedCues& cues, const SyncedCues& automatic);

} // namespace crate::writeback
