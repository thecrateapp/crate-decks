#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace crate::system {

/// A GPU of this computer, as /sys/class/drm/card<N>/device describes it.
struct GpuInfo {
    /// "card0"
    QString card;
    /// "0000:00:02.0"
    QString pciSlot;
    /// "0x8086" (Intel), "0x10de" (NVIDIA), "0x1002" (AMD)
    QString vendorId;
    QString deviceId;
    /// The kernel driver: "i915", "xe", "nvidia", "amdgpu", "nouveau"...
    QString driver;
    /// The firmware's boot display device (boot_vga): on a hybrid laptop, the
    /// integrated GPU that drives the panel.
    bool bootVga = false;

    /// Whether this is the integrated GPU: the boot display device of a
    /// hybrid laptop, or a lone Intel GPU on bus 0.
    bool integrated = false;

    /// "Intel", "NVIDIA", "AMD" or the vendor ID.
    QString vendorName() const;
    /// The NVIDIA proprietary driver, which offloads with its own variables.
    bool nvidiaProprietary() const;
    /// A driver that renders through Mesa, which offloads with DRI_PRIME.
    bool mesa() const;
};

/// The GPUs under `root` (normally "/"): every /sys/class/drm/card<N> with a
/// PCI device, connectors excluded. With more than one, the boot display
/// device (or, without one, an Intel GPU on bus 0) is the integrated one; the
/// others are discrete.
QList<GpuInfo> readGpus(const QString& root);

/// Whether `gpus` has an integrated and a discrete GPU.
bool isHybrid(const QList<GpuInfo>& gpus);

/// The discrete GPU that Decks can draw with, or nullptr.
const GpuInfo* discreteGpu(const QList<GpuInfo>& gpus);

/// The windowing system Qt will use.
enum class DisplayServer {
    Wayland,
    X11,
};

/// The display server Qt picks with `environment`: QT_QPA_PLATFORM when set,
/// else Wayland when the session is one (XDG_SESSION_TYPE, WAYLAND_DISPLAY).
DisplayServer displayServerFor(const QHash<QString, QString>& environment);

/// The NVIDIA EGL vendor file of glvnd under `root`
/// (/usr/share/glvnd/egl_vendor.d or /etc/glvnd/egl_vendor.d), empty if none.
QString nvidiaEglVendorFile(const QString& root);

struct EnvironmentVariable {
    QByteArray name;
    QByteArray value;

    bool operator==(const EnvironmentVariable& other) const {
        return name == other.name && value == other.value;
    }
};

/// The environment that makes Decks draw with the discrete GPU of `gpus`
/// (hybrid laptop plan §2), for a pure, tested decision:
/// - NVIDIA proprietary: __NV_PRIME_RENDER_OFFLOAD=1,
///   __GLX_VENDOR_LIBRARY_NAME=nvidia and, on Wayland, where Qt draws through
///   EGL, __EGL_VENDOR_LIBRARY_FILENAMES=`nvidiaEglVendorFile` (when known);
/// - Mesa (amdgpu, radeon, nouveau, i915 or xe on an Intel Arc card):
///   DRI_PRIME=1.
/// Empty without a discrete GPU. A variable already in `environment` is never
/// overridden: the user's choice wins.
QList<EnvironmentVariable> discreteGpuEnvironment(const QList<GpuInfo>& gpus,
        DisplayServer displayServer,
        const QHash<QString, QString>& environment,
        const QString& nvidiaEglVendorFile);

/// The item of the preference in the [Crate] group of the settings
/// (CratePreferences::discreteGpu()).
inline constexpr const char* kDiscreteGpuItem = "DiscreteGpu";

/// Whether the settings file in `settingsPath` asks to draw with the discrete
/// GPU. Read without the application, before Qt starts.
bool discreteGpuPreferred(const QString& settingsPath);

/// The environment of discreteGpuEnvironment() for this start: empty unless
/// the settings in `settingsPath` ask for it and this is Linux.
QList<EnvironmentVariable> discreteGpuEnvironmentAtStart(const QString& settingsPath,
        const QString& root,
        const QHash<QString, QString>& environment);

/// The hook of main(), before QApplication and so before Qt creates any
/// OpenGL context: sets the environment of discreteGpuEnvironmentAtStart()
/// for this process. Does nothing unless the preference is on.
void applyDiscreteGpuPreference(const QString& settingsPath);

} // namespace crate::system
