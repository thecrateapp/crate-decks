#pragma once

#include <QString>

class EffectsManager;

namespace crate::eqmap {

/// The controls of a deck's mixing EQ, the first effect of its equalizer
/// rack, as F0 Q6 confirmed them: src/test/crate/eqmap_test.cpp drives the
/// default EQ ("Biquad Full Kill Equalizer") of a deck through the engine
/// with a 60 Hz and a 5 kHz sine and measures what each control does
/// (results in the implementation plan, F0 Q6).
///
/// Every mixing EQ that ships declares its parameters in the same order
/// (`EqualizerUtil::createCommonParameters`), so the map holds for all of
/// them; only the depth of a cut differs.

inline QString effectGroup(const QString& deckGroup) {
    return QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deckGroup);
}

/// The gain knobs: a factor of the band's level, 1 neutral, 0 the deepest cut
/// and up to 4 of boost.
inline const QString kLowGain = QStringLiteral("parameter1");
inline const QString kMidGain = QStringLiteral("parameter2");
inline const QString kHighGain = QStringLiteral("parameter3");
/// The kill buttons: 1 removes the band whatever its knob says.
inline const QString kLowKill = QStringLiteral("button_parameter1");
inline const QString kMidKill = QStringLiteral("button_parameter2");
inline const QString kHighKill = QStringLiteral("button_parameter3");

constexpr double kNeutralGain = 1.0;

/// The low knob (kLowGain) that cuts a deck's lows by `cutDb` (0 or below)
/// on the default EQ, measured at 60 Hz. Down to -12 dB the knob is the
/// band's gain; below its quarter the EQ blends into a full kill, and the
/// knob comes from the measured curve.
double lowGainForCutDb(double cutDb);
/// The cut, in dB, of a low knob value: the inverse of lowGainForCutDb().
double cutDbForLowGain(double gain);

/// Whether the deck's mixing EQ is the default one, loaded and on: the EQ
/// this map was measured on. With the EQs bypassed in the preferences, no EQ
/// or another one, a bass handoff would not cut the lows as planned.
bool isDefaultEqOn(const EffectsManager& effects, const QString& deckGroup);

} // namespace crate::eqmap
