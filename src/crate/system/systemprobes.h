#pragma once

#include "crate/system/systemcheck.h"

namespace crate {

class CratePreferences;

namespace system {

class PowerHelper;

/// The system check's readings in the running application: the OpenGL
/// renderer that the waveforms found, the monitor of the main window (Qt),
/// the engine's audio controls ([App],audio_latency_usage and
/// audio_latency_overload_count), the discrete GPU preference of
/// `pPreferences` and whether `pPowerHelper` can install. Anything missing
/// reads as unknown.
Probes applicationProbes(const CratePreferences* pPreferences, const PowerHelper* pPowerHelper);

/// The renderer of `openGlVersion`, as WaveformWidgetFactory writes it:
/// "4.6 (Core Profile) Mesa 25.2.6 (Mesa Intel(R) Arc(TM) Graphics (MTL))"
/// gives "Mesa Intel(R) Arc(TM) Graphics (MTL)". Empty without one.
QString rendererOf(const QString& openGlVersion);

} // namespace system
} // namespace crate
