#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QString>
#include <QTemporaryDir>

#include "config.h"
#include "crate/cratepreferences.h"
#include "crate/system/gpus.h"
#include "test/crate/fakesystemtree.h"

// The discrete GPU preference of the hybrid laptop plan (§2): which GPUs this
// computer has and the environment that makes Decks draw with the discrete one.

namespace {

using crate::system::DisplayServer;
using crate::system::EnvironmentVariable;
using crate::system::GpuInfo;
using crate::test::FakeSystemTree;

GpuInfo gpu(const char* vendor, const char* driver, bool integrated) {
    GpuInfo info;
    info.vendorId = QLatin1String(vendor);
    info.driver = QLatin1String(driver);
    info.integrated = integrated;
    info.bootVga = integrated;
    return info;
}

const QString kNvidiaEgl = QStringLiteral("/usr/share/glvnd/egl_vendor.d/10_nvidia.json");

TEST(CrateSystemGpusTest, ReadsTheGpusOfAHybridLaptop) {
    FakeSystemTree tree;
    tree.addGpu(QStringLiteral("card0"),
            QStringLiteral("0000:00:02.0"),
            QStringLiteral("0x8086"),
            QStringLiteral("i915"),
            true);
    tree.addGpu(QStringLiteral("card1"),
            QStringLiteral("0000:01:00.0"),
            QStringLiteral("0x10de"),
            QStringLiteral("nvidia"),
            false);
    // Connectors are not GPUs
    tree.write(QStringLiteral("sys/class/drm/card0-eDP-1/status"), "connected");

    const QList<GpuInfo> gpus = crate::system::readGpus(tree.root());
    ASSERT_EQ(2, gpus.size());
    EXPECT_EQ(QStringLiteral("card0"), gpus[0].card);
    EXPECT_EQ(QStringLiteral("0000:00:02.0"), gpus[0].pciSlot);
    EXPECT_EQ(QStringLiteral("Intel"), gpus[0].vendorName());
    EXPECT_EQ(QStringLiteral("i915"), gpus[0].driver);
    EXPECT_TRUE(gpus[0].integrated);
    EXPECT_EQ(QStringLiteral("NVIDIA"), gpus[1].vendorName());
    EXPECT_FALSE(gpus[1].integrated);
    EXPECT_TRUE(gpus[1].nvidiaProprietary());
    EXPECT_TRUE(crate::system::isHybrid(gpus));
    ASSERT_NE(nullptr, crate::system::discreteGpu(gpus));
    EXPECT_EQ(QStringLiteral("card1"), crate::system::discreteGpu(gpus)->card);
}

TEST(CrateSystemGpusTest, OneGpuIsNotHybrid) {
    FakeSystemTree tree;
    tree.addGpu(QStringLiteral("card0"),
            QStringLiteral("0000:03:00.0"),
            QStringLiteral("0x1002"),
            QStringLiteral("amdgpu"),
            true);
    const QList<GpuInfo> gpus = crate::system::readGpus(tree.root());
    ASSERT_EQ(1, gpus.size());
    EXPECT_FALSE(gpus[0].integrated) << "A lone AMD card is a desktop's";
    EXPECT_FALSE(crate::system::isHybrid(gpus));
    EXPECT_EQ(nullptr, crate::system::discreteGpu(gpus));
    EXPECT_TRUE(crate::system::readGpus(QStringLiteral("/nonexistent")).isEmpty());
}

// Without boot_vga, the Intel GPU on bus 0 is the integrated one
TEST(CrateSystemGpusTest, IntelOnBusZeroIsIntegratedWithoutBootVga) {
    FakeSystemTree tree;
    tree.addGpu(QStringLiteral("card1"),
            QStringLiteral("0000:01:00.0"),
            QStringLiteral("0x1002"),
            QStringLiteral("amdgpu"),
            false);
    tree.addGpu(QStringLiteral("card0"),
            QStringLiteral("0000:00:02.0"),
            QStringLiteral("0x8086"),
            QStringLiteral("xe"),
            false);
    const QList<GpuInfo> gpus = crate::system::readGpus(tree.root());
    ASSERT_EQ(2, gpus.size());
    EXPECT_EQ(QStringLiteral("card0"), gpus[0].card);
    EXPECT_TRUE(gpus[0].integrated);
    EXPECT_EQ(QStringLiteral("card1"), crate::system::discreteGpu(gpus)->card);
}

TEST(CrateSystemGpusTest, DisplayServerFollowsQtsChoice) {
    using Env = QHash<QString, QString>;
    EXPECT_EQ(DisplayServer::Wayland,
            crate::system::displayServerFor(
                    Env{{QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-1")}}));
    EXPECT_EQ(DisplayServer::Wayland,
            crate::system::displayServerFor(
                    Env{{QStringLiteral("XDG_SESSION_TYPE"), QStringLiteral("wayland")}}));
    EXPECT_EQ(DisplayServer::X11,
            crate::system::displayServerFor(
                    Env{{QStringLiteral("DISPLAY"), QStringLiteral(":0")}}));
    // QT_QPA_PLATFORM wins over the session
    EXPECT_EQ(DisplayServer::X11,
            crate::system::displayServerFor(
                    Env{{QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-1")},
                            {QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("xcb")}}));
    EXPECT_EQ(DisplayServer::Wayland,
            crate::system::displayServerFor(
                    Env{{QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland;xcb")}}));
}

TEST(CrateSystemGpusTest, EnvironmentForEachDriverAndDisplayServer) {
    const QList<GpuInfo> nvidia = {gpu("0x8086", "i915", true), gpu("0x10de", "nvidia", false)};
    const QList<GpuInfo> amd = {gpu("0x8086", "i915", true), gpu("0x1002", "amdgpu", false)};
    const QList<GpuInfo> nouveau = {
            gpu("0x8086", "i915", true), gpu("0x10de", "nouveau", false)};
    const QList<GpuInfo> arc = {gpu("0x8086", "i915", true), gpu("0x8086", "xe", false)};
    const QList<GpuInfo> single = {gpu("0x8086", "i915", true)};
    const QHash<QString, QString> none;

    struct Case {
        const char* name;
        QList<GpuInfo> gpus;
        DisplayServer displayServer;
        QList<EnvironmentVariable> expected;
    };
    const QList<Case> cases = {
            {"NVIDIA on Wayland",
                    nvidia,
                    DisplayServer::Wayland,
                    {{"__NV_PRIME_RENDER_OFFLOAD", "1"},
                            {"__GLX_VENDOR_LIBRARY_NAME", "nvidia"},
                            {"__EGL_VENDOR_LIBRARY_FILENAMES", kNvidiaEgl.toLocal8Bit()}}},
            {"NVIDIA on X11",
                    nvidia,
                    DisplayServer::X11,
                    {{"__NV_PRIME_RENDER_OFFLOAD", "1"},
                            {"__GLX_VENDOR_LIBRARY_NAME", "nvidia"}}},
            {"AMD on Wayland", amd, DisplayServer::Wayland, {{"DRI_PRIME", "1"}}},
            {"AMD on X11", amd, DisplayServer::X11, {{"DRI_PRIME", "1"}}},
            {"nouveau", nouveau, DisplayServer::Wayland, {{"DRI_PRIME", "1"}}},
            {"Intel Arc", arc, DisplayServer::X11, {{"DRI_PRIME", "1"}}},
            {"one GPU", single, DisplayServer::Wayland, {}},
            {"no GPU", {}, DisplayServer::Wayland, {}},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(c.expected,
                crate::system::discreteGpuEnvironment(
                        c.gpus, c.displayServer, none, kNvidiaEgl))
                << c.name;
    }
    // Without the vendor file, no EGL vendor variable
    EXPECT_EQ((QList<EnvironmentVariable>{{"__NV_PRIME_RENDER_OFFLOAD", "1"},
                      {"__GLX_VENDOR_LIBRARY_NAME", "nvidia"}}),
            crate::system::discreteGpuEnvironment(
                    nvidia, DisplayServer::Wayland, none, QString()));
}

TEST(CrateSystemGpusTest, NeverOverridesTheUsersVariables) {
    const QList<GpuInfo> nvidia = {gpu("0x8086", "i915", true), gpu("0x10de", "nvidia", false)};
    const QHash<QString, QString> user = {
            {QStringLiteral("__EGL_VENDOR_LIBRARY_FILENAMES"),
                    QStringLiteral("/usr/share/glvnd/egl_vendor.d/50_mesa.json")},
            {QStringLiteral("__NV_PRIME_RENDER_OFFLOAD"), QStringLiteral("0")}};
    EXPECT_EQ((QList<EnvironmentVariable>{{"__GLX_VENDOR_LIBRARY_NAME", "nvidia"}}),
            crate::system::discreteGpuEnvironment(
                    nvidia, DisplayServer::Wayland, user, kNvidiaEgl));
    const QList<GpuInfo> amd = {gpu("0x8086", "i915", true), gpu("0x1002", "amdgpu", false)};
    EXPECT_TRUE(crate::system::discreteGpuEnvironment(amd,
            DisplayServer::X11,
            {{QStringLiteral("DRI_PRIME"), QStringLiteral("0")}},
            QString())
                    .isEmpty());
}

TEST(CrateSystemGpusTest, FindsTheNvidiaEglVendorFile) {
    FakeSystemTree tree;
    EXPECT_TRUE(crate::system::nvidiaEglVendorFile(tree.root()).isEmpty());
    tree.write(QStringLiteral("usr/share/glvnd/egl_vendor.d/50_mesa.json"), "{}");
    EXPECT_TRUE(crate::system::nvidiaEglVendorFile(tree.root()).isEmpty());
    tree.write(QStringLiteral("etc/glvnd/egl_vendor.d/10_nvidia.json"), "{}");
    EXPECT_EQ(QStringLiteral("/etc/glvnd/egl_vendor.d/10_nvidia.json"),
            crate::system::nvidiaEglVendorFile(tree.root()));
    tree.write(QStringLiteral("usr/share/glvnd/egl_vendor.d/10_nvidia.json"), "{}");
    EXPECT_EQ(kNvidiaEgl, crate::system::nvidiaEglVendorFile(tree.root()));
}

// The preference goes through the settings file that main() reads before
// the application exists, in the folder of --settingsPath
TEST(CrateSystemGpusTest, PreferenceRoundTripsThroughTheSettingsFile) {
    QTemporaryDir settings;
    ASSERT_TRUE(settings.isValid());
    FakeSystemTree tree;
    tree.addGpu(QStringLiteral("card0"),
            QStringLiteral("0000:00:02.0"),
            QStringLiteral("0x8086"),
            QStringLiteral("i915"),
            true);
    tree.addGpu(QStringLiteral("card1"),
            QStringLiteral("0000:01:00.0"),
            QStringLiteral("0x10de"),
            QStringLiteral("nvidia"),
            false);
    tree.write(QStringLiteral("usr/share/glvnd/egl_vendor.d/10_nvidia.json"), "{}");
    const QHash<QString, QString> wayland = {
            {QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-1")}};

    EXPECT_FALSE(crate::system::discreteGpuPreferred(settings.path()));
    EXPECT_TRUE(crate::system::discreteGpuEnvironmentAtStart(settings.path(), tree.root(), wayland)
                    .isEmpty());

    {
        auto pConfig = UserSettingsPointer(
                new UserSettings(QDir(settings.path()).filePath(MIXXX_SETTINGS_FILE)));
        crate::CratePreferences preferences(pConfig);
        EXPECT_FALSE(preferences.discreteGpu()) << "Off by default (Diego, 2026-11-15)";
        preferences.setDiscreteGpu(true);
        EXPECT_TRUE(preferences.discreteGpu());
        // Written at once, without waiting for the application to save
    }
    EXPECT_TRUE(crate::system::discreteGpuPreferred(settings.path()));
#ifdef Q_OS_LINUX
    EXPECT_EQ(3,
            crate::system::discreteGpuEnvironmentAtStart(settings.path(), tree.root(), wayland)
                    .size());
#endif
    QTemporaryDir other;
    EXPECT_FALSE(crate::system::discreteGpuPreferred(other.path()));
}

} // namespace
