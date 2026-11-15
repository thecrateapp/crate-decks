#include "crate/smartmix/curves.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace crate::curves {

namespace {

/// `clampUnit`: Android treats a value that is not finite as the end.
double clampUnit(double value) {
    if (!std::isfinite(value)) {
        return 1.0;
    }
    return std::clamp(value, 0.0, 1.0);
}

double smoothstep(double value) {
    const double x = std::clamp(value, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

/// 0 at the start, 1 from halfway on.
double easeIn(double progress) {
    if (progress >= 0.5) {
        return 1.0;
    }
    return 0.5 - 0.5 * std::cos(std::numbers::pi * progress / 0.5);
}

/// 1 until halfway, 0 at the end.
double easeOut(double progress) {
    if (progress <= 0.5) {
        return 1.0;
    }
    return 0.5 + 0.5 * std::cos(std::numbers::pi * (progress - 0.5) / 0.5);
}

} // namespace

EqualPowerGains equalPower(double crossfade) {
    const double safe = clampUnit(crossfade);
    EqualPowerGains gains;
    gains.outgoing = safe >= 1.0 ? 0.0 : std::cos(safe * std::numbers::pi * 0.5);
    gains.incoming = std::sin(safe * std::numbers::pi * 0.5);
    return gains;
}

double djBlend(double progress) {
    const double safe = clampUnit(progress);
    if (safe <= kDjBlendEntry) {
        return 0.5 * smoothstep(safe / kDjBlendEntry);
    }
    if (safe < kDjBlendExit) {
        return 0.5;
    }
    return 0.5 + 0.5 * smoothstep((safe - kDjBlendExit) / (1.0 - kDjBlendExit));
}

double crossfade(double progress, bool djBlendCurve) {
    return djBlendCurve ? djBlend(progress) : progress;
}

double stagingGain(double gainDb) {
    if (!std::isfinite(gainDb)) {
        return 1.0;
    }
    return gainFromDb(std::min(0.0, gainDb));
}

double stagingFactor(Role role, double progress, double stagingGain) {
    // Android passes the envelope's own progress, already within [0, 1].
    const double safe = clampUnit(progress);
    const double weight = role == Role::Outgoing ? easeIn(safe) : easeOut(safe);
    return 1.0 + (stagingGain - 1.0) * weight;
}

double transitionGain(Role role, double progress, double gainDb, bool djBlendCurve) {
    const double safe = clampUnit(progress);
    const EqualPowerGains gains = equalPower(crossfade(safe, djBlendCurve));
    const double equalPowerGain = role == Role::Outgoing ? gains.outgoing : gains.incoming;
    return equalPowerGain * stagingFactor(role, safe, stagingGain(gainDb));
}

double bassCutDb(Role role, double progress, double handoffProgress) {
    const double center = std::clamp(
            handoffProgress, kBassSwapFraction / 2.0, 1.0 - kBassSwapFraction / 2.0);
    const double position = (progress - (center - kBassSwapFraction / 2.0)) / kBassSwapFraction;
    double swapped = 0.0;
    if (position >= 1.0) {
        swapped = 1.0;
    } else if (position > 0.0) {
        swapped = 0.5 - 0.5 * std::cos(std::numbers::pi * position);
    }
    return role == Role::Incoming ? kBassCutDb * (1.0 - swapped) : kBassCutDb * swapped;
}

double tempoRestore(double fromRate, double toRate, double elapsedMs) {
    const double progress = std::clamp(elapsedMs / kTempoRestoreMs, 0.0, 1.0);
    return fromRate + (toRate - fromRate) * progress;
}

double gainFromDb(double gainDb) {
    return std::pow(10.0, gainDb / 20.0);
}

double dbFromGain(double gain) {
    return 20.0 * std::log10(gain);
}

} // namespace crate::curves
