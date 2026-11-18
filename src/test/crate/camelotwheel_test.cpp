#include "crate/ui/widgets/camelotwheel.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTest>
#include <cmath>

#include "crate/ui/skin/crateskin.h"
#include "crate/ui/widgets/camelotlegend.h"
#include "crate/ui/widgets/camelotwheelmodel.h"
#include "crate/ui/widgets/instrumentpainter.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"
#include "track/keyutils.h"

namespace {

using namespace crate::widgets;
using mixxx::track::io::key::ChromaticKey;

/// The values of the CrateDecks skin, as the skin loads them
crate::SkinVariables skinVariables() {
    return crate::readSkinVariables(
            QDir(ConfigObject<ConfigValue>::computeResourcePath())
                    .filePath(QStringLiteral("skins/CrateDecks/generated/vars.xml")));
}

ChromaticKey key(const QString& camelot) {
    return KeyUtils::guessKeyFromText(camelot);
}

/// How much of `colour` over `background` makes `pixel`: 0 is the
/// background, 1 the colour itself, and a negative value a pixel that is
/// not a shade of that colour (antialiased strokes are shades).
double shareOf(QRgb pixel, const QColor& colour, const QColor& background) {
    const double target[3] = {colour.redF(), colour.greenF(), colour.blueF()};
    const double base[3] = {background.redF(), background.greenF(), background.blueF()};
    const double value[3] = {qRed(pixel) / 255.0, qGreen(pixel) / 255.0, qBlue(pixel) / 255.0};
    double dot = 0;
    double length = 0;
    for (int channel = 0; channel < 3; ++channel) {
        dot += (value[channel] - base[channel]) * (target[channel] - base[channel]);
        length += (target[channel] - base[channel]) * (target[channel] - base[channel]);
    }
    if (length <= 0) {
        return -1;
    }
    const double share = dot / length;
    double residual = 0;
    for (int channel = 0; channel < 3; ++channel) {
        const double expected = base[channel] + share * (target[channel] - base[channel]);
        residual = std::max(residual, std::abs(expected - value[channel]));
    }
    return residual <= 4 / 255.0 ? share : -1;
}

/// The strongest shade of `colour` within `radius` of `centre`, or -1
double strongestShade(const QImage& image,
        QPointF centre,
        double radius,
        const QColor& colour,
        const QColor& background) {
    double strongest = -1;
    const QRect area = QRectF(centre.x() - radius, centre.y() - radius, 2 * radius, 2 * radius)
                               .toAlignedRect()
                               .intersected(image.rect());
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (QLineF(centre, QPointF(x + 0.5, y + 0.5)).length() > radius) {
                continue;
            }
            const QRgb pixel = image.pixel(x, y);
            if (pixel == background.rgb()) {
                continue;
            }
            strongest = std::max(strongest, shareOf(pixel, colour, background));
        }
    }
    return strongest;
}

class CrateCamelotWheelTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_style = InstrumentStyle::fromVariables(skinVariables());
        ASSERT_TRUE(m_style.isValid());
        m_pModel = std::make_unique<CamelotWheelModel>(m_style.opacityContext);
        m_pWheel = std::make_unique<CamelotWheel>(m_pModel.get(), m_style);
        // The size of the GU1 mockup (library-compatibles)
        m_pWheel->resize(m_pWheel->sizeHint());
    }

    void TearDown() override {
        m_pWheel.reset();
        m_pModel.reset();
    }

    QImage render() const {
        QImage image(m_pWheel->size(), QImage::Format_RGB32);
        image.fill(m_style.background);
        m_pWheel->render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
        return image;
    }

    QPointF centreOf(const QString& camelot) const {
        return m_pWheel->nodeCentre(m_pModel->indexOf(key(camelot)));
    }

    /// The node's own area: inside its stroke, before its label
    double nodeArea() const {
        return m_style.nodeSelected + m_style.strokeSelected;
    }

    InstrumentStyle m_style;
    std::unique_ptr<CamelotWheelModel> m_pModel;
    std::unique_ptr<CamelotWheel> m_pWheel;
};

TEST_F(CrateCamelotWheelTest, PaintsTheMockupsWheel) {
    EXPECT_EQ(m_pWheel->sizeHint(), QSize(470, 470));
    m_pModel->setMasterKey(key(QStringLiteral("8A")));
    m_pModel->setKeysInResults({key(QStringLiteral("5A")), key(QStringLiteral("10A"))});
    const QImage image = render();
    const QColor background = m_style.background;

    // The master deck's key: filled in accent
    const QPointF master = centreOf(QStringLiteral("8A"));
    EXPECT_EQ(image.pixelColor(master.toPoint()), m_style.accent);
    EXPECT_GT(strongestShade(image, master, nodeArea(), m_style.accentHi, background), 0.95);

    // Compatible keys: an accentHi stroke around a hollow node
    for (const QString& compatible : {QStringLiteral("7A"),
                 QStringLiteral("9A"),
                 QStringLiteral("8B"),
                 QStringLiteral("3A")}) {
        const QPointF centre = centreOf(compatible);
        EXPECT_EQ(image.pixelColor(centre.toPoint()), background) << compatible.toStdString();
        EXPECT_GT(strongestShade(image, centre, nodeArea(), m_style.accentHi, background), 0.95)
                << compatible.toStdString();
    }

    // In the results: at rest, a full textPrimary stroke
    const double rest = strongestShade(
            image, centreOf(QStringLiteral("5A")), nodeArea(), m_style.textPrimary, background);
    EXPECT_GT(rest, 0.95);

    // Unrelated and without results: context at a quarter
    for (const QString& context : {QStringLiteral("2A"), QStringLiteral("12B")}) {
        const double shade = strongestShade(
                image, centreOf(context), nodeArea(), m_style.textPrimary, background);
        EXPECT_NEAR(shade, m_style.opacityContext, 0.05) << context.toStdString();
        EXPECT_NEAR(shade / rest, 0.25, 0.05) << context.toStdString();
    }
}

TEST_F(CrateCamelotWheelTest, WithoutMasterEveryNodeRests) {
    const QImage image = render();
    for (const CamelotWheelModel::Node& node : m_pModel->nodes()) {
        const QPointF centre = m_pWheel->nodeCentre(m_pModel->indexOf(node.key));
        EXPECT_EQ(image.pixelColor(centre.toPoint()), m_style.background)
                << node.camelot.toStdString();
        EXPECT_GT(strongestShade(image,
                          centre,
                          nodeArea(),
                          m_style.textPrimary,
                          m_style.background),
                0.95)
                << node.camelot.toStdString();
        EXPECT_LT(strongestShade(
                          image, centre, nodeArea(), m_style.accentHi, m_style.background),
                0.5)
                << node.camelot.toStdString();
    }
}

TEST_F(CrateCamelotWheelTest, ClickOnNodeEmitsItsKey) {
    m_pModel->setMasterKey(key(QStringLiteral("8A")));
    QSignalSpy clicked(m_pWheel.get(), &CamelotWheel::keyClicked);
    QTest::mouseClick(m_pWheel.get(),
            Qt::LeftButton,
            Qt::NoModifier,
            centreOf(QStringLiteral("5A")).toPoint());
    ASSERT_EQ(clicked.count(), 1);
    EXPECT_EQ(clicked.at(0).at(0).value<ChromaticKey>(), key(QStringLiteral("5A")));

    // The centre figure and the space between rings are not keys
    QTest::mouseClick(m_pWheel.get(),
            Qt::LeftButton,
            Qt::NoModifier,
            m_pWheel->rect().center());
    EXPECT_EQ(clicked.count(), 1);
    EXPECT_EQ(m_pWheel->nodeAt(QPointF(m_pWheel->rect().center())), -1);
}

TEST_F(CrateCamelotWheelTest, FigureKeepsClearOfTheLabelsBesideIt) {
    m_pModel->setMasterKey(key(QStringLiteral("8A")));
    const QFontMetricsF metrics(
            m_style.monoFont(m_style.fontSizeWheelLabel, m_style.trackingWheelLabel));
    // The labels of 3A and 9A run inwards, level with the figure
    const double offset = m_style.nodeRoot + m_style.camelotLabelGap;
    const double baseline = m_pWheel->height() / 2.0 + m_style.camelotFigureBaseline;
    const int top = qRound(baseline - 0.7 * m_style.fontSizeDisplay);
    const int bottom = qRound(baseline);
    const auto blank = [&](const QImage& image, double from, double to) {
        for (int x = qRound(from); x <= qRound(to); ++x) {
            for (int y = top; y <= bottom; ++y) {
                if (image.pixelColor(x, y) != m_style.background) {
                    return false;
                }
            }
        }
        return true;
    };
    for (const KeyUtils::KeyNotation notation : {KeyUtils::KeyNotation::Lancelot,
                 KeyUtils::KeyNotation::LancelotAndTraditional,
                 KeyUtils::KeyNotation::OpenKeyAndTraditional}) {
        m_pModel->setNotation(notation);
        const QImage image = render();
        // The label and, where the wheel names the keys, the name after it
        const auto widthOf = [&](const QString& camelot) {
            const auto& node = m_pModel->nodes().at(m_pModel->indexOf(key(camelot)));
            double width = metrics.horizontalAdvance(node.label);
            if (m_pWheel->showsNames() && !node.name.isEmpty()) {
                width += m_style.camelotNameGap + metrics.horizontalAdvance(node.name);
            }
            return width;
        };
        // Background in the label gap before each label: the figure, made
        // smaller for a long notation, does not reach it
        const double right = centreOf(QStringLiteral("3A")).x() - offset -
                widthOf(QStringLiteral("3A"));
        EXPECT_TRUE(blank(image, right - m_style.camelotLabelGap, right - 1))
                << static_cast<int>(notation);
        const double left = centreOf(QStringLiteral("9A")).x() + offset +
                widthOf(QStringLiteral("9A"));
        EXPECT_TRUE(blank(image, left + 1, left + m_style.camelotLabelGap))
                << static_cast<int>(notation);
    }
}

TEST_F(CrateCamelotWheelTest, CachesStaticScaleUntilResize) {
    render();
    const qint64 scale = m_pWheel->scaleCacheKey();
    const qint64 frame = m_pWheel->frameCacheKey();
    EXPECT_NE(scale, 0);
    EXPECT_NE(frame, 0);
    // Another expose only copies the painted wheel
    render();
    EXPECT_EQ(m_pWheel->frameCacheKey(), frame);
    // The model changes the nodes and edges, never the rings and spokes
    m_pModel->setMasterKey(key(QStringLiteral("3B")));
    render();
    EXPECT_EQ(m_pWheel->scaleCacheKey(), scale);
    EXPECT_NE(m_pWheel->frameCacheKey(), frame);
    // So does another key notation: the labels change, the scale does not
    const qint64 named = m_pWheel->frameCacheKey();
    m_pModel->setNotation(KeyUtils::KeyNotation::OpenKey);
    render();
    EXPECT_EQ(m_pWheel->scaleCacheKey(), scale);
    EXPECT_NE(m_pWheel->frameCacheKey(), named);
    m_pWheel->resize(m_pWheel->width() - 40, m_pWheel->height() - 40);
    render();
    EXPECT_NE(m_pWheel->scaleCacheKey(), scale);
}

TEST_F(CrateCamelotWheelTest, NamesTheKeysWhereTheyFit) {
    m_pModel->setNotation(KeyUtils::KeyNotation::Lancelot);
    const CamelotWheelModel::Node& node =
            m_pModel->nodes().at(m_pModel->indexOf(key(QStringLiteral("8A"))));
    EXPECT_EQ(QStringLiteral("8A"), node.label);
    EXPECT_EQ(QStringLiteral("Am"), node.name);
    // At the mockup's size the names follow the codes ("9A Em"), and a
    // smaller wheel drops them before the labels leave it
    EXPECT_TRUE(m_pWheel->showsNames());
    m_pWheel->resize(m_pWheel->minimumSizeHint());
    EXPECT_FALSE(m_pWheel->showsNames());
    // In the traditional notation the label is the name already
    m_pModel->setNotation(KeyUtils::KeyNotation::Traditional);
    EXPECT_TRUE(m_pModel->nodes().at(m_pModel->indexOf(key(QStringLiteral("8A")))).name.isEmpty());

    EXPECT_EQ(QStringLiteral("A minor"), CamelotWheelModel::nameOf(key(QStringLiteral("8A"))));
    EXPECT_EQ(QStringLiteral("C major"), CamelotWheelModel::nameOf(key(QStringLiteral("8B"))));
    EXPECT_EQ(QStringLiteral("F♯ major"), CamelotWheelModel::nameOf(key(QStringLiteral("2B"))));
    EXPECT_TRUE(CamelotWheelModel::nameOf(mixxx::track::io::key::INVALID).isEmpty());
    EXPECT_EQ(key(QStringLiteral("12B")), CamelotWheelModel::keyOfCamelot(QStringLiteral(" 12b")));
    EXPECT_EQ(mixxx::track::io::key::INVALID,
            CamelotWheelModel::keyOfCamelot(QStringLiteral("Am")));
    EXPECT_EQ(mixxx::track::io::key::INVALID, CamelotWheelModel::keyOfCamelot(QString()));
}

TEST_F(CrateCamelotWheelTest, CentreNamesTheMasterKey) {
    m_pModel->setMasterKey(key(QStringLiteral("8A")));
    const QImage bare = render();
    m_pWheel->setSubtitle(CamelotWheelModel::nameOf(key(QStringLiteral("8A"))));
    EXPECT_EQ(QStringLiteral("A minor"), m_pWheel->subtitle());
    const QImage named = render();
    // The subtitle is painted under the figure, where nothing else is
    const QPoint centre = m_pWheel->rect().center();
    const int baseline = centre.y() + qRound(m_style.camelotSubtitleBaseline);
    const QRect band(centre.x() - 40,
            baseline - qRound(m_style.fontSizeMeta * 0.8),
            80,
            qRound(m_style.fontSizeMeta * 0.8));
    const auto inked = [&](const QImage& image) {
        int pixels = 0;
        for (int y = band.top(); y <= band.bottom(); ++y) {
            for (int x = band.left(); x <= band.right(); ++x) {
                pixels += image.pixelColor(x, y) != m_style.background ? 1 : 0;
            }
        }
        return pixels;
    };
    EXPECT_EQ(0, inked(bare));
    EXPECT_GT(inked(named), 20);
}

TEST_F(CrateCamelotWheelTest, LegendWrapsToItsWidth) {
    CamelotLegend legend(m_style);
    const QSize wide = legend.sizeHint();
    // In one row at its size hint, the note under it
    QVector<QRectF> rects = legend.layout(wide.width());
    ASSERT_EQ(legend.items().size() + 2, rects.size());
    for (int index = 1; index <= legend.items().size(); ++index) {
        EXPECT_DOUBLE_EQ(rects.first().top(), rects.at(index).top()) << index;
    }
    EXPECT_GT(rects.last().top(), rects.first().bottom());
    // Narrower, the items wrap and the legend grows, every item inside it
    const int narrow = wide.width() / 2;
    rects = legend.layout(narrow);
    EXPECT_GT(legend.heightForWidth(narrow), legend.heightForWidth(wide.width()));
    for (const QRectF& rect : rects) {
        EXPECT_LE(rect.right(), narrow + 0.5);
    }
    EXPECT_LE(legend.minimumSizeHint().width(), narrow);
}

TEST_F(CrateCamelotWheelTest, LegendPaintsTheWheelsSamples) {
    CamelotLegend legend(m_style);
    legend.resize(legend.sizeHint());
    QImage image(legend.size(), QImage::Format_RGB32);
    image.fill(m_style.background);
    legend.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    const QVector<QRectF> rects = legend.layout(legend.width());
    // The master deck's sample is filled in accent, the compatible edge is
    // accentHi
    const QRectF master = rects.at(1);
    const QPointF masterCentre(master.left() + m_style.wheelLegendGlyph / 2, master.center().y());
    EXPECT_EQ(m_style.accent, image.pixelColor(masterCentre.toPoint()));
    const QRectF compatible = rects.at(2);
    EXPECT_GT(strongestShade(image,
                      QPointF(compatible.left() + m_style.wheelLegendLine / 2,
                              compatible.center().y()),
                      2,
                      m_style.accentHi,
                      m_style.background),
            0.9);
}

} // namespace
