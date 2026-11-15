#include "crate/ui/widgets/levelring.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QPainter>
#include <cmath>
#include <numbers>

#include "crate/ui/skin/crateskin.h"
#include "crate/ui/widgets/instrumentpainter.h"
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

class CrateLevelRingTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_style = InstrumentStyle::fromVariables(skinVariables());
        ASSERT_TRUE(m_style.isValid());
    }

    /// The ring at `value`, magnified `zoom` times: the strokes are
    /// cosmetic and stay as wide, the circle grows, so that the arc can be
    /// measured along it
    QImage render(LevelRing& ring, double value, int zoom) const {
        ring.setValue(value);
        QImage image(ring.size() * zoom, QImage::Format_RGB32);
        image.fill(m_style.background);
        QPainter painter(&image);
        painter.scale(zoom, zoom);
        ring.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
        return image;
    }

    /// Whether `pixel` is mostly the level's accentHi: at least 60 % of it
    /// over the background, as antialiasing leaves a 1.5 px stroke, and
    /// cyan, not the grey of the track
    bool isLevel(const QColor& pixel) const {
        const QColor& level = m_style.accentHi;
        const QColor& background = m_style.background;
        const double share = static_cast<double>(pixel.green() - background.green()) /
                (level.green() - background.green());
        return share >= 0.6 && pixel.green() - pixel.red() > (level.green() - level.red()) / 2;
    }

    /// Whether the ring of `radius` in `image` is lit at `degrees` from 12
    /// o'clock: some pixel across the 1.5 px stroke is the level's colour
    bool isLitAt(const QImage& image, double radius, double degrees) const {
        const QPointF centre(image.width() / 2.0, image.height() / 2.0);
        const double angle = degrees * std::numbers::pi / 180;
        const QPointF direction(std::sin(angle), -std::cos(angle));
        for (const double across : {-1.5, -1.0, -0.5, 0.0, 0.5, 1.0, 1.5}) {
            const QPointF point = centre + direction * (radius + across);
            if (isLevel(image.pixelColor(static_cast<int>(std::floor(point.x())),
                        static_cast<int>(std::floor(point.y()))))) {
                return true;
            }
        }
        return false;
    }

    /// The share of the circle of `radius` that is lit, sampled every half
    /// degree
    double litShare(const QImage& image, double radius) const {
        constexpr int kSamples = 720;
        int lit = 0;
        for (int sample = 0; sample < kSamples; ++sample) {
            lit += isLitAt(image, radius, 360.0 * sample / kSamples) ? 1 : 0;
        }
        return static_cast<double>(lit) / kSamples;
    }

    InstrumentStyle m_style;
};

TEST_F(CrateLevelRingTest, ArcProportionalToValue) {
    LevelRing ring(m_style);
    // The mockup's box: r 7 and 2 px around it
    EXPECT_EQ(ring.sizeHint(), QSize(18, 18));
    ring.resize(ring.sizeHint());

    // From 12 o'clock, clockwise, in sixteenths of a degree
    ring.setValue(0.25);
    EXPECT_EQ(ring.spanAngle(), -90 * 16);
    ring.setValue(0.5);
    EXPECT_EQ(ring.spanAngle(), -180 * 16);
    ring.setValue(1);
    EXPECT_EQ(ring.spanAngle(), -360 * 16);
    ring.setValue(2);
    EXPECT_DOUBLE_EQ(ring.value(), 1);
    // Never shorter than the minimum, and no arc at all for nothing
    ring.setValue(0.01);
    EXPECT_EQ(ring.spanAngle(), levelSpan(m_style.levelRingMinimum, 0));
    ring.setValue(0);
    EXPECT_EQ(ring.spanAngle(), 0);

    constexpr int kZoom = 8;
    const double radius = m_style.levelRingRadius * kZoom;
    // The round caps add half the arc's width at each end
    const double caps = m_style.levelRingWidth / (2 * std::numbers::pi * radius);
    for (const double value : {0.1, 0.25, 0.5, 0.75, 1.0}) {
        const QImage image = render(ring, value, kZoom);
        EXPECT_NEAR(litShare(image, radius), std::min(1.0, value + caps), 0.02) << value;
    }
    EXPECT_EQ(litShare(render(ring, 0, kZoom), radius), 0);

    // A quarter lights the top right of the ring, clockwise, and nothing else
    const QImage quarter = render(ring, 0.25, kZoom);
    EXPECT_TRUE(isLitAt(quarter, radius, 45));
    EXPECT_FALSE(isLitAt(quarter, radius, 135));
    EXPECT_FALSE(isLitAt(quarter, radius, 315));
}

} // namespace
