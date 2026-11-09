#include <gtest/gtest.h>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QString>

#include "crate/ui/crateaddserverwidget.h"
#include "crate/ui/dialogs/dialogstyle.h"
#include "crate/ui/dialogs/levelgauge.h"
#include "preferences/configobject.h"
#include "skin/legacy/launchimage.h"
#include "skin/skinloader.h"
#include "test/mixxxtest.h"
#include "util/font.h"

namespace {

QString resourcePath() {
    return ConfigObject<ConfigValue>::computeResourcePath();
}

QString readText(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString generated(const QString& name) {
    return readText(QDir(resourcePath())
                    .filePath(QStringLiteral("skins/CrateDecks/generated/") +
                            name));
}

/// The rules of a QSS file without its comments: selectors and bodies.
QList<std::pair<QStringList, QString>> qssRules(const QString& qss) {
    static const QRegularExpression kComment(
            QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption);
    QString text = qss;
    text.remove(kComment);
    QList<std::pair<QStringList, QString>> rules;
    const QStringList blocks = text.split(QLatin1Char('}'));
    for (const QString& block : blocks) {
        const int open = block.indexOf(QLatin1Char('{'));
        if (open < 0) {
            continue;
        }
        QStringList selectors;
        const QStringList parts = block.left(open).split(QLatin1Char(','));
        for (const QString& part : parts) {
            const QString selector = part.simplified();
            if (!selector.isEmpty()) {
                selectors.append(selector);
            }
        }
        rules.append({selectors, block.mid(open + 1)});
    }
    return rules;
}

/// Keeps the warnings Qt logs while it lives.
class Warnings {
  public:
    Warnings() {
        s_pWarnings = this;
        m_previous = qInstallMessageHandler(&Warnings::handle);
    }
    ~Warnings() {
        qInstallMessageHandler(m_previous);
        s_pWarnings = nullptr;
    }
    QStringList messages;

  private:
    static void handle(QtMsgType type, const QMessageLogContext&, const QString& message) {
        if (s_pWarnings && type >= QtWarningMsg) {
            s_pWarnings->messages.append(message);
        }
    }
    static inline Warnings* s_pWarnings = nullptr;
    QtMessageHandler m_previous;
};

class CrateDialogsTest : public MixxxTest {
  protected:
    static void SetUpTestSuite() {
        FontUtils::initializeFonts(ConfigObject<ConfigValue>::computeResourcePath());
    }
    void TearDown() override {
        // Other tests must not run under the application's style
        application()->setStyleSheet(QString());
    }
};

TEST_F(CrateDialogsTest, DefaultStyleKeepsMixxxDefaultRules) {
    // Mixxx's default.qss gives every skin a few images and colours (the
    // library preview button, the BPM lock, the cue menu, the colour
    // picker, the search's clear button): Crate's must keep each of those
    // rules, so that a sync that adds one fails here
    const QString mixxx = readText(
            QDir(resourcePath()).filePath(QStringLiteral("skins/default.qss")));
    const QString crate = generated(QStringLiteral("default.qss"));
    ASSERT_FALSE(mixxx.isEmpty());
    ASSERT_FALSE(crate.isEmpty());
    QSet<QString> crateSelectors;
    for (const auto& [selectors, body] : qssRules(crate)) {
        for (const QString& selector : selectors) {
            crateSelectors.insert(selector);
        }
    }
    int checked = 0;
    for (const auto& [selectors, body] : qssRules(mixxx)) {
        if (!body.contains(QLatin1String("image")) && !body.contains(QLatin1String("icon")) &&
                !body.contains(QLatin1String("background"))) {
            continue;
        }
        for (const QString& selector : selectors) {
            ++checked;
            EXPECT_TRUE(crateSelectors.contains(selector)) << selector.toStdString();
        }
    }
    EXPECT_GT(checked, 10);
}

TEST_F(CrateDialogsTest, DefaultStyleUsesOnlyGeneratedImages) {
    const QString style = crate::defaultStyleSheet(resourcePath());
    ASSERT_FALSE(style.isEmpty());
    // Solar icons and generated controls only, no Mixxx images
    EXPECT_FALSE(style.contains(QLatin1String(":/images/")));
    EXPECT_FALSE(style.contains(QLatin1String("url(skins:")));
    static const QRegularExpression kUrl(QStringLiteral("url\\(([^)]+)\\)"));
    int urls = 0;
    for (auto match = kUrl.globalMatch(style); match.hasNext();) {
        const QString path = match.next().captured(1);
        ++urls;
        EXPECT_TRUE(QFileInfo::exists(path)) << path.toStdString();
    }
    EXPECT_GT(urls, 20);
}

TEST_F(CrateDialogsTest, DefaultStyleReachesParentlessDialogs) {
    const QString style = crate::defaultStyleSheet(resourcePath());
    ASSERT_FALSE(style.isEmpty());
    Warnings warnings;
    crate::applyApplicationStyle(style);
    // About and Preferences have no parent: a style on the main window
    // would not reach them
    QDialog dialog;
    auto* pButton = new QPushButton(QStringLiteral("OK"), &dialog);
    auto* pLabel = crate::monoLabel(QStringLiteral("Servers"), QStringLiteral("MonoTest"), &dialog);
    dialog.ensurePolished();
    pButton->ensurePolished();
    pLabel->ensurePolished();
    EXPECT_QSTRING_EQ(QStringLiteral("Roboto"), pButton->font().family());
    EXPECT_QSTRING_EQ(QStringLiteral("JetBrains Mono"), pLabel->font().family());
    EXPECT_EQ(QFont::AllUppercase, pLabel->font().capitalization());
    for (const QString& message : std::as_const(warnings.messages)) {
        EXPECT_FALSE(message.contains(QLatin1String("Could not parse"))) << message.toStdString();
    }
    // Outside dialogs, the buttons are the skin's
    QWidget skin;
    auto* pSkinButton = new QPushButton(QStringLiteral("Play"), &skin);
    pSkinButton->ensurePolished();
    EXPECT_NE(QStringLiteral("Roboto"), pSkinButton->font().family());
}

TEST_F(CrateDialogsTest, SplashShowsTheCrateBrand) {
    Warnings warnings;
    // Mixxx's own splash, with the Mixxx logo
    LaunchImage splash(nullptr, QString());
    EXPECT_TRUE(splash.styleSheet().contains(QLatin1String("mixxx-icon-logo")));
    crate::styleLaunchImage(&splash, resourcePath());
    EXPECT_FALSE(splash.styleSheet().contains(QLatin1String("mixxx")));
    EXPECT_TRUE(splash.styleSheet().contains(QLatin1String("crate-logo.svg")));
    auto* pName = splash.findChild<QLabel*>(QStringLiteral("CrateSplashName"));
    ASSERT_NE(nullptr, pName);
    EXPECT_QSTRING_EQ(QStringLiteral("Crate Decks"), pName->text());
    EXPECT_NE(nullptr, splash.findChild<QLabel*>(QStringLiteral("CrateSplashLogo")));
    // Painted on the app's background, not Mixxx's grey
    splash.resize(400, 300);
    const QImage image = splash.grab().toImage();
    EXPECT_EQ(crate::DialogMetrics::installed().colour("CrateBgApp").rgb(),
            image.pixelColor(2, 2).rgb())
            << warnings.messages.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(warnings.messages.isEmpty());

    // The same with any skin: the splash belongs to the application
    for (const QString& skin : {QStringLiteral("LateNight"), QStringLiteral("CrateDecks")}) {
        config()->setValue(ConfigKey("[Config]", "ResizableSkin"), skin);
        mixxx::skin::SkinLoader loader(config());
        std::unique_ptr<LaunchImage> pSplash(loader.loadLaunchImage(nullptr));
        ASSERT_TRUE(pSplash) << skin.toStdString();
        EXPECT_TRUE(pSplash->styleSheet().contains(QLatin1String("crate-logo.svg")))
                << skin.toStdString();
        EXPECT_NE(nullptr, pSplash->findChild<QLabel*>(QStringLiteral("CrateSplashName")));
    }
}

TEST_F(CrateDialogsTest, CacheRingShowsTheLevel) {
    crate::LevelGauge gauge;
    gauge.setValue(0.5);
    QImage image(gauge.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    gauge.render(&image);
    const QColor accent = crate::DialogMetrics::installed().colour("CrateAccentHi");
    const int radius = crate::DialogMetrics::installed().px("cacheGaugeRadius");
    const QPoint centre(gauge.width() / 2, gauge.height() / 2);
    const auto isAccent = [&](QPoint at) {
        const QColor pixel = image.pixelColor(at);
        return pixel.alpha() > 128 && std::abs(pixel.red() - accent.red()) < 40 &&
                std::abs(pixel.blue() - accent.blue()) < 40;
    };
    // From the top clockwise: the right half is lit, the left is not
    EXPECT_TRUE(isAccent(centre + QPoint(radius, 0)));
    EXPECT_FALSE(isAccent(centre - QPoint(radius, 0)));
    EXPECT_DOUBLE_EQ(1.0, (gauge.setValue(3), gauge.value()));
}

TEST_F(CrateDialogsTest, SpinBoxesInDialogsShowTheirValue) {
    crate::applyApplicationStyle(crate::defaultStyleSheet(resourcePath()));
    QDialog dialog;
    auto* pSpinBox = new QSpinBox(&dialog);
    pSpinBox->setValue(10);
    pSpinBox->resize(200, 40);
    dialog.resize(300, 100);
    dialog.show();
    QCoreApplication::processEvents();
    auto* pEdit = pSpinBox->findChild<QLineEdit*>();
    ASSERT_NE(nullptr, pEdit);
    // Without arrows the text takes the whole field, after its padding
    EXPECT_GT(pEdit->width(), pSpinBox->width() / 2);
    const crate::DialogMetrics& metrics = crate::DialogMetrics::installed();
    EXPECT_EQ(metrics.px("fieldPaddingX") - metrics.px("strokeHairline"),
            pEdit->textMargins().left());
    EXPECT_QSTRING_EQ(QStringLiteral("10"), pEdit->text());
}

TEST(CrateAddServerWidgetTest, StepsAreNumberedAsTheMockup) {
    using Step = crate::AddServerFlow::Step;
    EXPECT_EQ(1, crate::AddServerWidget::stepNumber(Step::EnterAddress));
    EXPECT_EQ(2, crate::AddServerWidget::stepNumber(Step::ConfirmLocalNetwork));
    EXPECT_EQ(2, crate::AddServerWidget::stepNumber(Step::CheckingServer));
    EXPECT_EQ(3, crate::AddServerWidget::stepNumber(Step::SignIn));
    EXPECT_EQ(3, crate::AddServerWidget::stepNumber(Step::SigningIn));
    EXPECT_EQ(3, crate::AddServerWidget::kStepCount);
}

} // namespace
