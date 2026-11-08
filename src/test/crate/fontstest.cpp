#include <gtest/gtest.h>

#include <QByteArray>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QList>
#include <QRawFont>
#include <QString>
#include <QtEndian>

#include "preferences/configobject.h"
#include "util/font.h"

namespace {

// The three fonts of the Crate admin, in res/fonts/ (design 14.1). The
// versions are those of the admin's woff2 files (res/crate-design/README.md).
const QString kBody = QStringLiteral("Roboto");
const QString kDisplay = QStringLiteral("Bricolage Grotesque");
const QString kMono = QStringLiteral("JetBrains Mono");

const QString kSample = QStringLiteral("DECK A · 8A · 124.00 BPM");

QFont fontOf(const QString& family, int pixelSize = 40) {
    QFont font(family);
    font.setPixelSize(pixelSize);
    return font;
}

/// The fontRevision field of the 'head' table (16.16 fixed point) of the
/// font file Qt picked for this font.
double fontRevision(const QFont& font) {
    const QByteArray head = QRawFont::fromFont(font).fontTable("head");
    if (head.size() < 8) {
        return 0;
    }
    return qFromBigEndian<qint32>(head.constData() + 4) / 65536.0;
}

QList<qreal> digitAdvances(const QFont& font) {
    const QFontMetricsF metrics(font);
    QList<qreal> advances;
    for (char digit = '0'; digit <= '9'; ++digit) {
        advances.append(metrics.horizontalAdvance(QString(QChar(digit))));
    }
    return advances;
}

bool allEqual(const QList<qreal>& values) {
    for (qreal value : values) {
        if (!qFuzzyCompare(value, values.first())) {
            return false;
        }
    }
    return true;
}

class CrateFontsTest : public testing::Test {
  protected:
    static void SetUpTestSuite() {
        // The same call as CoreServices at startup
        FontUtils::initializeFonts(ConfigObject<ConfigValue>::computeResourcePath());
    }
};

TEST_F(CrateFontsTest, RegistersAdminFonts) {
    const QStringList families = QFontDatabase::families();
    for (const QString& family : {kBody, kDisplay, kMono}) {
        EXPECT_TRUE(families.contains(family)) << family.toStdString();
        EXPECT_EQ(family.toStdString(), QFontInfo(fontOf(family)).family().toStdString());
    }
    // The bundled files, not other copies installed on the system
    EXPECT_NEAR(3.015, fontRevision(fontOf(kBody)), 0.001);
    EXPECT_NEAR(1.001, fontRevision(fontOf(kDisplay)), 0.001);
    EXPECT_NEAR(2.211, fontRevision(fontOf(kMono)), 0.001);
}

TEST_F(CrateFontsTest, MonoHasTabularDigits) {
    EXPECT_TRUE(allEqual(digitAdvances(fontOf(kMono, 11))));

    // Bricolage Grotesque has proportional digits by default and tabular
    // ones with the tnum feature, for the BPM and time of the deck
    // (design 14.3).
    QFont display = fontOf(kDisplay);
    display.setWeight(QFont::Bold);
    EXPECT_FALSE(allEqual(digitAdvances(display)));
    display.setFeature(QFont::Tag("tnum"), 1);
    EXPECT_TRUE(allEqual(digitAdvances(display)));
}

TEST_F(CrateFontsTest, VariableAxesChangeTheGlyphs) {
    // Roboto's weight axis (100-900)
    QFont light = fontOf(kBody, 15);
    light.setVariableAxis(QFont::Tag("wght"), 100);
    QFont black = fontOf(kBody, 15);
    black.setVariableAxis(QFont::Tag("wght"), 900);
    EXPECT_LT(QFontMetricsF(light).horizontalAdvance(kSample),
            QFontMetricsF(black).horizontalAdvance(kSample));

    // Bricolage Grotesque's optical size axis (12-96): at the same pixel
    // size, small optical sizes are spaced wider
    QFont caption = fontOf(kDisplay);
    caption.setVariableAxis(QFont::Tag("opsz"), 12);
    QFont poster = fontOf(kDisplay);
    poster.setVariableAxis(QFont::Tag("opsz"), 96);
    EXPECT_NE(QFontMetricsF(caption).horizontalAdvance(kSample),
            QFontMetricsF(poster).horizontalAdvance(kSample));
}

} // namespace
