#include "crate/system/systemcheck.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <utility>

#include "moc_systemcheck.cpp"

namespace crate::system {

namespace {

QString translate(const char* text) {
    return QCoreApplication::translate("crate::SystemCheck", text);
}

QString readText(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

/// The integer in `path`, or `fallback`.
qint64 readNumber(const QString& path, qint64 fallback = -1) {
    bool ok = false;
    const qint64 value = readText(path).toLongLong(&ok);
    return ok ? value : fallback;
}

/// The sustained (long_term) limit of the RAPL zone `zone`, in watts.
int readPl1Watts(const QDir& powercap, const QString& zone) {
    const QDir dir(powercap.filePath(zone));
    for (int i = 0; i < 4; ++i) {
        const QString constraint = QStringLiteral("constraint_%1_").arg(i);
        if (readText(dir.filePath(constraint + QStringLiteral("name"))) ==
                QLatin1String("long_term")) {
            const qint64 microwatts =
                    readNumber(dir.filePath(constraint + QStringLiteral("power_limit_uw")));
            return microwatts > 0 ? static_cast<int>(std::lround(microwatts / 1e6)) : -1;
        }
    }
    return -1;
}

/// The numeric entries of `dir`, sorted.
QList<int> numericEntries(const QDir& dir) {
    QList<int> numbers;
    const QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        bool ok = false;
        const int number = entry.toInt(&ok);
        if (ok) {
            numbers.append(number);
        }
    }
    std::sort(numbers.begin(), numbers.end());
    return numbers;
}

/// The DRM clients of `pid`, from its fdinfo files.
GpuClientTimes readGpuClientTimes(const QDir& proc, int pid) {
    static const QRegularExpression kEngine(
            QStringLiteral("^drm-engine-([a-z0-9-]+):\\s+(\\d+)\\s+ns$"));
    static const QRegularExpression kCapacity(
            QStringLiteral("^drm-engine-capacity-([a-z0-9-]+):\\s+(\\d+)$"));
    GpuClientTimes times;
    const QDir fdinfo(proc.filePath(QStringLiteral("%1/fdinfo").arg(pid)));
    // The same client can be open through several descriptors
    QHash<QString, QHash<QString, qint64>> clients;
    const QList<int> fds = numericEntries(fdinfo);
    for (int fd : fds) {
        const QStringList lines = readText(fdinfo.filePath(QString::number(fd)))
                                          .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QString client;
        QHash<QString, qint64> engines;
        for (const QString& line : lines) {
            if (line.startsWith(QLatin1String("drm-client-id:"))) {
                client = line.mid(line.indexOf(QLatin1Char(':')) + 1).trimmed();
            } else if (line.startsWith(QLatin1String("drm-pdev:"))) {
                times.pdev = line.mid(line.indexOf(QLatin1Char(':')) + 1).trimmed();
            } else if (const QRegularExpressionMatch capacity = kCapacity.match(line);
                    capacity.hasMatch()) {
                times.capacity.insert(capacity.captured(1), capacity.captured(2).toInt());
            } else if (const QRegularExpressionMatch engine = kEngine.match(line);
                    engine.hasMatch()) {
                engines.insert(engine.captured(1), engine.captured(2).toLongLong());
            }
        }
        if (!client.isEmpty() && !engines.isEmpty()) {
            clients.insert(client, engines);
        }
    }
    if (clients.isEmpty()) {
        return GpuClientTimes();
    }
    times.pid = pid;
    for (const auto& engines : std::as_const(clients)) {
        for (auto it = engines.constBegin(); it != engines.constEnd(); ++it) {
            times.engineNs[it.key()] += it.value();
        }
    }
    return times;
}

QString percent(double fraction) {
    return QString::number(std::lround(fraction * 100));
}

/// The GPU of `gpus` that `renderer` names, or nullptr when it cannot tell
/// (no match, or two GPUs of that vendor).
const GpuInfo* rendererGpu(const QList<GpuInfo>& gpus, const QString& renderer) {
    QString vendor;
    if (renderer.contains(QLatin1String("NVIDIA"), Qt::CaseInsensitive) ||
            renderer.contains(QLatin1String("GeForce"), Qt::CaseInsensitive)) {
        vendor = QStringLiteral("NVIDIA");
    } else if (renderer.contains(QLatin1String("Intel"), Qt::CaseInsensitive)) {
        vendor = QStringLiteral("Intel");
    } else if (renderer.contains(QLatin1String("AMD"), Qt::CaseInsensitive) ||
            renderer.contains(QLatin1String("Radeon"), Qt::CaseInsensitive)) {
        vendor = QStringLiteral("AMD");
    } else {
        return nullptr;
    }
    const GpuInfo* pFound = nullptr;
    for (const GpuInfo& gpu : gpus) {
        if (gpu.vendorName() == vendor) {
            if (pFound) {
                return nullptr;
            }
            pFound = &gpu;
        }
    }
    return pFound;
}

bool softwareRenderer(const QString& renderer) {
    for (const char* name : {"llvmpipe", "softpipe", "SwiftShader", "Software Rasterizer"}) {
        if (renderer.contains(QLatin1String(name), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

CheckResult unavailable(const QString& id, const QString& title, const QString& detail) {
    CheckResult result;
    result.id = id;
    result.title = title;
    result.status = CheckStatus::Unavailable;
    result.detail = detail;
    return result;
}

/// The throttle reasons set in at least half of `clocks`.
QStringList steadyThrottleReasons(const QList<GpuClock>& clocks) {
    QHash<QString, int> counts;
    for (const GpuClock& clock : clocks) {
        for (const QString& reason : clock.throttleReasons) {
            ++counts[reason];
        }
    }
    QStringList reasons;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (2 * it.value() >= clocks.size()) {
            reasons.append(it.key());
        }
    }
    reasons.sort();
    return reasons;
}

bool isPowerReason(const QString& reason) {
    return reason == QLatin1String("pl1") || reason == QLatin1String("pl2");
}

bool isThermalReason(const QString& reason) {
    return reason == QLatin1String("thermal") || reason == QLatin1String("ratl") ||
            reason == QLatin1String("prochot") || reason == QLatin1String("vr_thermalert");
}

CheckResult clockResult(const Measurement& m) {
    const QString id = QStringLiteral("gpuClock");
    const QString title = translate("Integrated GPU clock");
    if (!m.isLinux) {
        return unavailable(id, title, translate("Not available on this system."));
    }
    QList<GpuClock> clocks;
    for (const GpuClock& clock : m.clocks) {
        if (clock.isValid()) {
            clocks.append(clock);
        }
    }
    if (clocks.isEmpty()) {
        return unavailable(id, title, translate("This GPU does not report its clock."));
    }
    CheckResult result;
    result.id = id;
    result.title = title;
    result.status = CheckStatus::Ok;
    double sum = 0;
    for (const GpuClock& clock : std::as_const(clocks)) {
        sum += clock.actualMhz;
    }
    const int average = static_cast<int>(std::lround(sum / clocks.size()));
    const int maximum = clocks.last().maxMhz;
    const double ratio = static_cast<double>(average) / maximum;
    // Busy: not idle (rc6) for at least half of the check
    double busy = -1;
    if (clocks.first().idleMs >= 0 && clocks.last().idleMs >= 0 && m.elapsedMs > 0) {
        const double idle = static_cast<double>(clocks.last().idleMs - clocks.first().idleMs) /
                static_cast<double>(m.elapsedMs);
        busy = std::clamp(1.0 - idle, 0.0, 1.0);
    }
    const QStringList reasons = steadyThrottleReasons(clocks);
    const bool power = std::any_of(reasons.cbegin(), reasons.cend(), isPowerReason);
    const bool thermal = std::any_of(reasons.cbegin(), reasons.cend(), isThermalReason);
    const QString clock = translate("%1 of %2 MHz on average")
                                  .arg(QString::number(average), QString::number(maximum));
    if (busy >= 0 && busy < kBusyFraction) {
        result.detail = clock + QStringLiteral(" · ") +
                translate(
                        "The GPU was mostly idle during the check: run it again while "
                        "a track plays.");
        return result;
    }
    const QString load = busy >= 0
            ? QStringLiteral(" · ") + translate("%1 % busy").arg(percent(busy))
            : QString();
    result.detail = clock + load;
    if (ratio >= kClockRatioWarning && !power && !thermal) {
        return result;
    }
    result.status = CheckStatus::Warning;
    QStringList why;
    if (power) {
        why.append(translate("held back by the processor's power limit (PL1)"));
    }
    if (thermal) {
        why.append(translate("held back by temperature"));
    }
    if (!why.isEmpty()) {
        result.detail += QStringLiteral(" · ") + why.join(QStringLiteral(", "));
    }
    if (thermal) {
        result.advice = translate(
                "Keep the laptop's vents clear and other heavy work off while you play.");
    } else if (power) {
        result.advice = translate("Raise the power limit on AC (see Power limit).");
    } else {
        result.advice = translate(
                "The integrated GPU runs far below its clock: see the "
                "power limit and the compositor.");
    }
    return result;
}

CheckResult powerResult(const Measurement& m) {
    const QString id = QStringLiteral("powerLimit");
    const QString title = translate("Power limit");
    if (!m.isLinux) {
        return unavailable(id, title, translate("Not available on this system."));
    }
    if (m.power.isEmpty() || m.power.last().pl1Watts() < 0) {
        return unavailable(id, title, translate("This processor does not report a power limit."));
    }
    const PowerReading& last = m.power.last();
    const int watts = last.pl1Watts();
    CheckResult result;
    result.id = id;
    result.title = title;
    result.status = CheckStatus::Ok;
    if (!last.onAc) {
        result.detail = translate("On battery: %1 W sustained.").arg(QString::number(watts));
        return result;
    }
    result.detail = translate("%1 W sustained on AC.").arg(QString::number(watts));
    if (watts < kPl1WarningWatts) {
        result.status = CheckStatus::Warning;
        result.detail = translate(
                "The firmware holds the processor at %1 W on AC, which "
                "throttles the integrated GPU.")
                                .arg(QString::number(watts));
        if (m.powerHelperInstalled) {
            result.advice = translate(
                    "The power limit helper is installed but the limit is "
                    "still low: check /etc/default/crate-power-limit.");
        } else if (m.powerHelperAvailable && last.mmioPl1Watts > 0) {
            result.action = CheckAction::PowerHelper;
            result.advice = translate(
                    "Install the power limit helper: it raises the limit to 45 W on AC and "
                    "restores %1 W on battery. It needs your administrator password, and "
                    "the laptop runs warmer.")
                                    .arg(QString::number(watts));
        } else {
            result.advice = translate(
                    "Raise the sustained power limit (PL1) in the firmware settings, if "
                    "they offer it.");
        }
        return result;
    }
    // With the helper on, the package must not stay hot
    int coolest = -1;
    for (const PowerReading& reading : m.power) {
        if (reading.packageCelsius >= 0) {
            coolest = coolest < 0 ? reading.packageCelsius
                                  : std::min(coolest, reading.packageCelsius);
        }
    }
    if (m.powerHelperInstalled && coolest >= kHotCelsius) {
        result.status = CheckStatus::Warning;
        result.detail += QStringLiteral(" · ") +
                translate("The processor stays at %1 °C.").arg(QString::number(coolest));
        result.advice = translate(
                "Lower CRATE_PL1_AC_WATTS in /etc/default/crate-power-limit, or keep "
                "other heavy work off while you play.");
    }
    return result;
}

CheckResult compositorResult(const Measurement& m) {
    const QString id = QStringLiteral("compositor");
    const QString title = translate("Desktop compositor");
    if (!m.isLinux) {
        return unavailable(id, title, translate("Not available on this system."));
    }
    if (!m.compositorFirst.isValid() || !m.compositorLast.isValid() ||
            m.compositorFirst.pid != m.compositorLast.pid) {
        return unavailable(id, title, translate("No known compositor uses the GPU."));
    }
    const double load = gpuLoad(m.compositorFirst, m.compositorLast, m.elapsedMs * 1000000);
    CheckResult result;
    result.id = id;
    result.title = title;
    result.status = CheckStatus::Ok;
    result.detail = translate("%1 kept its GPU %2 % busy.")
                            .arg(m.compositorLast.name, percent(load));
    if (load >= kCompositorLoadWarning) {
        result.status = CheckStatus::Warning;
        result.advice = translate(
                "Its GPU has little left for the waveforms: turn off blur, shadows and "
                "animations, or lower the refresh rate of the other monitors.");
    }
    return result;
}

/// Whether Decks draws on the integrated GPU, as far as the renderer tells.
bool drawsOnIntegrated(const Measurement& m) {
    const GpuInfo* pGpu = m.render.known ? rendererGpu(m.gpus, m.render.renderer) : nullptr;
    return pGpu && pGpu->integrated;
}

CheckResult displayResult(const Measurement& m) {
    const QString id = QStringLiteral("display");
    const QString title = translate("Monitor");
    if (!m.display.known) {
        return unavailable(id, title, translate("Decks' window is not on a monitor yet."));
    }
    CheckResult result;
    result.id = id;
    result.title = title;
    result.status = CheckStatus::Ok;
    result.detail = translate("%1 × %2 at %3 Hz")
                            .arg(QString::number(m.display.width),
                                    QString::number(m.display.height),
                                    QString::number(std::lround(m.display.refreshHz)));
    const bool demanding = m.display.refreshHz > kHighRefreshHz &&
            m.display.width * m.display.height >= kHighResolutionPixels;
    if (demanding && drawsOnIntegrated(m)) {
        result.status = CheckStatus::Warning;
        result.advice = isHybrid(m.gpus)
                ? translate(
                          "A high refresh rate at this resolution is a lot for the "
                          "integrated GPU: set this monitor to 60 Hz while you play, or "
                          "draw Decks with the discrete GPU.")
                : translate(
                          "A high refresh rate at this resolution is a lot for the "
                          "integrated GPU: set this monitor to 60 Hz while you play.");
    }
    return result;
}

CheckResult audioResult(const Measurement& m) {
    const QString id = QStringLiteral("audio");
    const QString title = translate("Audio");
    QList<AudioInfo> known;
    for (const AudioInfo& audio : m.audio) {
        if (audio.known) {
            known.append(audio);
        }
    }
    if (known.isEmpty()) {
        return unavailable(id, title, translate("The sound output is not running."));
    }
    double usage = 0;
    for (const AudioInfo& audio : std::as_const(known)) {
        usage = std::max(usage, audio.usagePercent);
    }
    const int dropouts = std::max(0, known.last().overloadCount - known.first().overloadCount);
    CheckResult result;
    result.id = id;
    result.title = title;
    result.status = CheckStatus::Ok;
    result.detail = translate("Up to %1 % of the audio buffer used, %2 dropouts during the check.")
                            .arg(QString::number(std::lround(usage)), QString::number(dropouts));
    if (dropouts > 0 || usage >= kAudioUsageWarning) {
        result.status = CheckStatus::Warning;
        result.advice = translate(
                "The buffer is too short for this load: raise the audio buffer in "
                "Preferences · Sound hardware.");
    }
    return result;
}

CheckResult graphicsResult(const Measurement& m, bool integratedFallsShort) {
    CheckResult result;
    result.id = QStringLiteral("graphics");
    result.title = translate("Graphics");
    QStringList gpus;
    for (const GpuInfo& gpu : m.gpus) {
        gpus.append(QStringLiteral("%1 (%2)").arg(gpu.vendorName(), gpu.driver));
    }
    if (!m.render.known && gpus.isEmpty()) {
        result.status = CheckStatus::Unavailable;
        result.detail = translate("The graphics are not known yet.");
        return result;
    }
    QStringList detail;
    if (!gpus.isEmpty()) {
        detail.append(gpus.join(QStringLiteral(" + ")));
    }
    if (m.render.known && !m.render.renderer.isEmpty()) {
        detail.append(translate("Decks draws with %1").arg(m.render.renderer));
    }
    result.detail = detail.join(QStringLiteral(" · "));
    result.status = CheckStatus::Ok;
    if (m.render.known && !m.render.usable) {
        result.status = CheckStatus::Blocking;
        result.advice = translate(
                "There is no OpenGL 2.1 or OpenGL ES 2.0: the waveforms "
                "cannot draw. Install or update the graphics driver.");
        return result;
    }
    if (m.render.known && softwareRenderer(m.render.renderer)) {
        result.status = CheckStatus::Warning;
        result.advice = translate(
                "Decks draws without the GPU: install the graphics "
                "driver.");
        return result;
    }
    const GpuInfo* pDiscrete = discreteGpu(m.gpus);
    if (!pDiscrete || !drawsOnIntegrated(m) || !integratedFallsShort) {
        return result;
    }
    result.status = CheckStatus::Warning;
    if (m.discreteGpuPreferred) {
        result.advice = translate(
                "\"Draw with the discrete GPU\" is on, but Decks still "
                "draws with the integrated GPU: check that the %1 driver "
                "can offload (PRIME).")
                                .arg(pDiscrete->vendorName());
    } else {
        result.action = CheckAction::DiscreteGpu;
        result.advice = translate(
                "The integrated GPU falls short: draw Decks with the "
                "discrete %1 GPU. It applies at the next start and keeps "
                "the discrete GPU awake, which costs battery.")
                                .arg(pDiscrete->vendorName());
    }
    return result;
}

} // namespace

int PowerReading::pl1Watts() const {
    if (mmioPl1Watts > 0 && msrPl1Watts > 0) {
        return std::min(mmioPl1Watts, msrPl1Watts);
    }
    return std::max(mmioPl1Watts, msrPl1Watts);
}

GpuClock readGpuClock(const QString& root, const QString& card) {
    GpuClock clock;
    const QDir cardDir(QDir(root).filePath(QStringLiteral("sys/class/drm/") + card));
    // i915
    const QDir gt(cardDir.filePath(QStringLiteral("gt/gt0")));
    if (gt.exists(QStringLiteral("rps_act_freq_mhz"))) {
        clock.actualMhz = static_cast<int>(
                readNumber(gt.filePath(QStringLiteral("rps_act_freq_mhz")), 0));
        clock.maxMhz = static_cast<int>(
                readNumber(gt.filePath(QStringLiteral("rps_RP0_freq_mhz")), 0));
        clock.idleMs = readNumber(gt.filePath(QStringLiteral("rc6_residency_ms")));
        const QStringList files = gt.entryList({QStringLiteral("throttle_reason_*")}, QDir::Files);
        for (const QString& file : files) {
            const QString reason = file.mid(qstrlen("throttle_reason_"));
            if (reason != QLatin1String("status") && readNumber(gt.filePath(file), 0) > 0) {
                clock.throttleReasons.append(reason);
            }
        }
        return clock;
    }
    // xe
    const QDir freq(cardDir.filePath(QStringLiteral("device/tile0/gt0/freq0")));
    if (freq.exists(QStringLiteral("act_freq"))) {
        clock.actualMhz = static_cast<int>(
                readNumber(freq.filePath(QStringLiteral("act_freq")), 0));
        clock.maxMhz = static_cast<int>(readNumber(freq.filePath(QStringLiteral("rp0_freq")), 0));
        clock.idleMs = readNumber(cardDir.filePath(
                QStringLiteral("device/tile0/gt0/gtidle/idle_residency_ms")));
        const QDir throttle(freq.filePath(QStringLiteral("throttle")));
        const QStringList files = throttle.entryList({QStringLiteral("reason_*")}, QDir::Files);
        for (const QString& file : files) {
            if (readNumber(throttle.filePath(file), 0) > 0) {
                clock.throttleReasons.append(file.mid(qstrlen("reason_")));
            }
        }
    }
    return clock;
}

PowerReading readPower(const QString& root) {
    PowerReading reading;
    const QDir sys(QDir(root).filePath(QStringLiteral("sys/class")));
    const QDir powercap(sys.filePath(QStringLiteral("powercap")));
    reading.mmioPl1Watts = readPl1Watts(powercap, QStringLiteral("intel-rapl-mmio:0"));
    reading.msrPl1Watts = readPl1Watts(powercap, QStringLiteral("intel-rapl:0"));

    const QDir supplies(sys.filePath(QStringLiteral("power_supply")));
    bool battery = false;
    bool mainsOnline = false;
    const QStringList entries = supplies.entryList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    for (const QString& supply : entries) {
        const QString type = readText(supplies.filePath(supply + QStringLiteral("/type")));
        if (type == QLatin1String("Battery")) {
            battery = true;
        } else if (type == QLatin1String("Mains") &&
                readNumber(supplies.filePath(supply + QStringLiteral("/online")), 0) == 1) {
            mainsOnline = true;
        }
    }
    reading.onAc = mainsOnline || !battery;

    const QDir thermal(sys.filePath(QStringLiteral("thermal")));
    const QStringList zones = thermal.entryList(
            {QStringLiteral("thermal_zone*")}, QDir::Dirs | QDir::System);
    for (const QString& zone : zones) {
        if (readText(thermal.filePath(zone + QStringLiteral("/type"))) ==
                QLatin1String("x86_pkg_temp")) {
            const qint64 milli = readNumber(thermal.filePath(zone + QStringLiteral("/temp")));
            if (milli > 0) {
                reading.packageCelsius = static_cast<int>(milli / 1000);
            }
            break;
        }
    }
    return reading;
}

GpuClientTimes readCompositorGpuTimes(const QString& root) {
    // /proc/<pid>/comm, at most 15 characters
    static const QStringList kCompositors = {QStringLiteral("Hyprland"),
            QStringLiteral("sway"),
            QStringLiteral("kwin_wayland"),
            QStringLiteral("kwin_x11"),
            QStringLiteral("gnome-shell"),
            QStringLiteral("mutter"),
            QStringLiteral("niri"),
            QStringLiteral("cosmic-comp"),
            QStringLiteral("weston"),
            QStringLiteral("labwc"),
            QStringLiteral("river"),
            QStringLiteral("wayfire"),
            QStringLiteral("Xorg"),
            QStringLiteral("picom")};
    const QDir proc(QDir(root).filePath(QStringLiteral("proc")));
    const QList<int> pids = numericEntries(proc);
    for (int pid : pids) {
        const QString name = readText(proc.filePath(QStringLiteral("%1/comm").arg(pid)));
        if (!kCompositors.contains(name)) {
            continue;
        }
        GpuClientTimes times = readGpuClientTimes(proc, pid);
        if (times.isValid()) {
            times.name = name;
            return times;
        }
    }
    return GpuClientTimes();
}

double gpuLoad(const GpuClientTimes& first, const GpuClientTimes& last, qint64 elapsedNs) {
    if (elapsedNs <= 0) {
        return 0;
    }
    double load = 0;
    for (auto it = last.engineNs.constBegin(); it != last.engineNs.constEnd(); ++it) {
        const qint64 busy = it.value() - first.engineNs.value(it.key(), it.value());
        const int capacity = std::max(1, last.capacity.value(it.key(), 1));
        load = std::max(load,
                static_cast<double>(busy) / (static_cast<double>(elapsedNs) * capacity));
    }
    return std::clamp(load, 0.0, 1.0);
}

QList<CheckResult> evaluate(const Measurement& measurement) {
    const CheckResult clock = clockResult(measurement);
    const CheckResult power = powerResult(measurement);
    const CheckResult compositor = compositorResult(measurement);
    const CheckResult display = displayResult(measurement);
    const CheckResult audio = audioResult(measurement);
    // The integrated GPU falls short when its clock, its compositor or the
    // monitor say so (Diego, 2026-11-15: recommend the discrete GPU then)
    const bool fallsShort = clock.status == CheckStatus::Warning ||
            compositor.status == CheckStatus::Warning || display.status == CheckStatus::Warning;
    return {graphicsResult(measurement, fallsShort), clock, power, compositor, display, audio};
}

SystemCheck::SystemCheck(Probes probes, QObject* pParent)
        : QObject(pParent),
          m_probes(std::move(probes)),
          m_durationMs(std::max(0, m_probes.durationMs)) {
    m_timer.setInterval(kSampleIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &SystemCheck::sample);
}

void SystemCheck::setDuration(int durationMs) {
    m_durationMs = std::max(0, durationMs);
}

qint64 SystemCheck::elapsedMs() const {
    return isRunning() ? m_clock.elapsed() : 0;
}

void SystemCheck::showResults(const QList<CheckResult>& results) {
    m_results = results;
    emit resultsChanged();
}

void SystemCheck::start() {
    if (isRunning()) {
        return;
    }
    m_measurement = Measurement();
#ifdef Q_OS_LINUX
    m_measurement.isLinux = true;
    m_measurement.gpus = readGpus(m_probes.root);
#else
    m_measurement.isLinux = false;
#endif
    m_clockCard.clear();
    for (const GpuInfo& gpu : std::as_const(m_measurement.gpus)) {
        if (gpu.integrated) {
            m_clockCard = gpu.card;
        }
    }
    if (m_probes.render) {
        m_measurement.render = m_probes.render();
    }
    if (m_probes.display) {
        m_measurement.display = m_probes.display();
    }
    if (m_probes.discreteGpuPreferred) {
        m_measurement.discreteGpuPreferred = m_probes.discreteGpuPreferred();
    }
    if (m_probes.powerHelperAvailable) {
        m_measurement.powerHelperAvailable = m_probes.powerHelperAvailable();
    }
    m_measurement.powerHelperInstalled = QFile::exists(QDir(m_probes.root)
                    .filePath(QStringLiteral(
                            "etc/systemd/system/crate-power-limit.service")));
    m_clock.start();
    m_timer.start();
    emit runningChanged(true);
    sample();
}

void SystemCheck::sample() {
    const qint64 elapsed = m_clock.elapsed();
    if (m_measurement.isLinux) {
        if (!m_clockCard.isEmpty()) {
            m_measurement.clocks.append(readGpuClock(m_probes.root, m_clockCard));
        }
        m_measurement.power.append(readPower(m_probes.root));
        GpuClientTimes compositor = readCompositorGpuTimes(m_probes.root);
        if (!m_measurement.compositorFirst.isValid()) {
            m_measurement.compositorFirst = compositor;
        }
        m_measurement.compositorLast = std::move(compositor);
    }
    if (m_probes.audio) {
        m_measurement.audio.append(m_probes.audio());
    }
    m_measurement.elapsedMs = elapsed;
    emit sampled(elapsed);
    if (elapsed >= m_durationMs) {
        finish();
    }
}

void SystemCheck::finish() {
    m_timer.stop();
    m_results = evaluate(m_measurement);
    emit runningChanged(false);
    emit resultsChanged();
    emit finished();
}

} // namespace crate::system
