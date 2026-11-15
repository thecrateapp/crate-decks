#include "crate/ui/widgets/instrumentpainter.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QPainter>
#include <cmath>

#include "crate/ui/skin/crateskin.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"

namespace {

using namespace crate::widgets;

/// The values of the CrateDecks skin, as the skin loads them
crate::SkinVariables skinVariables() {
    return crate::readSkinVariables(
            QDir(ConfigObject<ConfigValue>::computeResourcePath())
                    .filePath(QStringLiteral("skins/CrateDecks/generated/vars.xml")));
}

/// A dash pattern as tokens.json writes it ("9 4"), in pixels
QVector<qreal> patternOf(const QString& text) {
    QVector<qreal> pattern;
    const QStringList parts = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        pattern.append(part.toDouble());
    }
    return pattern;
}

class CrateInstrumentPainterTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_variables = skinVariables();
        m_style = InstrumentStyle::fromVariables(m_variables);
        ASSERT_TRUE(m_style.isValid());
    }

    crate::SkinVariables m_variables;
    InstrumentStyle m_style;
};

TEST_F(CrateInstrumentPainterTest, HairlineIsCosmeticOnePixel) {
    const QPen pen = hairlinePen(m_style.textPrimary, m_style);
    EXPECT_TRUE(pen.isCosmetic());
    EXPECT_DOUBLE_EQ(m_style.strokeHairline, 1.0);
    EXPECT_DOUBLE_EQ(pen.widthF(), m_style.strokeHairline);

    // Drawn under a scale and at a whole coordinate, which would straddle
    // two pixel rows, the hairline stays one crisp device pixel: it does
    // not scale and it is aligned to the middle of a pixel
    const QColor background = m_style.background;
    QImage image(QSize(64, 64), QImage::Format_RGB32);
    image.fill(background);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(3, 3);
        drawHairline(&painter, QLineF(2, 10, 18, 10), m_style.textPrimary, m_style);
    }
    int paintedRows = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb pixel = image.pixel(32, y);
        if (pixel == background.rgb()) {
            continue;
        }
        ++paintedRows;
        EXPECT_EQ(pixel, m_style.textPrimary.rgb()) << "row " << y;
    }
    EXPECT_EQ(paintedRows, 1);
}

TEST_F(CrateInstrumentPainterTest, TickRingHasLongTickEveryFour) {
    EXPECT_EQ(m_style.tickCount, 48);
    EXPECT_EQ(m_style.tickEvery, 4);
    const QPointF centre(100, 100);
    const double radius = 40;
    const QVector<Tick> ticks = tickRing(centre,
            radius,
            m_style.tickCount,
            m_style.tickEvery,
            m_style.tickLong,
            m_style.tickShort);
    ASSERT_EQ(ticks.size(), 48);
    int longTicks = 0;
    for (int index = 0; index < ticks.size(); ++index) {
        const Tick& tick = ticks.at(index);
        const bool shouldBeLong = index % 4 == 0;
        EXPECT_EQ(tick.isLong, shouldBeLong) << "tick " << index;
        EXPECT_NEAR(tick.line.length(),
                shouldBeLong ? m_style.tickLong : m_style.tickShort,
                1e-9)
                << "tick " << index;
        // Outwards from the ring
        EXPECT_NEAR(QLineF(centre, tick.line.p1()).length(), radius, 1e-9);
        longTicks += tick.isLong ? 1 : 0;
    }
    EXPECT_EQ(longTicks, 12);
    // The first tick is a long one at 12 o'clock, and they go clockwise
    EXPECT_NEAR(ticks.at(0).line.p1().x(), centre.x(), 1e-9);
    EXPECT_LT(ticks.at(0).line.p1().y(), centre.y());
    EXPECT_GT(ticks.at(1).line.p1().x(), centre.x());
}

TEST_F(CrateInstrumentPainterTest, DashPatternsMatchTokens) {
    const struct {
        StrokePattern pattern;
        const char* variable;
    } kPatterns[] = {
            {StrokePattern::Data, "CrateInstDashData"},
            {StrokePattern::Queue, "CrateInstDashQueue"},
            {StrokePattern::Telemetry, "CrateInstDashTelemetry"},
            {StrokePattern::Axis, "CrateInstDashAxis"},
            {StrokePattern::Spoke, "CrateInstDashSpoke"},
    };
    for (const auto& expected : kPatterns) {
        const QVector<qreal> tokenPattern =
                patternOf(m_variables.value(QLatin1String(expected.variable)));
        ASSERT_EQ(tokenPattern.size(), 2) << expected.variable;
        EXPECT_EQ(dashPattern(expected.pattern, m_style), tokenPattern) << expected.variable;

        // Qt measures dashes in pen widths, the tokens in pixels as SVG
        // does: a pen of any width draws the token's pixels
        for (const double width : {m_style.strokeHairline, m_style.strokeConnection}) {
            const QPen pen = patternPen(expected.pattern, m_style.textMuted, width, m_style);
            EXPECT_EQ(pen.style(), Qt::CustomDashLine);
            const QVector<qreal> penPattern = pen.dashPattern();
            ASSERT_EQ(penPattern.size(), tokenPattern.size());
            for (int index = 0; index < penPattern.size(); ++index) {
                EXPECT_NEAR(penPattern.at(index) * width, tokenPattern.at(index), 1e-9)
                        << expected.variable << " at width " << width;
            }
        }
    }
    // The request (tempo) pattern is a solid line
    EXPECT_TRUE(dashPattern(StrokePattern::Solid, m_style).isEmpty());
    EXPECT_EQ(patternPen(StrokePattern::Solid, m_style.textMuted, 1, m_style).style(),
            Qt::SolidLine);
}

} // namespace
