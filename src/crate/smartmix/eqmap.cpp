#include "crate/smartmix/eqmap.h"

#include <array>
#include <cmath>
#include <utility>

namespace crate::eqmap {

namespace {

/// The cut a low knob value gives a 60 Hz sine through the default EQ
/// ("Biquad Full Kill Equalizer", crossovers at 246 Hz and 2484 Hz),
/// measured by eqmap_test.cpp (F0 Q6): {knob, dB}, from neutral to the full
/// kill. The knob is not the band's gain below neutral: at a quarter it cuts
/// 8 dB, not 12, and towards 0 the EQ blends into its isolator's kill.
constexpr std::array<std::pair<double, double>, 12> kMeasuredCuts{{
        {1.0, 0.0},
        {0.5, -4.17},
        {0.354, -6.12},
        {0.25, -8.07},
        {0.2, -11.38},
        {0.15, -15.07},
        {0.125, -17.27},
        {0.1, -19.81},
        {0.075, -22.75},
        {0.05, -26.66},
        {0.025, -32.62},
        {0.0, -49.14},
}};

double interpolate(double x, double x0, double x1, double y0, double y1) {
    return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
}

} // namespace

double lowGainForCutDb(double cutDb) {
    if (cutDb >= 0.0) {
        // A boost: the knob is the band's gain there.
        return std::pow(10.0, cutDb / 20.0);
    }
    for (std::size_t i = 1; i < kMeasuredCuts.size(); ++i) {
        const auto [knob, db] = kMeasuredCuts[i];
        if (cutDb >= db) {
            const auto [previousKnob, previousDb] = kMeasuredCuts[i - 1];
            return interpolate(cutDb, previousDb, db, previousKnob, knob);
        }
    }
    return 0.0;
}

double cutDbForLowGain(double gain) {
    if (gain >= 1.0) {
        return 20.0 * std::log10(gain);
    }
    for (std::size_t i = 1; i < kMeasuredCuts.size(); ++i) {
        const auto [knob, db] = kMeasuredCuts[i];
        if (gain >= knob) {
            const auto [previousKnob, previousDb] = kMeasuredCuts[i - 1];
            return interpolate(gain, previousKnob, knob, previousDb, db);
        }
    }
    return kMeasuredCuts.back().second;
}

} // namespace crate::eqmap
