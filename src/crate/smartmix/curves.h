#pragma once

namespace crate::curves {

/// The shapes of a Smart Mix transition, the same as the Android player's
/// (Crate monorepo, app/listen/android: `NativeMixAudioProcessor` and
/// `BassHandoffFilter`), so Android and Decks play a plan alike (design D8).
/// `progress` runs from 0, when the incoming track starts, to 1, when the
/// transition ends; values outside that range are clamped, and a value that
/// is not finite counts as 1, as on Android. src/test/crate/curves_test.cpp
/// checks every function against a table generated from Android's code.

/// Which side of the transition a deck plays.
enum class Role {
    Outgoing,
    Incoming,
};

/// The two gains of an equal-power crossfade, whose squares add up to 1.
struct EqualPowerGains {
    double outgoing = 1.0;
    double incoming = 0.0;
};

/// `cos` and `sin` of a quarter turn times `crossfade`; the outgoing gain is
/// exactly 0 at the end (`equalPowerOutgoing`, `equalPowerIncoming`).
EqualPowerGains equalPower(double crossfade);

/// The crossfade of the long beatmatched blend (`djBlendProgress`): it eases
/// to the middle until kDjBlendEntry, holds both tracks at full blend until
/// kDjBlendExit and eases out to the end.
double djBlend(double progress);
constexpr double kDjBlendEntry = 0.3;
constexpr double kDjBlendExit = 0.7;

/// The crossfade a curve gives at `progress`: djBlend() for `dj-blend`,
/// `progress` itself for `equal-power`.
double crossfade(double progress, bool djBlendCurve);

/// The level a plan's gain sets for a deck as a factor (`startEnvelope`):
/// Smart Mix only lowers a track, so positive gains count as 0 dB.
double stagingGain(double gainDb);

/// How far a deck is from unity towards its staging gain at `progress`
/// (`staging(stagingGain, easeIn/easeOut(progress))`): the outgoing deck
/// reaches its gain halfway through the transition, the incoming deck starts
/// at its gain and is back to unity at the end.
double stagingFactor(Role role, double progress, double stagingGain);

/// The whole gain Android applies to a deck at `progress`: its equal-power
/// gain at the curve's crossfade times its staging factor
/// (`envelopeFactorForNextFrame`).
double transitionGain(Role role, double progress, double gainDb, bool djBlendCurve);

/// The bass handoff (`BassHandoffFilter`): how much a deck's lows are cut,
/// in dB (0 or below), at `progress`. The basses swap in kBassSwapFraction of
/// the transition centred on `handoffProgress`, along half a cosine: the
/// incoming deck starts at kBassCutDb and the outgoing one ends there.
double bassCutDb(Role role, double progress, double handoffProgress);
constexpr double kBassCutDb = -24.0;
constexpr double kBassSwapFraction = 0.125;
/// Android cuts the lows with a low shelf at this frequency.
constexpr double kBassShelfHz = 150.0;

/// After a beatmatch the incoming deck goes back to its own tempo along a
/// straight line over this time (`CrateNativePlaybackService.startTempoRestore`,
/// which steps along it every 500 ms; the director at every tick).
constexpr double kTempoRestoreMs = 4000.0;
/// The tempo at `elapsedMs` of that restore, from `fromRate` to `toRate`.
double tempoRestore(double fromRate, double toRate, double elapsedMs);

double gainFromDb(double gainDb);
double dbFromGain(double gain);

} // namespace crate::curves
