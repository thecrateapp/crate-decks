#include "crate/smartmix/curves.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QHash>
#include <QStringList>
#include <QTextStream>
#include <limits>
#include <utility>

#include "test/mixxxtest.h"

namespace {

using crate::curves::Role;

// Android computes in float: a float has 24 bits of mantissa, so its values
// of 1 or below are within 1e-7 of the exact ones, and its dB values within
// 1e-5. The bounds leave room for the rounding of the steps in between.
constexpr double kGainTolerance = 2e-6;
constexpr double kDbTolerance = 2e-5;
// The gains of the table's columns (export_android_curves.py).
constexpr double kOutgoingGainDb = -4.2867;
constexpr double kIncomingGainDb = -3.7616;

class CrateCurvesTest : public MixxxTest {
  protected:
    /// The rows of android-curves.csv, generated from Android's own code by
    /// export_android_curves.py, by column name.
    QList<QHash<QString, double>> referenceRows() const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/smartmix/android-curves.csv")));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QTextStream stream(&file);
        QStringList header;
        QList<QHash<QString, double>> rows;
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (line.startsWith(QLatin1Char('#')) || line.isEmpty()) {
                continue;
            }
            const QStringList fields = line.split(QLatin1Char(','));
            if (header.isEmpty()) {
                header = fields;
                continue;
            }
            EXPECT_EQ(fields.size(), header.size()) << line.toStdString();
            QHash<QString, double> row;
            for (int i = 0; i < fields.size(); ++i) {
                bool ok = false;
                row.insert(header.at(i), fields.at(i).toDouble(&ok));
                EXPECT_TRUE(ok) << line.toStdString();
            }
            rows.append(row);
        }
        return rows;
    }
};

TEST_F(CrateCurvesTest, MatchesAndroidReferenceValues) {
    const QList<QHash<QString, double>> rows = referenceRows();
    ASSERT_GE(rows.size(), 65);
    const double outgoingStaging = crate::curves::stagingGain(kOutgoingGainDb);
    const double incomingStaging = crate::curves::stagingGain(kIncomingGainDb);
    for (const auto& row : rows) {
        const double progress = row.value(QStringLiteral("progress"));
        SCOPED_TRACE(testing::Message() << "progress " << progress);
        const auto gains = crate::curves::equalPower(progress);
        EXPECT_NEAR(gains.outgoing, row.value(QStringLiteral("equal_power_out")), kGainTolerance);
        EXPECT_NEAR(gains.incoming, row.value(QStringLiteral("equal_power_in")), kGainTolerance);
        EXPECT_NEAR(crate::curves::djBlend(progress),
                row.value(QStringLiteral("dj_blend")),
                kGainTolerance);
        for (const auto& [label, handoff] : {std::pair{QStringLiteral("050"), 0.5},
                     std::pair{QStringLiteral("002"), 0.02},
                     std::pair{QStringLiteral("097"), 0.97}}) {
            EXPECT_NEAR(crate::curves::bassCutDb(Role::Outgoing, progress, handoff),
                    row.value(QStringLiteral("bass_out_") + label),
                    kDbTolerance);
            EXPECT_NEAR(crate::curves::bassCutDb(Role::Incoming, progress, handoff),
                    row.value(QStringLiteral("bass_in_") + label),
                    kDbTolerance);
        }
        if (progress < 0.0 || progress > 1.0) {
            // Android's envelope never runs outside the transition
            // (`min(1, frame / frames)`) and its ease functions do not clamp
            // there; ours do.
            continue;
        }
        EXPECT_NEAR(crate::curves::stagingFactor(Role::Outgoing, progress, outgoingStaging),
                row.value(QStringLiteral("staging_out")),
                kGainTolerance);
        EXPECT_NEAR(crate::curves::stagingFactor(Role::Incoming, progress, incomingStaging),
                row.value(QStringLiteral("staging_in")),
                kGainTolerance);
        for (const bool djBlend : {false, true}) {
            const QString suffix = djBlend ? QStringLiteral("_dj_blend") : QString();
            EXPECT_NEAR(crate::curves::transitionGain(
                                Role::Outgoing, progress, kOutgoingGainDb, djBlend),
                    row.value(QStringLiteral("gain_out") + suffix),
                    kGainTolerance);
            EXPECT_NEAR(crate::curves::transitionGain(
                                Role::Incoming, progress, kIncomingGainDb, djBlend),
                    row.value(QStringLiteral("gain_in") + suffix),
                    kGainTolerance);
        }
    }
}

TEST_F(CrateCurvesTest, StagingOnlyLowersATrack) {
    EXPECT_DOUBLE_EQ(crate::curves::stagingGain(0.0), 1.0);
    EXPECT_DOUBLE_EQ(crate::curves::stagingGain(3.0), 1.0);
    EXPECT_NEAR(crate::curves::stagingGain(-6.0), 0.501187, 1e-6);
    EXPECT_DOUBLE_EQ(crate::curves::stagingGain(std::numeric_limits<double>::quiet_NaN()), 1.0);
}

TEST_F(CrateCurvesTest, TempoRestoreIsAStraightLineOverFourSeconds) {
    EXPECT_DOUBLE_EQ(crate::curves::tempoRestore(1.04, 1.0, 0.0), 1.04);
    EXPECT_DOUBLE_EQ(crate::curves::tempoRestore(1.04, 1.0, 2000.0), 1.02);
    EXPECT_DOUBLE_EQ(crate::curves::tempoRestore(1.04, 1.0, 4000.0), 1.0);
    EXPECT_DOUBLE_EQ(crate::curves::tempoRestore(1.04, 1.0, 9000.0), 1.0);
}

} // namespace
