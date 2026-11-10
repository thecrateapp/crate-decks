#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QPainter>

#include "crate/ui/skin/crateskin.h"
#include "crate/ui/skin/mixerstyle.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"

namespace {

/// The values of the CrateDecks skin, as the skin loads them
crate::SkinVariables skinVariables() {
    return crate::readSkinVariables(
            QDir(ConfigObject<ConfigValue>::computeResourcePath())
                    .filePath(QStringLiteral("skins/CrateDecks/generated/vars.xml")));
}

QColor colourOf(const crate::SkinVariables& variables, const QString& name) {
    return QColor(variables.value(name));
}

/// A colour painted over the background, as the screen shows it
QColor over(const QColor& colour, const QColor& background) {
    const double alpha = colour.alphaF();
    return QColor::fromRgbF(
            static_cast<float>(colour.redF() * alpha + background.redF() * (1 - alpha)),
            static_cast<float>(colour.greenF() * alpha + background.greenF() * (1 - alpha)),
            static_cast<float>(colour.blueF() * alpha + background.blueF() * (1 - alpha)));
}

bool near(QRgb pixel, const QColor& expected, int tolerance = 3) {
    return std::abs(qRed(pixel) - expected.red()) <= tolerance &&
            std::abs(qGreen(pixel) - expected.green()) <= tolerance &&
            std::abs(qBlue(pixel) - expected.blue()) <= tolerance;
}

class CrateSkinMixerTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_variables = skinVariables();
        m_background = colourOf(m_variables, QStringLiteral("CrateBgApp"));
        m_fader = crate::FaderStyle::fromVariables(m_variables);
        m_meter = crate::LevelMeterStyle::fromVariables(m_variables);
        ASSERT_TRUE(m_fader.isValid());
        ASSERT_TRUE(m_meter.isValid());
    }

    QImage canvas(QSize size) const {
        QImage image(size, QImage::Format_RGB32);
        image.fill(m_background);
        return image;
    }

    crate::SkinVariables m_variables;
    QColor m_background;
    crate::FaderStyle m_fader;
    crate::LevelMeterStyle m_meter;
};

TEST_F(CrateSkinMixerTest, HandleEdgeIsWhereWSliderComposedDrawsIt) {
    // round((1 - parameter) * (length - handle)) on a vertical fader
    EXPECT_EQ(0, crate::faderHandleEdge(200, 11, 1.0, false));
    EXPECT_EQ(189, crate::faderHandleEdge(200, 11, 0.0, false));
    EXPECT_EQ(95, crate::faderHandleEdge(200, 11, 0.5, false));
    EXPECT_EQ(189, crate::faderHandleEdge(200, 11, 1.0, true));
    EXPECT_EQ(0, crate::faderHandleEdge(5, 11, 0.3, false));
}

TEST_F(CrateSkinMixerTest, FaderTicksAreCrispAtTheHandleMiddle) {
    // A channel fader 30 px across, stretched to 200: the GU1 mockups put
    // the rail on 15.5 and a tick every 10 % 11 px either side of it
    const QSize size(30, 200);
    QImage image = canvas(size);
    {
        QPainter painter(&image);
        crate::paintFaderScale(&painter, size, false, m_fader);
    }
    const QColor rail = over(m_fader.rail, m_background);
    const QColor tick = m_fader.tick;
    const QColor centre = m_fader.centreTick;
    // The rail between the middles of the handle at both ends
    EXPECT_TRUE(near(image.pixel(15, 100), rail));
    EXPECT_TRUE(near(image.pixel(15, 2), m_background));
    for (int step = 0; step <= m_fader.tickSteps; ++step) {
        const double parameter = static_cast<double>(step) / m_fader.tickSteps;
        const int row = crate::faderHandleEdge(200, m_fader.handleLength, parameter, false) +
                static_cast<int>(m_fader.handleLength / 2);
        const bool isCentre = 2 * step == m_fader.tickSteps;
        // Whole pixels on both sides of the handle, none above or below
        EXPECT_TRUE(near(image.pixel(3, row), isCentre ? centre : tick)) << step;
        EXPECT_TRUE(near(image.pixel(27, row), isCentre ? centre : tick)) << step;
        EXPECT_TRUE(near(image.pixel(3, row - 1), m_background)) << step;
        EXPECT_TRUE(near(image.pixel(3, row + 1), m_background)) << step;
        // The centre tick is the long one
        EXPECT_EQ(isCentre, near(image.pixel(0, row), centre)) << step;
    }
}

TEST_F(CrateSkinMixerTest, ActiveHandleFillsWithTheFaderColour) {
    const QSize size(30, 200);
    const QColor deck = colourOf(m_variables, QStringLiteral("CrateDeckBD"));
    QImage image = canvas(size);
    {
        QPainter painter(&image);
        crate::paintActiveFaderHandle(&painter, size, false, 0.5, deck, m_fader);
    }
    const int edge = crate::faderHandleEdge(200, m_fader.handleLength, 0.5, false);
    const int middle = edge + static_cast<int>(m_fader.handleLength / 2);
    // Filled 18 x 10 around the rail, with the onAccent line through it
    EXPECT_TRUE(near(image.pixel(10, middle - 3), deck));
    EXPECT_TRUE(near(image.pixel(22, middle + 3), deck));
    EXPECT_TRUE(near(image.pixel(15, middle), m_fader.activeLine));
    EXPECT_TRUE(near(image.pixel(15, edge - 2), m_background));
    EXPECT_TRUE(near(image.pixel(3, middle), m_background));
}

TEST_F(CrateSkinMixerTest, MeterTurnsToWarningAboveItsLevel) {
    const QSize size(4, 102);
    const QColor deck = colourOf(m_variables, QStringLiteral("CrateDeckAC"));
    const QColor track = over(m_meter.track, m_background);
    const int scale = size.height() - m_meter.topGap;
    const int warningTop = size.height() - qRound(scale * m_meter.warningLevel);
    // -6 dB on Mixxx's meter scale is about three quarters of it
    EXPECT_NEAR(0.74, m_meter.warningLevel, 0.01);

    QImage image = canvas(size);
    {
        QPainter painter(&image);
        crate::paintLevelMeter(&painter, size, 0.95, 0.98, deck, m_meter);
    }
    EXPECT_TRUE(near(image.pixel(1, size.height() - 1), deck));
    EXPECT_TRUE(near(image.pixel(1, warningTop + 2), deck));
    EXPECT_TRUE(near(image.pixel(1, warningTop - 2), m_meter.warning));
    // The background's 1 px rule at the warning level
    EXPECT_TRUE(near(image.pixel(1, warningTop), m_meter.warningRule));
    // The gap under the clip segment is never lit
    EXPECT_TRUE(near(image.pixel(1, 0), track));
    // A 1 px peak hold in warning, over the track
    const int peakTop = size.height() - qRound(scale * 0.98);
    EXPECT_TRUE(near(image.pixel(1, peakTop), m_meter.warning));
    EXPECT_TRUE(near(image.pixel(1, peakTop - 1), track));

    // Below the warning level, bar and peak hold are the meter's colour
    image = canvas(size);
    {
        QPainter painter(&image);
        crate::paintLevelMeter(&painter, size, 0.3, 0.5, deck, m_meter);
    }
    const int levelTop = size.height() - qRound(scale * 0.3);
    EXPECT_TRUE(near(image.pixel(1, levelTop), deck));
    EXPECT_TRUE(near(image.pixel(1, levelTop - 1), track));
    EXPECT_TRUE(near(image.pixel(1, size.height() - qRound(scale * 0.5)), deck));
}

} // namespace
