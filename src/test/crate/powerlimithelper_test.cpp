#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "preferences/configobject.h"

// The power limit helper of the hybrid laptop plan (§4): the script that sets
// PL1 and its installer, run against a fake sysfs as the reference was.

namespace {

QString helperPath(const QString& name) {
    return QDir(ConfigObject<ConfigValue>::computeResourcePath())
            .filePath(QStringLiteral("crate-power-limit/") + name);
}

bool writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) >= 0;
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray();
}

class CratePowerLimitHelperTest : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_sysfs.isValid());
        ASSERT_TRUE(writeFile(limitPath(), "20000000\n"));
        ASSERT_TRUE(writeFile(supply("AC", "type"), "Mains\n"));
        ASSERT_TRUE(writeFile(supply("BAT0", "type"), "Battery\n"));
        ASSERT_TRUE(writeFile(supply("BAT0", "online"), "1\n"));
        setOnAc(true);
    }

    QString limitPath() const {
        return m_sysfs.filePath(
                QStringLiteral("powercap/intel-rapl-mmio:0/constraint_0_power_limit_uw"));
    }
    QString supply(const char* name, const char* file) const {
        return m_sysfs.filePath(
                QStringLiteral("power_supply/%1/%2")
                        .arg(QLatin1String(name), QLatin1String(file)));
    }
    void setOnAc(bool onAc) {
        ASSERT_TRUE(writeFile(supply("AC", "online"), onAc ? "1\n" : "0\n"));
    }

    /// Runs the helper `script` with the fake sysfs and `environment`.
    int run(const QString& script,
            const QStringList& arguments = {},
            const QStringList& environment = {}) {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("CRATE_PL1_AC_WATTS"));
        env.remove(QStringLiteral("CRATE_PL1_BATTERY_WATTS"));
        env.insert(QStringLiteral("CRATE_POWER_SYSFS"), m_sysfs.path());
        for (const QString& assignment : environment) {
            const qsizetype equals = assignment.indexOf(QLatin1Char('='));
            env.insert(assignment.left(equals), assignment.mid(equals + 1));
        }
        process.setProcessEnvironment(env);
        process.start(QStringLiteral("/bin/sh"), QStringList{helperPath(script)} + arguments);
        if (!process.waitForFinished(10000)) {
            return -1;
        }
        m_output = QString::fromUtf8(process.readAllStandardOutput() +
                process.readAllStandardError());
        return process.exitCode();
    }

    QTemporaryDir m_sysfs;
    QString m_output;
};

TEST_F(CratePowerLimitHelperTest, RaisesTheLimitOnAc) {
    ASSERT_EQ(0, run(QStringLiteral("crate-power-limit"))) << m_output.toStdString();
    EXPECT_EQ("45000000", readFile(limitPath()));
    EXPECT_TRUE(m_output.contains(QLatin1String("45 W (AC)"))) << m_output.toStdString();
}

TEST_F(CratePowerLimitHelperTest, RestoresTheBatteryLimit) {
    ASSERT_TRUE(writeFile(limitPath(), "45000000\n"));
    setOnAc(false);
    ASSERT_EQ(0, run(QStringLiteral("crate-power-limit"))) << m_output.toStdString();
    EXPECT_EQ("20000000", readFile(limitPath()));
}

TEST_F(CratePowerLimitHelperTest, TakesTheUsersWatts) {
    ASSERT_EQ(0,
            run(QStringLiteral("crate-power-limit"),
                    {},
                    {QStringLiteral("CRATE_PL1_AC_WATTS=38"),
                            QStringLiteral("CRATE_PL1_BATTERY_WATTS=15")}));
    EXPECT_EQ("38000000", readFile(limitPath()));
    setOnAc(false);
    ASSERT_EQ(0,
            run(QStringLiteral("crate-power-limit"),
                    {},
                    {QStringLiteral("CRATE_PL1_BATTERY_WATTS=15")}));
    EXPECT_EQ("15000000", readFile(limitPath()));
}

// Machines without MMIO RAPL: nothing to set, and the service does not fail
TEST_F(CratePowerLimitHelperTest, DoesNothingWithoutMmioRapl) {
    ASSERT_TRUE(QFile::remove(limitPath()));
    EXPECT_EQ(0, run(QStringLiteral("crate-power-limit")));
    EXPECT_FALSE(QFileInfo::exists(limitPath()));
    EXPECT_TRUE(m_output.contains(QLatin1String("missing"))) << m_output.toStdString();
}

// The installer under DESTDIR: the files where the unit expects them, and the
// firmware's limit kept as the battery value
TEST_F(CratePowerLimitHelperTest, InstallerKeepsTheFirmwareLimitForBattery) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString destDir = QStringLiteral("DESTDIR=") + root.path();
    ASSERT_EQ(0, run(QStringLiteral("install.sh"), {}, {destDir})) << m_output.toStdString();
    for (const char* file : {"usr/local/bin/crate-power-limit",
                 "etc/systemd/system/crate-power-limit.service",
                 "etc/udev/rules.d/90-crate-power-limit.rules",
                 "usr/lib/systemd/system-sleep/crate-power-limit"}) {
        EXPECT_TRUE(QFileInfo::exists(root.filePath(QLatin1String(file)))) << file;
    }
    EXPECT_TRUE(QFileInfo(root.filePath(QStringLiteral("usr/local/bin/crate-power-limit")))
                    .isExecutable());
    const QByteArray defaults = readFile(root.filePath(QStringLiteral(
            "etc/default/crate-power-limit")));
    EXPECT_TRUE(defaults.contains("CRATE_PL1_AC_WATTS=45")) << defaults.constData();
    EXPECT_TRUE(defaults.contains("CRATE_PL1_BATTERY_WATTS=20")) << defaults.constData();
    // Under DESTDIR nothing runs, so the limit is the firmware's still
    EXPECT_EQ("20000000", readFile(limitPath()));

    // The user's values survive a second install
    ASSERT_TRUE(writeFile(root.filePath(QStringLiteral("etc/default/crate-power-limit")),
            "CRATE_PL1_AC_WATTS=40\n"));
    ASSERT_EQ(0, run(QStringLiteral("install.sh"), {}, {destDir}));
    EXPECT_EQ("CRATE_PL1_AC_WATTS=40",
            readFile(root.filePath(QStringLiteral("etc/default/crate-power-limit"))));

    ASSERT_EQ(0, run(QStringLiteral("install.sh"), {QStringLiteral("--uninstall")}, {destDir}));
    EXPECT_FALSE(QFileInfo::exists(
            root.filePath(QStringLiteral("usr/local/bin/crate-power-limit"))));
    EXPECT_FALSE(QFileInfo::exists(
            root.filePath(QStringLiteral("etc/systemd/system/crate-power-limit.service"))));
    EXPECT_TRUE(QFileInfo::exists(root.filePath(QStringLiteral("etc/default/crate-power-limit"))));
}

} // namespace
