#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>

#include "crate/system/gpus.h"

namespace crate::system {

/// The integrated GPU's clock at one moment (i915 gt/gt0, or xe
/// tile0/gt0/freq0).
struct GpuClock {
    int actualMhz = 0;
    /// The highest clock the GPU can reach (RP0)
    int maxMhz = 0;
    /// Total time in its idle state (rc6), -1 if unknown
    qint64 idleMs = -1;
    /// The throttle reasons set: "pl1", "pl2", "thermal", "ratl", "prochot"...
    QStringList throttleReasons;

    bool isValid() const {
        return maxMhz > 0;
    }
};

/// The clock of `card` under `root`; invalid when the driver does not say.
GpuClock readGpuClock(const QString& root, const QString& card);

/// The processor's power limits and whether it runs on mains power.
struct PowerReading {
    /// The sustained limit (PL1) of MMIO RAPL (intel-rapl-mmio:0), in watts;
    /// -1 without it. The firmware sets this one.
    int mmioPl1Watts = -1;
    /// The sustained limit of MSR RAPL (intel-rapl:0); -1 without it.
    int msrPl1Watts = -1;
    /// On mains power, or a computer without a battery.
    bool onAc = true;
    /// The package temperature (x86_pkg_temp), -1 if unknown.
    int packageCelsius = -1;

    /// The limit that holds: the lower of both, -1 without RAPL.
    int pl1Watts() const;
};

PowerReading readPower(const QString& root);

/// GPU time of a process's DRM clients (/proc/<pid>/fdinfo, drm-engine-*).
struct GpuClientTimes {
    int pid = 0;
    QString name;
    /// The PCI slot of the GPU it draws with
    QString pdev;
    /// Busy nanoseconds by engine ("render", "copy"...), all clients summed
    QHash<QString, qint64> engineNs;
    /// Engines with several instances (drm-engine-capacity-*)
    QHash<QString, int> capacity;

    bool isValid() const {
        return pid > 0;
    }
};

/// The compositor's GPU time under `root`: the first process with a known
/// compositor name (Hyprland, sway, kwin, gnome-shell, Xorg...) and a DRM
/// client. Invalid if there is none.
GpuClientTimes readCompositorGpuTimes(const QString& root);

/// The busiest engine's load (0 to 1) between two readings `elapsedNs`
/// apart.
double gpuLoad(const GpuClientTimes& first, const GpuClientTimes& last, qint64 elapsedNs);

/// The OpenGL context Decks draws with.
struct RenderInfo {
    bool known = false;
    /// GL_RENDERER: "Mesa Intel(R) Arc(TM) Graphics (MTL)", "NVIDIA GeForce
    /// RTX 4060 Laptop GPU/PCIe/SSE2"...
    QString renderer;
    /// OpenGL 2.1 or OpenGL ES 2.0 at least
    bool usable = true;
};

/// The monitor that shows Decks' main window.
struct DisplayInfo {
    bool known = false;
    int width = 0;
    int height = 0;
    double refreshHz = 0;
};

/// The sound output, from the engine's controls.
struct AudioInfo {
    bool known = false;
    /// How much of the audio buffer the engine used, in percent
    double usagePercent = 0;
    /// Buffer underruns since the start
    int overloadCount = 0;
};

/// What the system check measured, the input of evaluate().
struct Measurement {
    bool isLinux = true;
    QList<GpuInfo> gpus;
    RenderInfo render;
    DisplayInfo display;
    bool discreteGpuPreferred = false;
    qint64 elapsedMs = 0;
    /// The integrated GPU's clock at each sample
    QList<GpuClock> clocks;
    /// The power limits at each sample
    QList<PowerReading> power;
    bool powerHelperInstalled = false;
    bool powerHelperAvailable = false;
    GpuClientTimes compositorFirst;
    GpuClientTimes compositorLast;
    QList<AudioInfo> audio;
};

enum class CheckStatus {
    Ok,
    /// Decks can play, but not as smoothly as it could: the advice says what
    /// to do
    Warning,
    /// Decks will not play smoothly on this computer as it is
    Blocking,
    /// This system does not tell, or the check does not apply
    Unavailable,
};

/// What a result offers to do.
enum class CheckAction {
    None,
    /// Turn on "Draw with the discrete GPU" (next start)
    DiscreteGpu,
    /// Install the power limit helper (pkexec)
    PowerHelper,
};

struct CheckResult {
    /// "graphics", "gpuClock", "powerLimit", "compositor", "display", "audio"
    QString id;
    QString title;
    CheckStatus status = CheckStatus::Unavailable;
    /// What was measured
    QString detail;
    /// What to do; empty when all is well
    QString advice;
    CheckAction action = CheckAction::None;
};

/// The hybrid laptop plan's thresholds (§3).
inline constexpr double kClockRatioWarning = 0.6;
inline constexpr double kBusyFraction = 0.5;
inline constexpr int kPl1WarningWatts = 35;
inline constexpr double kCompositorLoadWarning = 0.75;
inline constexpr int kHotCelsius = 95;
inline constexpr double kHighRefreshHz = 75;
inline constexpr int kHighResolutionPixels = 2560 * 1440;
inline constexpr double kAudioUsageWarning = 80;
/// "Above 75 % for 5 s"
inline constexpr int kCheckDurationMs = 5000;

/// The results of `measurement`, in the order of the plan's table: graphics,
/// integrated GPU clock, power limit, compositor, monitor, audio. Pure: the
/// tests build the measurement.
QList<CheckResult> evaluate(const Measurement& measurement);

/// Where the check reads from. The application's are applicationProbes()
/// (systemprobes.h); tests give a fake root and their own readings.
struct Probes {
    /// The root of /sys and /proc
    QString root = QStringLiteral("/");
    /// How long a run measures
    int durationMs = kCheckDurationMs;
    std::function<RenderInfo()> render;
    std::function<DisplayInfo()> display;
    std::function<AudioInfo()> audio;
    std::function<bool()> discreteGpuPreferred;
    /// Whether the power limit helper can be installed from here
    std::function<bool()> powerHelperAvailable;
};

/// The system check of the hybrid laptop plan (§3), without widgets: it reads
/// the GPUs, then samples the integrated GPU's clock, the power limits, the
/// compositor's GPU time and the audio for a few seconds, and evaluates them.
/// The first start and Preferences · Crate show it (SystemCheckView).
class SystemCheck : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
  public:
    static constexpr int kSampleIntervalMs = 250;

    explicit SystemCheck(Probes probes, QObject* pParent = nullptr);

    /// How long start() measures.
    void setDuration(int durationMs);
    int durationMs() const {
        return m_durationMs;
    }

    bool isRunning() const {
        return m_timer.isActive();
    }
    /// The results of the last run; empty before the first.
    QList<CheckResult> results() const {
        return m_results;
    }
    /// The measurement of the last run.
    const Measurement& measurement() const {
        return m_measurement;
    }
    /// The milliseconds the running check has measured.
    qint64 elapsedMs() const;

    /// Shows `results` without measuring, for the screenshots.
    void showResults(const QList<CheckResult>& results);

  public slots:
    /// Measures for durationMs() and then evaluates. Does nothing while it
    /// runs.
    void start();

  signals:
    void runningChanged(bool running);
    /// After each sample, `elapsedMs` into the run.
    void sampled(qint64 elapsedMs);
    void resultsChanged();
    void finished();

  private:
    void sample();
    void finish();

    const Probes m_probes;
    int m_durationMs;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QString m_clockCard;
    Measurement m_measurement;
    QList<CheckResult> m_results;
};

} // namespace crate::system
