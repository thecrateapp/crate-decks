#include "crate/recs/harmony.h"

#include <gtest/gtest.h>

#include <QString>

// The Harmony column of the Compatibles view (plan V3.PR2): the relation of
// a track's Camelot key to the followed track's, read from the two keys.

namespace {

using crate::recs::Harmony;
using crate::recs::harmonyLabel;
using crate::recs::harmonyOf;

QString key(int number, char ring) {
    return QString::number(number) + QLatin1Char(ring);
}

/// The number `steps` round the ring from `number`, 1 to 12.
int step(int number, int steps) {
    return ((number - 1 + steps) % 12 + 12) % 12 + 1;
}

TEST(CompatiblesHarmonyTest, EveryKeyAgainstEveryKey) {
    // Table: for each of the 24 keys, what each of the 24 keys is to it
    for (const char seedRing : {'A', 'B'}) {
        for (int seed = 1; seed <= 12; ++seed) {
            for (const char ring : {'A', 'B'}) {
                for (int number = 1; number <= 12; ++number) {
                    Harmony expected = Harmony::Other;
                    if (ring != seedRing) {
                        expected = number == seed ? Harmony::Relative : Harmony::Other;
                    } else if (number == seed) {
                        expected = Harmony::Same;
                    } else if (number == step(seed, 1)) {
                        expected = Harmony::Up;
                    } else if (number == step(seed, -1)) {
                        expected = Harmony::Down;
                    } else if (number == step(seed, 7)) {
                        expected = Harmony::EnergyBoost;
                    }
                    EXPECT_EQ(expected, harmonyOf(key(seed, seedRing), key(number, ring)))
                            << key(seed, seedRing).toStdString() << " -> "
                            << key(number, ring).toStdString();
                }
            }
        }
    }
}

TEST(CompatiblesHarmonyTest, WrapsRoundTheWheel) {
    const struct {
        const char* seed;
        const char* key;
        Harmony harmony;
    } cases[] = {
            {"12A", "1A", Harmony::Up},
            {"1A", "12A", Harmony::Down},
            {"12B", "1B", Harmony::Up},
            {"1B", "12B", Harmony::Down},
            {"8A", "3A", Harmony::EnergyBoost},
            {"6B", "1B", Harmony::EnergyBoost},
            {"5A", "12A", Harmony::EnergyBoost},
            {"8A", "8B", Harmony::Relative},
            {"8A", "9B", Harmony::Other},
            {"8A", "1A", Harmony::Other},
    };
    for (const auto& item : cases) {
        EXPECT_EQ(item.harmony,
                harmonyOf(QString::fromLatin1(item.seed), QString::fromLatin1(item.key)))
                << item.seed << " -> " << item.key;
    }
}

TEST(CompatiblesHarmonyTest, UnreadableKeysAreUnknown) {
    for (const char* text : {"", "8", "A", "13A", "0B", "8C", "Am", "x8A"}) {
        EXPECT_EQ(Harmony::Unknown, harmonyOf(QStringLiteral("8A"), QString::fromLatin1(text)))
                << text;
        EXPECT_EQ(Harmony::Unknown, harmonyOf(QString::fromLatin1(text), QStringLiteral("8A")))
                << text;
    }
    // Case and blanks do not matter
    EXPECT_EQ(Harmony::Up, harmonyOf(QStringLiteral(" 8a"), QStringLiteral("9A ")));
}

TEST(CompatiblesHarmonyTest, LabelsAreTheMockups) {
    EXPECT_EQ(QStringLiteral("Same key"), harmonyLabel(Harmony::Same));
    EXPECT_EQ(QStringLiteral("+1"), harmonyLabel(Harmony::Up));
    EXPECT_EQ(QStringLiteral("−1"), harmonyLabel(Harmony::Down));
    EXPECT_EQ(QStringLiteral("Relative"), harmonyLabel(Harmony::Relative));
    EXPECT_EQ(QStringLiteral("Energy +7"), harmonyLabel(Harmony::EnergyBoost));
    EXPECT_EQ(QStringLiteral("—"), harmonyLabel(Harmony::Other));
    EXPECT_TRUE(harmonyLabel(Harmony::Unknown).isEmpty());
    EXPECT_TRUE(crate::recs::isHarmonic(Harmony::EnergyBoost));
    EXPECT_FALSE(crate::recs::isHarmonic(Harmony::Other));
    EXPECT_FALSE(crate::recs::isHarmonic(Harmony::Unknown));
}

TEST(CompatiblesHarmonyTest, StepsRoundTheRing) {
    EXPECT_EQ(QStringLiteral("9A"), crate::recs::camelotStep(QStringLiteral("8A"), 1));
    EXPECT_EQ(QStringLiteral("12A"), crate::recs::camelotStep(QStringLiteral("1A"), -1));
    EXPECT_EQ(QStringLiteral("3A"), crate::recs::camelotStep(QStringLiteral("8A"), 7));
    EXPECT_EQ(QStringLiteral("8B"), crate::recs::camelotStep(QStringLiteral("8A"), 0, true));
    EXPECT_TRUE(crate::recs::camelotStep(QStringLiteral("Am"), 1).isEmpty());
}

} // namespace
