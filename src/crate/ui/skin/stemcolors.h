#pragma once

#include <QColor>
#include <array>

namespace crate {

/// The colours of the four stems of Crate's separation, in the sidecar's
/// order (drums, bass, other, vocals): the tokens stemDrums, stemBass,
/// stemOther and stemVocals of res/crate-design/tokens.json, through the
/// generated skin variables. For the stem files Decks writes and wherever
/// Decks shows a stem.
std::array<QColor, 4> stemColors();

} // namespace crate
