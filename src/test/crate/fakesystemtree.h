#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

namespace crate::test {

/// A fake root with the /sys and /proc files the system check reads, as the
/// power limit helper's test fakes sysfs.
class FakeSystemTree {
  public:
    QString root() const {
        return m_dir.path();
    }

    /// Writes `content` to `path`, relative to the root.
    bool write(const QString& path, const QByteArray& content) const {
        const QString file = m_dir.filePath(path);
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile out(file);
        return out.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                out.write(content + '\n') >= 0;
    }
    bool write(const QString& path, qint64 value) const {
        return write(path, QByteArray::number(value));
    }

    /// /sys/class/drm/`card` with its PCI device.
    void addGpu(const QString& card,
            const QString& slot,
            const QString& vendor,
            const QString& driver,
            bool bootVga) const {
        const QString device = QStringLiteral("sys/class/drm/") + card + QStringLiteral("/device/");
        write(device + QStringLiteral("vendor"), vendor.toLatin1());
        write(device + QStringLiteral("device"), "0x0001");
        write(device + QStringLiteral("uevent"),
                QByteArrayLiteral("DRIVER=") + driver.toLatin1() +
                        QByteArrayLiteral("\nPCI_SLOT_NAME=") + slot.toLatin1());
        if (bootVga) {
            write(device + QStringLiteral("boot_vga"), "1");
        }
    }

    /// The i915 clock files of `card` (gt/gt0).
    void setI915Clock(const QString& card, int actualMhz, int maxMhz, qint64 rc6Ms) const {
        const QString gt = QStringLiteral("sys/class/drm/") + card + QStringLiteral("/gt/gt0/");
        write(gt + QStringLiteral("rps_act_freq_mhz"), actualMhz);
        write(gt + QStringLiteral("rps_RP0_freq_mhz"), maxMhz);
        write(gt + QStringLiteral("rps_RPn_freq_mhz"), 100);
        write(gt + QStringLiteral("rc6_residency_ms"), rc6Ms);
        for (const char* reason : {"pl1", "pl2", "pl4", "thermal", "ratl", "prochot"}) {
            write(gt + QStringLiteral("throttle_reason_") + QLatin1String(reason), 0);
        }
    }
    void setI915Throttle(const QString& card, const char* reason, bool on) const {
        write(QStringLiteral("sys/class/drm/") + card +
                        QStringLiteral("/gt/gt0/throttle_reason_") + QLatin1String(reason),
                on ? 1 : 0);
    }

    /// The sustained limit (constraint 0, long_term) of a RAPL zone
    /// ("intel-rapl-mmio:0" or "intel-rapl:0").
    void setRaplPl1(const char* zone, int watts) const {
        const QString dir = QStringLiteral("sys/class/powercap/") + QLatin1String(zone) +
                QLatin1Char('/');
        write(dir + QStringLiteral("name"), "package-0");
        write(dir + QStringLiteral("constraint_0_name"), "long_term");
        write(dir + QStringLiteral("constraint_0_power_limit_uw"), qint64{watts} * 1000000);
        write(dir + QStringLiteral("constraint_1_name"), "short_term");
        write(dir + QStringLiteral("constraint_1_power_limit_uw"), qint64{115} * 1000000);
    }

    void setOnAc(bool onAc) const {
        write(QStringLiteral("sys/class/power_supply/AC/type"), "Mains");
        write(QStringLiteral("sys/class/power_supply/AC/online"), onAc ? 1 : 0);
        write(QStringLiteral("sys/class/power_supply/BAT0/type"), "Battery");
    }

    void setPackageTemperature(int celsius) const {
        write(QStringLiteral("sys/class/thermal/thermal_zone3/type"), "x86_pkg_temp");
        write(QStringLiteral("sys/class/thermal/thermal_zone3/temp"), qint64{celsius} * 1000);
    }

    /// A process named `comm` with one DRM client of `pdev` whose render
    /// engine was busy `renderNs` in total.
    void setGpuClient(int pid, const char* comm, const char* pdev, qint64 renderNs) const {
        const QString proc = QStringLiteral("proc/%1/").arg(pid);
        write(proc + QStringLiteral("comm"), comm);
        const QByteArray fdinfo = QByteArrayLiteral(
                                          "pos:\t0\nflags:\t02100002\ndrm-driver:\ti915\n"
                                          "drm-client-id:\t16\ndrm-pdev:\t") +
                pdev + QByteArrayLiteral("\ndrm-engine-render:\t") +
                QByteArray::number(renderNs) +
                QByteArrayLiteral(
                        " ns\ndrm-engine-copy:\t0 ns\ndrm-engine-video:\t0 ns\n"
                        "drm-engine-capacity-video:\t2\n");
        // The same client through two descriptors counts once
        write(proc + QStringLiteral("fdinfo/42"), fdinfo);
        write(proc + QStringLiteral("fdinfo/43"), fdinfo);
        write(proc + QStringLiteral("fdinfo/0"), "pos:\t0\nflags:\t02\n");
    }

  private:
    QTemporaryDir m_dir;
};

} // namespace crate::test
