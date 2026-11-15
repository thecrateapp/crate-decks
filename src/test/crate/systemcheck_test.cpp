#include "crate/system/systemcheck.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

#include "crate/system/powerhelper.h"
#include "crate/system/systemprobes.h"
#include "test/crate/fakesystemtree.h"
#include "test/mixxxtest.h"

// The system check of the hybrid laptop plan (§3): each reading against a fake
// /sys and /proc, the evaluation of what was measured, and a whole run.

namespace {

using crate::system::CheckAction;
using crate::system::CheckResult;
using crate::system::CheckStatus;
using crate::system::GpuClock;
using crate::system::GpuInfo;
using crate::system::Measurement;
using crate::system::PowerReading;
using crate::test::FakeSystemTree;

const QString kIntel = QStringLiteral("card0");

GpuInfo gpu(const char* card, const char* vendor, const char* driver, bool integrated) {
    GpuInfo info;
    info.card = QLatin1String(card);
    info.vendorId = QLatin1String(vendor);
    info.driver = QLatin1String(driver);
    info.integrated = integrated;
    return info;
}

QList<GpuInfo> xpsGpus() {
    return {gpu("card0", "0x8086", "i915", true), gpu("card1", "0x10de", "nvidia", false)};
}

GpuClock clock(int actualMhz, qint64 idleMs, QStringList reasons = {}) {
    GpuClock clock;
    clock.actualMhz = actualMhz;
    clock.maxMhz = 2350;
    clock.idleMs = idleMs;
    clock.throttleReasons = std::move(reasons);
    return clock;
}

PowerReading power(int mmioWatts, bool onAc, int celsius = 70) {
    PowerReading reading;
    reading.mmioPl1Watts = mmioWatts;
    reading.msrPl1Watts = 115;
    reading.onAc = onAc;
    reading.packageCelsius = celsius;
    return reading;
}

crate::system::GpuClientTimes compositorTimes(qint64 renderNs) {
    crate::system::GpuClientTimes times;
    times.pid = 3906;
    times.name = QStringLiteral("Hyprland");
    times.engineNs.insert(QStringLiteral("render"), renderNs);
    return times;
}

/// Diego's XPS 16 on 2026-11-14 (plan §1): Decks on the Arc, PL1 at 20 W,
/// the Arc at a fifth of its clock, Hyprland 90 % busy, 3840 × 2400 at 90 Hz.
Measurement xpsMeasurement() {
    Measurement m;
    m.gpus = xpsGpus();
    m.render.known = true;
    m.render.renderer = QStringLiteral("Mesa Intel(R) Arc(TM) Graphics (MTL)");
    m.display = {true, 3840, 2400, 90};
    m.elapsedMs = 5000;
    m.clocks = {clock(500, 1000, {QStringLiteral("pl1")}),
            clock(450, 1200, {QStringLiteral("pl1")}),
            clock(550, 1400, {QStringLiteral("pl1")})};
    m.power = {power(20, true), power(20, true)};
    m.powerHelperAvailable = true;
    m.compositorFirst = compositorTimes(0);
    m.compositorLast = compositorTimes(4'500'000'000);
    return m;
}

const CheckResult& resultOf(const QList<CheckResult>& results, const char* id) {
    for (const CheckResult& result : results) {
        if (result.id == QLatin1String(id)) {
            return result;
        }
    }
    static const CheckResult none;
    ADD_FAILURE() << "No result " << id;
    return none;
}

TEST(CrateSystemCheckTest, ReadsTheI915Clock) {
    FakeSystemTree tree;
    tree.setI915Clock(kIntel, 600, 2350, 12345);
    tree.setI915Throttle(kIntel, "pl1", true);
    tree.write(QStringLiteral("sys/class/drm/card0/gt/gt0/throttle_reason_status"), 1);
    const GpuClock clock = crate::system::readGpuClock(tree.root(), kIntel);
    ASSERT_TRUE(clock.isValid());
    EXPECT_EQ(600, clock.actualMhz);
    EXPECT_EQ(2350, clock.maxMhz);
    EXPECT_EQ(12345, clock.idleMs);
    EXPECT_EQ(QStringList{QStringLiteral("pl1")}, clock.throttleReasons);
    EXPECT_FALSE(crate::system::readGpuClock(tree.root(), QStringLiteral("card1")).isValid());
}

TEST(CrateSystemCheckTest, ReadsTheXeClock) {
    FakeSystemTree tree;
    const QString freq = QStringLiteral("sys/class/drm/card0/device/tile0/gt0/freq0/");
    tree.write(freq + QStringLiteral("act_freq"), 1200);
    tree.write(freq + QStringLiteral("rp0_freq"), 2250);
    tree.write(freq + QStringLiteral("throttle/reason_thermal"), 1);
    tree.write(freq + QStringLiteral("throttle/reason_pl1"), 0);
    tree.write(QStringLiteral("sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms"),
            777);
    const GpuClock clock = crate::system::readGpuClock(tree.root(), kIntel);
    EXPECT_EQ(1200, clock.actualMhz);
    EXPECT_EQ(2250, clock.maxMhz);
    EXPECT_EQ(777, clock.idleMs);
    EXPECT_EQ(QStringList{QStringLiteral("thermal")}, clock.throttleReasons);
}

TEST(CrateSystemCheckTest, ReadsThePowerLimits) {
    FakeSystemTree tree;
    EXPECT_EQ(-1, crate::system::readPower(tree.root()).pl1Watts());
    EXPECT_TRUE(crate::system::readPower(tree.root()).onAc) << "No battery: a desktop";

    tree.setRaplPl1("intel-rapl-mmio:0", 20);
    tree.setRaplPl1("intel-rapl:0", 115);
    tree.setOnAc(true);
    tree.setPackageTemperature(88);
    PowerReading reading = crate::system::readPower(tree.root());
    EXPECT_EQ(20, reading.mmioPl1Watts);
    EXPECT_EQ(115, reading.msrPl1Watts);
    EXPECT_EQ(20, reading.pl1Watts()) << "The lower limit holds";
    EXPECT_TRUE(reading.onAc);
    EXPECT_EQ(88, reading.packageCelsius);

    tree.setOnAc(false);
    EXPECT_FALSE(crate::system::readPower(tree.root()).onAc);
}

TEST(CrateSystemCheckTest, ReadsTheCompositorsGpuTime) {
    FakeSystemTree tree;
    EXPECT_FALSE(crate::system::readCompositorGpuTimes(tree.root()).isValid());
    // Another program's GPU time is not the compositor's
    tree.setGpuClient(100, "firefox", "0000:00:02.0", 9'000'000'000);
    tree.setGpuClient(3906, "Hyprland", "0000:00:02.0", 2'000'000'000);
    const auto times = crate::system::readCompositorGpuTimes(tree.root());
    ASSERT_TRUE(times.isValid());
    EXPECT_EQ(3906, times.pid);
    EXPECT_EQ(QStringLiteral("Hyprland"), times.name);
    EXPECT_EQ(QStringLiteral("0000:00:02.0"), times.pdev);
    EXPECT_EQ(2'000'000'000, times.engineNs.value(QStringLiteral("render")))
            << "One client through two descriptors counts once";
    EXPECT_EQ(2, times.capacity.value(QStringLiteral("video")));
    EXPECT_FALSE(times.engineNs.contains(QStringLiteral("capacity-video")));

    auto later = times;
    later.engineNs[QStringLiteral("render")] += 3'750'000'000;
    EXPECT_DOUBLE_EQ(0.75, crate::system::gpuLoad(times, later, 5'000'000'000));
    // An engine with two instances is busy for half of its capacity
    later.engineNs[QStringLiteral("video")] = 10'000'000'000;
    EXPECT_DOUBLE_EQ(1.0, crate::system::gpuLoad(times, later, 5'000'000'000));
    EXPECT_DOUBLE_EQ(0.0, crate::system::gpuLoad(times, later, 0));
}

TEST(CrateSystemCheckTest, RendererComesFromTheOpenGlVersion) {
    EXPECT_EQ(QStringLiteral("Mesa Intel(R) Arc(TM) Graphics (MTL)"),
            crate::system::rendererOf(QStringLiteral(
                    "4.6 (Core Profile) Mesa 25.2.6 (Mesa Intel(R) Arc(TM) Graphics (MTL))")));
    EXPECT_EQ(QStringLiteral("NVIDIA GeForce RTX 4060 Laptop GPU/PCIe/SSE2"),
            crate::system::rendererOf(QStringLiteral(
                    "ES OpenGL ES 3.2 NVIDIA 580.95 (NVIDIA GeForce RTX 4060 Laptop "
                    "GPU/PCIe/SSE2)")));
    EXPECT_TRUE(crate::system::rendererOf(QStringLiteral("None")).isEmpty());
}

// The XPS of the plan: every check warns, the discrete GPU and the helper are
// offered
TEST(CrateSystemCheckTest, HybridLaptopHeldBackByItsFirmware) {
    const QList<CheckResult> results = crate::system::evaluate(xpsMeasurement());
    ASSERT_EQ(6, results.size());
    EXPECT_EQ(QStringLiteral("graphics"), results[0].id);

    const CheckResult& graphics = resultOf(results, "graphics");
    EXPECT_EQ(CheckStatus::Warning, graphics.status);
    EXPECT_EQ(CheckAction::DiscreteGpu, graphics.action);
    EXPECT_TRUE(graphics.detail.contains(QLatin1String("Intel (i915) + NVIDIA (nvidia)")))
            << graphics.detail.toStdString();
    EXPECT_TRUE(graphics.advice.contains(QLatin1String("battery")));

    const CheckResult& gpuClock = resultOf(results, "gpuClock");
    EXPECT_EQ(CheckStatus::Warning, gpuClock.status);
    EXPECT_TRUE(gpuClock.detail.contains(QLatin1String("500 of 2350 MHz")))
            << gpuClock.detail.toStdString();
    EXPECT_TRUE(gpuClock.detail.contains(QLatin1String("PL1")));
    EXPECT_TRUE(gpuClock.detail.contains(QLatin1String("92 % busy")))
            << gpuClock.detail.toStdString();

    const CheckResult& powerLimit = resultOf(results, "powerLimit");
    EXPECT_EQ(CheckStatus::Warning, powerLimit.status);
    EXPECT_EQ(CheckAction::PowerHelper, powerLimit.action);
    EXPECT_TRUE(powerLimit.detail.contains(QLatin1String("20 W")));

    const CheckResult& compositor = resultOf(results, "compositor");
    EXPECT_EQ(CheckStatus::Warning, compositor.status);
    EXPECT_TRUE(compositor.detail.contains(QLatin1String("Hyprland kept its GPU 90 % busy")))
            << compositor.detail.toStdString();

    const CheckResult& display = resultOf(results, "display");
    EXPECT_EQ(CheckStatus::Warning, display.status);
    EXPECT_EQ(QStringLiteral("3840 × 2400 at 90 Hz"), display.detail);

    EXPECT_EQ(CheckStatus::Unavailable, resultOf(results, "audio").status);
}

// The discrete GPU is recommended only when the integrated one falls short
// (Diego, 2026-11-15)
TEST(CrateSystemCheckTest, IntegratedGpuThatCopesIsNotAdvisedAgainst) {
    Measurement m = xpsMeasurement();
    m.display = {true, 1920, 1200, 60};
    m.clocks = {clock(2300, 0), clock(2350, 500)};
    m.power = {power(45, true)};
    m.compositorLast = compositorTimes(1'000'000'000);
    const QList<CheckResult> results = crate::system::evaluate(m);
    for (const CheckResult& result : results) {
        EXPECT_NE(CheckStatus::Warning, result.status) << result.id.toStdString();
        EXPECT_EQ(CheckAction::None, result.action) << result.id.toStdString();
    }
    EXPECT_EQ(CheckStatus::Ok, resultOf(results, "graphics").status);
}

TEST(CrateSystemCheckTest, IdleGpuAsksToRunAgainWhilePlaying) {
    Measurement m = xpsMeasurement();
    // Idle for 4.5 of the 5 seconds
    m.clocks = {clock(400, 0, {QStringLiteral("pl1")}), clock(400, 4500, {QStringLiteral("pl1")})};
    const CheckResult result = resultOf(crate::system::evaluate(m), "gpuClock");
    EXPECT_EQ(CheckStatus::Ok, result.status);
    EXPECT_TRUE(result.detail.contains(QLatin1String("while a track plays")));
}

TEST(CrateSystemCheckTest, DiscreteGpuAlreadyOnIsNotOfferedAgain) {
    Measurement m = xpsMeasurement();
    m.discreteGpuPreferred = true;
    const CheckResult graphics = resultOf(crate::system::evaluate(m), "graphics");
    EXPECT_EQ(CheckStatus::Warning, graphics.status);
    EXPECT_EQ(CheckAction::None, graphics.action);
    EXPECT_TRUE(graphics.advice.contains(QLatin1String("is on")));

    // Drawing on the RTX: all is well
    m.render.renderer = QStringLiteral("NVIDIA GeForce RTX 4060 Laptop GPU/PCIe/SSE2");
    EXPECT_EQ(CheckStatus::Ok, resultOf(crate::system::evaluate(m), "graphics").status);
    EXPECT_EQ(CheckStatus::Ok, resultOf(crate::system::evaluate(m), "display").status)
            << "A demanding monitor is no problem for the discrete GPU";
}

TEST(CrateSystemCheckTest, PowerLimitAdviceFollowsTheHelper) {
    Measurement m = xpsMeasurement();
    m.powerHelperAvailable = false;
    CheckResult result = resultOf(crate::system::evaluate(m), "powerLimit");
    EXPECT_EQ(CheckAction::None, result.action);
    EXPECT_TRUE(result.advice.contains(QLatin1String("firmware settings")));

    m.powerHelperInstalled = true;
    result = resultOf(crate::system::evaluate(m), "powerLimit");
    EXPECT_EQ(CheckStatus::Warning, result.status);
    EXPECT_EQ(CheckAction::None, result.action);
    EXPECT_TRUE(result.advice.contains(QLatin1String("/etc/default/crate-power-limit")));

    // On battery the firmware's limit is no problem
    m.power = {power(20, false)};
    EXPECT_EQ(CheckStatus::Ok, resultOf(crate::system::evaluate(m), "powerLimit").status);

    // With the helper on, a package that stays hot warns
    m.power = {power(45, true, 97), power(45, true, 99)};
    result = resultOf(crate::system::evaluate(m), "powerLimit");
    EXPECT_EQ(CheckStatus::Warning, result.status);
    EXPECT_TRUE(result.detail.contains(QStringLiteral("97 °C"))) << result.detail.toStdString();
    m.power = {power(45, true, 97), power(45, true, 80)};
    EXPECT_EQ(CheckStatus::Ok, resultOf(crate::system::evaluate(m), "powerLimit").status);
}

TEST(CrateSystemCheckTest, NoOpenGlBlocks) {
    Measurement m = xpsMeasurement();
    m.render.usable = false;
    EXPECT_EQ(CheckStatus::Blocking, resultOf(crate::system::evaluate(m), "graphics").status);
    m.render.usable = true;
    m.render.renderer = QStringLiteral("llvmpipe (LLVM 20.1.8, 256 bits)");
    EXPECT_EQ(CheckStatus::Warning, resultOf(crate::system::evaluate(m), "graphics").status);
}

TEST(CrateSystemCheckTest, AudioDropoutsWarn) {
    Measurement m = xpsMeasurement();
    m.audio = {{true, 30, 4}, {true, 45, 4}};
    CheckResult result = resultOf(crate::system::evaluate(m), "audio");
    EXPECT_EQ(CheckStatus::Ok, result.status);
    EXPECT_TRUE(result.detail.contains(QLatin1String("45 %")));
    m.audio.append({true, 40, 6});
    result = resultOf(crate::system::evaluate(m), "audio");
    EXPECT_EQ(CheckStatus::Warning, result.status);
    EXPECT_TRUE(result.detail.contains(QLatin1String("2 dropouts")));
    EXPECT_TRUE(result.advice.contains(QLatin1String("Sound hardware")));
}

// Windows and macOS: the GPU and the monitor only
TEST(CrateSystemCheckTest, OtherSystemsOnlyCheckTheGpuAndTheMonitor) {
    Measurement m = xpsMeasurement();
    m.isLinux = false;
    const QList<CheckResult> results = crate::system::evaluate(m);
    for (const char* id : {"gpuClock", "powerLimit", "compositor"}) {
        EXPECT_EQ(CheckStatus::Unavailable, resultOf(results, id).status) << id;
        EXPECT_EQ(QStringLiteral("Not available on this system."), resultOf(results, id).detail);
    }
    EXPECT_NE(CheckStatus::Unavailable, resultOf(results, "graphics").status);
    EXPECT_NE(CheckStatus::Unavailable, resultOf(results, "display").status);
}

class CrateSystemCheckRunTest : public MixxxTest {};

// A whole run over a fake tree: it samples for its duration and measures what
// changed meanwhile
TEST_F(CrateSystemCheckRunTest, MeasuresForItsDuration) {
#ifndef Q_OS_LINUX
    GTEST_SKIP() << "Reads /sys and /proc";
#endif
    FakeSystemTree tree;
    tree.addGpu(kIntel,
            QStringLiteral("0000:00:02.0"),
            QStringLiteral("0x8086"),
            QStringLiteral("i915"),
            true);
    tree.addGpu(QStringLiteral("card1"),
            QStringLiteral("0000:01:00.0"),
            QStringLiteral("0x10de"),
            QStringLiteral("nvidia"),
            false);
    tree.setI915Clock(kIntel, 450, 2350, 0);
    tree.setI915Throttle(kIntel, "pl1", true);
    tree.setRaplPl1("intel-rapl-mmio:0", 20);
    tree.setOnAc(true);
    tree.setGpuClient(3906, "Hyprland", "0000:00:02.0", 0);

    crate::system::Probes probes;
    probes.root = tree.root();
    probes.render = [] {
        return crate::system::RenderInfo{
                true, QStringLiteral("Mesa Intel(R) Arc(TM) Graphics (MTL)"), true};
    };
    probes.display = [] {
        return crate::system::DisplayInfo{true, 3840, 2400, 90};
    };
    probes.powerHelperAvailable = [] {
        return true;
    };
    crate::system::SystemCheck check(probes);
    check.setDuration(300);
    // The compositor keeps the GPU busy from the first sample on: busy until
    // the next sample at each sample
    QObject::connect(&check, &crate::system::SystemCheck::sampled, [&tree](qint64 elapsedMs) {
        tree.setGpuClient(3906,
                "Hyprland",
                "0000:00:02.0",
                (elapsedMs + 2 * crate::system::SystemCheck::kSampleIntervalMs) * 1'000'000);
    });
    QSignalSpy finished(&check, &crate::system::SystemCheck::finished);
    QSignalSpy running(&check, &crate::system::SystemCheck::runningChanged);
    check.start();
    EXPECT_TRUE(check.isRunning());
    check.start(); // nothing while it runs
    ASSERT_TRUE(finished.wait(5000));
    EXPECT_FALSE(check.isRunning());
    EXPECT_EQ(2, running.size());
    EXPECT_GE(check.measurement().elapsedMs, 300);
    EXPECT_GE(check.measurement().clocks.size(), 2);

    const QList<CheckResult> results = check.results();
    ASSERT_EQ(6, results.size());
    EXPECT_EQ(CheckStatus::Warning, resultOf(results, "compositor").status)
            << resultOf(results, "compositor").detail.toStdString();
    EXPECT_EQ(CheckAction::PowerHelper, resultOf(results, "powerLimit").action);
    EXPECT_EQ(CheckAction::DiscreteGpu, resultOf(results, "graphics").action);

    // Installed: the unit in /etc
    tree.write(QStringLiteral("etc/systemd/system/crate-power-limit.service"), "[Unit]");
    check.setDuration(0);
    // Without a duration it finishes at once
    check.start();
    EXPECT_EQ(2, finished.size());
    EXPECT_TRUE(check.measurement().powerHelperInstalled);
    EXPECT_EQ(CheckAction::None, resultOf(check.results(), "powerLimit").action);
}

TEST_F(CrateSystemCheckRunTest, PowerHelperRunsTheInstallerWithItsProgram) {
    QTemporaryDir helper;
    ASSERT_TRUE(helper.isValid());
    crate::system::PowerHelper powerHelper;
    powerHelper.setDirectory(helper.path());
    powerHelper.setProgram(QStringLiteral("/bin/sh"));
    EXPECT_FALSE(powerHelper.isAvailable()) << "No installer";

    const QString marker = helper.filePath(QStringLiteral("ran"));
    {
        QFile script(helper.filePath(QStringLiteral("install.sh")));
        ASSERT_TRUE(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral("#!/bin/sh\ntouch '%1'\necho installed\n")
                        .arg(marker)
                        .toUtf8());
        script.close();
        script.setPermissions(script.permissions() | QFile::ExeOwner);
    }
#ifdef Q_OS_LINUX
    EXPECT_TRUE(powerHelper.isAvailable());
#endif
    QSignalSpy finished(&powerHelper, &crate::system::PowerHelper::finished);
    powerHelper.install();
    EXPECT_TRUE(powerHelper.isRunning());
    ASSERT_TRUE(finished.wait(5000));
    EXPECT_FALSE(powerHelper.isRunning());
    EXPECT_TRUE(finished.first().at(0).toBool());
    EXPECT_EQ(QStringLiteral("installed"), finished.first().at(1).toString());
    EXPECT_TRUE(QFileInfo::exists(marker));

    // pkexec dismissed or a failing installer
    powerHelper.setProgram(QStringLiteral("/bin/false"));
    powerHelper.install();
    ASSERT_TRUE(finished.wait(5000));
    EXPECT_FALSE(finished.last().at(0).toBool());

    powerHelper.setProgram(QStringLiteral("/nonexistent/pkexec"));
    EXPECT_FALSE(powerHelper.isAvailable());
}

// The packages ship the helper next to the resources
TEST(CrateSystemCheckTest, PowerHelperShipsWithTheResources) {
    EXPECT_TRUE(QFileInfo(QDir(crate::system::PowerHelper::defaultDirectory())
                                  .filePath(QStringLiteral("install.sh")))
                    .isExecutable());
}

} // namespace
