#include "crate/smartmix/eqmap.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <cmath>
#include <numbers>

#include "control/controlobject.h"
#include "effects/backends/builtin/bessel4lvmixeqeffect.h"
#include "effects/chains/equalizereffectchain.h"
#include "effects/defs.h"
#include "effects/effectslot.h"
#include "test/crate/wavwriter.h"
#include "test/signalpathtest.h"

namespace {

constexpr int kSampleRate = 44100;
// -12 dBFS: room for the EQ's boosts and no clipping in the main mix.
constexpr double kAmplitude = 0.25 * 32767.0;
// Buffers played before measuring, so the EQ's parameter ramps and filters
// settle, and buffers measured: about 0.3 s and 0.5 s.
constexpr int kSettleBuffers = 24;
constexpr int kMeasureBuffers = 40;

/// F0 Q6, measured instead of heard: a deck plays a sine through the real
/// engine with the default mixing EQ loaded in its equalizer rack, and the
/// level of the main output shows what each EQ control does. The default
/// crossovers are the app's (246 Hz and 2484 Hz).
class CrateEqMapTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();
        ControlObject::set(ConfigKey(kMixerProfile, kLowEqFrequency), 246.0);
        ControlObject::set(ConfigKey(kMixerProfile, kHighEqFrequency), 2484.0);
        // PlayerManager::addDeck() does this in the app.
        m_pEffectsManager->addDeck(ChannelHandleAndGroup(m_pChannel1->getHandle(), m_sGroup1));
        // Loads the default EQ into the rack.
        m_pEffectsManager->setup();
        ASSERT_TRUE(m_tracks.isValid());
    }

    /// Plays a sine of `hz` on deck 1 and returns the main output's RMS level
    /// with the EQ's controls at their defaults.
    void loadSine(double hz) {
        const QString path = m_tracks.filePath(QStringLiteral("sine-%1.wav").arg(hz));
        crate::test::writeWav(path, kSampleRate, 20 * kSampleRate, [hz](SINT frame, int) {
            return static_cast<int>(std::lround(kAmplitude *
                    std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(frame) /
                            kSampleRate)));
        });
        loadTrack(m_pMixerDeck1.get(), Track::newTemporary(path));
        ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("play")), 1.0);
    }

    static ConfigKey eqKey(const QString& item) {
        return ConfigKey(crate::eqmap::effectGroup(m_sGroup1), item);
    }

    /// The main output's level in dB relative to the EQ at neutral, with
    /// `item` set to `value` and every other control at its default.
    double levelDb(const QString& item, double value) {
        resetEq();
        const double reference = rmsOfMainOutput();
        ControlObject::set(eqKey(item), value);
        const double level = rmsOfMainOutput();
        resetEq();
        return 20.0 * std::log10(level / reference);
    }

    void resetEq() {
        for (const QString& item : {crate::eqmap::kLowGain,
                     crate::eqmap::kMidGain,
                     crate::eqmap::kHighGain}) {
            ControlObject::set(eqKey(item), crate::eqmap::kNeutralGain);
        }
        for (const QString& item : {crate::eqmap::kLowKill,
                     crate::eqmap::kMidKill,
                     crate::eqmap::kHighKill}) {
            ControlObject::set(eqKey(item), 0.0);
        }
    }

    double rmsOfMainOutput() {
        for (int i = 0; i < kSettleBuffers; ++i) {
            ProcessBuffer();
        }
        double sum = 0.0;
        std::size_t samples = 0;
        for (int i = 0; i < kMeasureBuffers; ++i) {
            ProcessBuffer();
            for (const CSAMPLE sample : m_pEngineMixer->getMainBuffer()) {
                sum += static_cast<double>(sample) * sample;
            }
            samples += m_pEngineMixer->getMainBuffer().size();
        }
        return std::sqrt(sum / static_cast<double>(samples));
    }

    QTemporaryDir m_tracks;
};

TEST_F(CrateEqMapTest, DefaultEqIsLoadedInTheRack) {
    const ControlObject* pLoaded = ControlObject::getControl(
            ConfigKey(crate::eqmap::effectGroup(m_sGroup1), QStringLiteral("loaded")));
    ASSERT_NE(pLoaded, nullptr);
    EXPECT_EQ(pLoaded->get(), 1.0);
}

// The bass handoff needs the EQ that was measured, loaded and on (F6.C-PR3):
// not with the preferences' "Bypass EQ", nor with another EQ.
TEST_F(CrateEqMapTest, DefaultEqIsOnOnlyWhenLoadedAndEnabled) {
    EXPECT_TRUE(crate::eqmap::isDefaultEqOn(*m_pEffectsManager, m_sGroup1));
    EXPECT_FALSE(crate::eqmap::isDefaultEqOn(*m_pEffectsManager, QStringLiteral("[Channel9]")));
    const EffectSlotPointer pSlot =
            m_pEffectsManager->getEqualizerEffectChain(m_sGroup1)->getEffectSlot(0);
    pSlot->setEnabled(false);
    EXPECT_FALSE(crate::eqmap::isDefaultEqOn(*m_pEffectsManager, m_sGroup1));
    pSlot->setEnabled(true);
    pSlot->loadEffectWithDefaults(m_pEffectsManager->getBackendManager()->getManifest(
            Bessel4LVMixEQEffect::getId(), EffectBackendType::BuiltIn));
    EXPECT_FALSE(crate::eqmap::isDefaultEqOn(*m_pEffectsManager, m_sGroup1));
    pSlot->loadEffectWithDefaults(EffectManifestPointer());
    EXPECT_FALSE(crate::eqmap::isDefaultEqOn(*m_pEffectsManager, m_sGroup1));
}

// Measured: parameter1 at 0 -49.1 dB, button_parameter1 -49.1 dB; the mid
// and high controls -0.6 dB at most.
TEST_F(CrateEqMapTest, LowGainAndLowKillCutTheLows) {
    loadSine(60.0);
    EXPECT_LT(levelDb(crate::eqmap::kLowGain, 0.0), -40.0);
    EXPECT_LT(levelDb(crate::eqmap::kLowKill, 1.0), -40.0);
    EXPECT_GT(levelDb(crate::eqmap::kMidGain, 0.0), -1.0);
    EXPECT_GT(levelDb(crate::eqmap::kMidKill, 1.0), -1.0);
    EXPECT_GT(levelDb(crate::eqmap::kHighGain, 0.0), -1.0);
    EXPECT_GT(levelDb(crate::eqmap::kHighKill, 1.0), -1.0);
}

// Measured: parameter1 and button_parameter1 -0.03 dB; parameter3 at 0 and
// button_parameter3 -28.2 dB (the high kill is not as deep as the low one).
TEST_F(CrateEqMapTest, LowControlsLeaveTheHighs) {
    loadSine(5000.0);
    EXPECT_GT(levelDb(crate::eqmap::kLowGain, 0.0), -0.5);
    EXPECT_GT(levelDb(crate::eqmap::kLowKill, 1.0), -0.5);
    EXPECT_LT(levelDb(crate::eqmap::kHighGain, 0.0), -20.0);
    EXPECT_LT(levelDb(crate::eqmap::kHighKill, 1.0), -20.0);
}

// The director's bass handoff asks the EQ for Android's cuts; the knob it
// gets from lowGainForCutDb() must give them.
TEST_F(CrateEqMapTest, LowGainForCutDbGivesTheCut) {
    loadSine(60.0);
    for (const double cutDb : {-3.0, -6.0, -12.0, -18.0, -24.0}) {
        EXPECT_NEAR(levelDb(crate::eqmap::kLowGain, crate::eqmap::lowGainForCutDb(cutDb)),
                cutDb,
                1.0)
                << cutDb << " dB";
    }
    // Above neutral the knob is the shelf's gain, which a 60 Hz sine meets
    // a little under (+5.4 to +5.8 dB measured, by build and run). The
    // director only uses boosts to give the DJ's own knob back, which
    // CutDbForLowGainInvertsLowGainForCutDb checks exactly.
    EXPECT_NEAR(levelDb(crate::eqmap::kLowGain, crate::eqmap::lowGainForCutDb(6.0)), 6.0, 1.0);
}

TEST(CrateEqMapCurveTest, CutDbForLowGainInvertsLowGainForCutDb) {
    for (const double cutDb : {6.0, 0.0, -2.0, -4.17, -10.0, -24.0, -40.0}) {
        EXPECT_NEAR(crate::eqmap::cutDbForLowGain(crate::eqmap::lowGainForCutDb(cutDb)),
                cutDb,
                1e-9);
    }
    EXPECT_EQ(crate::eqmap::lowGainForCutDb(0.0), crate::eqmap::kNeutralGain);
    EXPECT_EQ(crate::eqmap::lowGainForCutDb(-60.0), 0.0);
}

} // namespace
