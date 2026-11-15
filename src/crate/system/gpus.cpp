#include "crate/system/gpus.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QtGlobal>
#include <algorithm>
#include <utility>

#include "config.h"
#include "preferences/configobject.h"
#include "util/logger.h"

namespace crate::system {

namespace {

const mixxx::Logger kLogger("crate::system::Gpus");

const QString kVendorIntel = QStringLiteral("0x8086");
const QString kVendorNvidia = QStringLiteral("0x10de");
const QString kVendorAmd = QStringLiteral("0x1002");

constexpr const char* kNvPrimeRenderOffload = "__NV_PRIME_RENDER_OFFLOAD";
constexpr const char* kGlxVendorLibraryName = "__GLX_VENDOR_LIBRARY_NAME";
constexpr const char* kEglVendorLibraryFilenames = "__EGL_VENDOR_LIBRARY_FILENAMES";
constexpr const char* kDriPrime = "DRI_PRIME";

QString readText(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

/// The KEY=value lines of a uevent file.
QHash<QString, QString> readUevent(const QString& path) {
    QHash<QString, QString> values;
    const QStringList lines = readText(path).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals > 0) {
            values.insert(line.left(equals), line.mid(equals + 1).trimmed());
        }
    }
    return values;
}

/// The variables of this process.
QHash<QString, QString> processEnvironment() {
    QHash<QString, QString> environment;
    const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
    const QStringList keys = system.keys();
    for (const QString& key : keys) {
        environment.insert(key, system.value(key));
    }
    return environment;
}

} // namespace

QString GpuInfo::vendorName() const {
    if (vendorId == kVendorIntel) {
        return QStringLiteral("Intel");
    }
    if (vendorId == kVendorNvidia) {
        return QStringLiteral("NVIDIA");
    }
    if (vendorId == kVendorAmd) {
        return QStringLiteral("AMD");
    }
    return vendorId;
}

bool GpuInfo::nvidiaProprietary() const {
    return driver == QLatin1String("nvidia");
}

bool GpuInfo::mesa() const {
    static const QStringList kMesaDrivers = {QStringLiteral("amdgpu"),
            QStringLiteral("radeon"),
            QStringLiteral("nouveau"),
            QStringLiteral("i915"),
            QStringLiteral("xe")};
    return kMesaDrivers.contains(driver);
}

QList<GpuInfo> readGpus(const QString& root) {
    static const QRegularExpression kCardName(QStringLiteral("^card\\d+$"));
    const QDir drm(QDir(root).filePath(QStringLiteral("sys/class/drm")));
    QStringList cards = drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    // card2 before card10
    std::sort(cards.begin(), cards.end(), [](const QString& a, const QString& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    QList<GpuInfo> gpus;
    for (const QString& card : std::as_const(cards)) {
        if (!kCardName.match(card).hasMatch()) {
            continue;
        }
        const QString device = drm.filePath(card + QStringLiteral("/device/"));
        const QHash<QString, QString> uevent = readUevent(device + QStringLiteral("uevent"));
        const QString slot = uevent.value(QStringLiteral("PCI_SLOT_NAME"));
        const QString vendor = readText(device + QStringLiteral("vendor"));
        if (slot.isEmpty() || vendor.isEmpty()) {
            // Not a PCI device (a virtual or a platform display)
            continue;
        }
        GpuInfo gpu;
        gpu.card = card;
        gpu.pciSlot = slot;
        gpu.vendorId = vendor;
        gpu.deviceId = readText(device + QStringLiteral("device"));
        gpu.driver = uevent.value(QStringLiteral("DRIVER"));
        gpu.bootVga = readText(device + QStringLiteral("boot_vga")) == QLatin1String("1");
        gpus.append(gpu);
    }
    if (gpus.size() == 1) {
        // A lone Intel GPU on bus 0 is the processor's; a lone AMD or NVIDIA
        // card is a desktop's
        GpuInfo& gpu = gpus.first();
        gpu.integrated = gpu.vendorId == kVendorIntel &&
                gpu.pciSlot.startsWith(QLatin1String("0000:00:"));
        return gpus;
    }
    // The boot display device drives the panel: the integrated GPU
    int integrated = -1;
    for (int i = 0; i < gpus.size(); ++i) {
        if (gpus[i].bootVga) {
            integrated = i;
            break;
        }
    }
    if (integrated < 0) {
        for (int i = 0; i < gpus.size(); ++i) {
            if (gpus[i].vendorId == kVendorIntel &&
                    gpus[i].pciSlot.startsWith(QLatin1String("0000:00:"))) {
                integrated = i;
                break;
            }
        }
    }
    if (integrated >= 0) {
        gpus[integrated].integrated = true;
    }
    return gpus;
}

bool isHybrid(const QList<GpuInfo>& gpus) {
    return discreteGpu(gpus) != nullptr;
}

const GpuInfo* discreteGpu(const QList<GpuInfo>& gpus) {
    const bool hasIntegrated = std::any_of(gpus.cbegin(), gpus.cend(), [](const GpuInfo& gpu) {
        return gpu.integrated;
    });
    if (gpus.size() < 2 || !hasIntegrated) {
        return nullptr;
    }
    for (const GpuInfo& gpu : gpus) {
        if (!gpu.integrated && (gpu.nvidiaProprietary() || gpu.mesa())) {
            return &gpu;
        }
    }
    return nullptr;
}

DisplayServer displayServerFor(const QHash<QString, QString>& environment) {
    const QString platform = environment.value(QStringLiteral("QT_QPA_PLATFORM"));
    if (!platform.isEmpty()) {
        // A list ("wayland;xcb"): the first one Qt tries
        return platform.startsWith(QLatin1String("wayland")) ? DisplayServer::Wayland
                                                             : DisplayServer::X11;
    }
    if (environment.value(QStringLiteral("XDG_SESSION_TYPE")) == QLatin1String("wayland") ||
            !environment.value(QStringLiteral("WAYLAND_DISPLAY")).isEmpty()) {
        return DisplayServer::Wayland;
    }
    return DisplayServer::X11;
}

QString nvidiaEglVendorFile(const QString& root) {
    for (const char* directory : {"usr/share/glvnd/egl_vendor.d", "etc/glvnd/egl_vendor.d"}) {
        const QDir vendors(QDir(root).filePath(QLatin1String(directory)));
        const QStringList files = vendors.entryList(
                {QStringLiteral("*nvidia*.json")}, QDir::Files, QDir::Name);
        if (!files.isEmpty()) {
            // The path the running system has, not the one under `root`
            return QDir::cleanPath(QLatin1Char('/') + QLatin1String(directory) +
                    QLatin1Char('/') + files.first());
        }
    }
    return QString();
}

QList<EnvironmentVariable> discreteGpuEnvironment(const QList<GpuInfo>& gpus,
        DisplayServer displayServer,
        const QHash<QString, QString>& environment,
        const QString& nvidiaEglVendorFile) {
    QList<EnvironmentVariable> variables;
    const GpuInfo* pGpu = discreteGpu(gpus);
    if (!pGpu) {
        return variables;
    }
    const auto add = [&variables, &environment](const char* name, const QByteArray& value) {
        if (!environment.contains(QLatin1String(name))) {
            variables.append({QByteArray(name), value});
        }
    };
    if (pGpu->nvidiaProprietary()) {
        add(kNvPrimeRenderOffload, QByteArrayLiteral("1"));
        add(kGlxVendorLibraryName, QByteArrayLiteral("nvidia"));
        if (displayServer == DisplayServer::Wayland && !nvidiaEglVendorFile.isEmpty()) {
            add(kEglVendorLibraryFilenames, nvidiaEglVendorFile.toLocal8Bit());
        }
    } else {
        add(kDriPrime, QByteArrayLiteral("1"));
    }
    return variables;
}

bool discreteGpuPreferred(const QString& settingsPath) {
    // Not SettingsManager: there is no application yet
    const ConfigObject<ConfigValue> config(
            QDir(settingsPath).filePath(MIXXX_SETTINGS_FILE), QString(), QString());
    return config.getValue<bool>(
            ConfigKey(QStringLiteral("[Crate]"), QLatin1String(kDiscreteGpuItem)), false);
}

QList<EnvironmentVariable> discreteGpuEnvironmentAtStart(const QString& settingsPath,
        const QString& root,
        const QHash<QString, QString>& environment) {
#ifdef Q_OS_LINUX
    if (!discreteGpuPreferred(settingsPath)) {
        return {};
    }
    return discreteGpuEnvironment(readGpus(root),
            displayServerFor(environment),
            environment,
            nvidiaEglVendorFile(root));
#else
    Q_UNUSED(settingsPath);
    Q_UNUSED(root);
    Q_UNUSED(environment);
    return {};
#endif
}

void applyDiscreteGpuPreference(const QString& settingsPath) {
    const QList<EnvironmentVariable> variables = discreteGpuEnvironmentAtStart(
            settingsPath, QStringLiteral("/"), processEnvironment());
    for (const EnvironmentVariable& variable : variables) {
        qputenv(variable.name.constData(), variable.value);
        kLogger.info() << "Drawing with the discrete GPU:" << variable.name << '='
                       << variable.value;
    }
}

} // namespace crate::system
