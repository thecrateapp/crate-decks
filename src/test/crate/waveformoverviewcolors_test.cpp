#include <gtest/gtest.h>

#include <QDomDocument>
#include <QImage>
#include <QPainter>
#include <QSharedPointer>
#include <algorithm>

#include "skin/legacy/skincontext.h"
#include "test/mixxxtest.h"
#include "waveform/renderers/waveformoverviewrenderer.h"
#include "waveform/renderers/waveformsignalcolors.h"
#include "waveform/waveform.h"

namespace {

// The overview's RGB mode paints each column in the mix of the skin's low,
// mid and high colours. Upstream painted the bands themselves as red, green
// and blue, so a deck's overview never took the skin's colours (Decks' deck A
// in its cyans, deck B in its greys).
class CrateWaveformOverviewColorsTest : public MixxxTest {
  protected:
    static constexpr int kSamples = 64;

    WaveformSignalColors colors() const {
        QDomDocument document;
        document.setContent(QStringLiteral(
                "<Overview>"
                "<SignalColor>#06b6d4</SignalColor>"
                "<SignalRGBLowColor>#0e7490</SignalRGBLowColor>"
                "<SignalRGBMidColor>#06b6d4</SignalRGBMidColor>"
                "<SignalRGBHighColor>#67e8f9</SignalRGBHighColor>"
                "</Overview>"));
        WaveformSignalColors signalColors;
        const SkinContext context(config(), QString());
        signalColors.setup(document.documentElement(), context);
        return signalColors;
    }

    // A waveform whose every sample has only the given bands
    static ConstWaveformPointer waveformOf(
            unsigned char low, unsigned char mid, unsigned char high) {
        auto pWaveform = QSharedPointer<Waveform>::create(44100, 44100, 441, kSamples * 2, 0);
        for (int i = 0; i < pWaveform->getDataSize(); ++i) {
            WaveformFilteredData& data = pWaveform->data()[i].filtered;
            data.low = low;
            data.mid = mid;
            data.high = high;
            data.all = 200;
        }
        return pWaveform;
    }

    // The colour of the column in the middle of the drawing, just above the axis
    static QColor paintedColor(const ConstWaveformPointer& pWaveform,
            const WaveformSignalColors& signalColors) {
        QImage image(pWaveform->getDataSize() / 2, 2 * 255, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.translate(0.0, image.height() / 2.0);
            waveformOverviewRenderer::drawWaveformPartRGB(
                    &painter, pWaveform, nullptr, pWaveform->getDataSize(), signalColors);
        }
        return image.pixelColor(image.width() / 2, image.height() / 2 - 2);
    }

    // `color` scaled so that its brightest channel is full, as the renderer
    // normalises each column
    static QColor normalized(const QColor& color) {
        const double max = std::max({color.redF(), color.greenF(), color.blueF()});
        QColor out;
        out.setRgbF(static_cast<float>(color.redF() / max),
                static_cast<float>(color.greenF() / max),
                static_cast<float>(color.blueF() / max));
        return out;
    }

    static void expectNear(const QColor& actual, const QColor& expected) {
        EXPECT_NEAR(actual.red(), expected.red(), 2) << actual.name().toStdString();
        EXPECT_NEAR(actual.green(), expected.green(), 2) << actual.name().toStdString();
        EXPECT_NEAR(actual.blue(), expected.blue(), 2) << actual.name().toStdString();
    }
};

TEST_F(CrateWaveformOverviewColorsTest, LowsTakeTheSkinsLowColor) {
    const WaveformSignalColors signalColors = colors();
    expectNear(paintedColor(waveformOf(200, 0, 0), signalColors),
            normalized(signalColors.getRgbLowColor()));
}

TEST_F(CrateWaveformOverviewColorsTest, HighsTakeTheSkinsHighColor) {
    const WaveformSignalColors signalColors = colors();
    expectNear(paintedColor(waveformOf(0, 0, 200), signalColors),
            normalized(signalColors.getRgbHighColor()));
}

TEST_F(CrateWaveformOverviewColorsTest, NeverPaintsTheBandsAsRedGreenAndBlue) {
    // Only lows: upstream painted pure red
    const QColor lows = paintedColor(waveformOf(200, 0, 0), colors());
    EXPECT_LT(lows.red(), lows.blue());
    EXPECT_GT(lows.green(), 0);
}

} // namespace
