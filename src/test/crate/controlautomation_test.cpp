#include "crate/smartmix/controlautomation.h"

#include <gtest/gtest.h>

#include <QSignalSpy>

#include "control/controlobject.h"
#include "test/mixxxtest.h"

namespace {

const ConfigKey kKey(QStringLiteral("[Channel1]"), QStringLiteral("pregain"));

class CrateControlAutomationTest : public MixxxTest {
  protected:
    ControlObject m_control{kKey, true, false, false, 1.0};
    crate::ControlAutomation m_automation{kKey};
};

TEST_F(CrateControlAutomationTest, WritesTheLastTargetOnceAtCommit) {
    m_automation.set(0.5);
    m_automation.set(0.7);
    EXPECT_EQ(m_control.get(), 1.0);
    EXPECT_TRUE(m_automation.commit());
    EXPECT_EQ(m_control.get(), 0.7);
    // Nothing pending, and the same value again is no write.
    EXPECT_FALSE(m_automation.commit());
    EXPECT_FALSE(m_automation.write(0.7));
    EXPECT_EQ(m_automation.writeCount(), 1);
}

TEST_F(CrateControlAutomationTest, StopsAtAManualChange) {
    QSignalSpy overridden(&m_automation, &crate::ControlAutomation::overridden);
    ASSERT_TRUE(m_automation.write(0.8));
    // The DJ turns the knob.
    m_control.set(0.6);
    EXPECT_FALSE(m_automation.write(0.9));
    EXPECT_TRUE(m_automation.isOverridden());
    EXPECT_EQ(m_control.get(), 0.6);
    ASSERT_EQ(overridden.count(), 1);
    EXPECT_EQ(overridden.at(0).at(0).value<ConfigKey>(), kKey);
    // Once only.
    EXPECT_TRUE(m_automation.checkOverride());
    EXPECT_EQ(overridden.count(), 1);

    m_automation.reset();
    EXPECT_FALSE(m_automation.isOverridden());
    EXPECT_TRUE(m_automation.write(0.9));
    EXPECT_EQ(m_control.get(), 0.9);
}

TEST_F(CrateControlAutomationTest, AControlNeverWrittenIsNotOverridden) {
    m_control.set(0.4);
    EXPECT_FALSE(m_automation.checkOverride());
    // Taking the value it already has makes it the automation's.
    EXPECT_FALSE(m_automation.write(0.4));
    m_control.set(0.3);
    EXPECT_TRUE(m_automation.checkOverride());
}

TEST_F(CrateControlAutomationTest, AMissingControlIsNeverWritten) {
    crate::ControlAutomation missing(
            ConfigKey(QStringLiteral("[Channel9]"), QStringLiteral("pregain")));
    EXPECT_FALSE(missing.isValid());
    EXPECT_FALSE(missing.write(0.5));
    EXPECT_FALSE(missing.checkOverride());
}

} // namespace
