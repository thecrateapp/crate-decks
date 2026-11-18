#pragma once

#include <QString>

namespace crate::recs {

/// How a track's key relates to the followed track's on the Camelot wheel
/// (design 14.3, GU1 mockup library-compatibles: the Harmony column). The
/// server says only same, adjacent or relative; Decks reads the direction of
/// a step and the energy boost from the two Camelot keys.
enum class Harmony {
    /// Either key is missing or is not a Camelot key.
    Unknown = 0,
    /// The same key.
    Same,
    /// One step clockwise on the same ring ("+1"): 8A to 9A, 12A to 1A.
    Up,
    /// One step anticlockwise on the same ring ("−1"): 8A to 7A, 1A to 12A.
    Down,
    /// The same number on the other ring: 8A and 8B.
    Relative,
    /// Seven steps clockwise on the same ring ("Energy +7"): 8A to 3A.
    EnergyBoost,
    /// Any other key.
    Other,
};

/// The relation of `camelot` ("9A") to `seedCamelot` ("8A"). Case and
/// surrounding blanks do not matter.
Harmony harmonyOf(const QString& seedCamelot, const QString& camelot);

/// Whether a track in that relation mixes in key: the same key, ±1, the
/// relative key or the energy boost.
bool isHarmonic(Harmony harmony);

/// What the Harmony column says: "Same key", "+1", "−1", "Relative",
/// "Energy +7", or "—" for another key; empty when unknown.
QString harmonyLabel(Harmony harmony);

/// The relation of the server's `harmonicRelationship` (same, adjacent,
/// relative, anything else), for a track whose keys Decks cannot read:
/// adjacent has no direction, so it is Up.
Harmony harmonyOfRelationship(const QString& harmonicRelationship);

/// The key `steps` away from `camelot` on its ring, or the relative key
/// with `relative`; empty if `camelot` is not a Camelot key.
QString camelotStep(const QString& camelot, int steps, bool relative = false);

} // namespace crate::recs
