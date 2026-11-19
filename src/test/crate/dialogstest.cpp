#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QSpinBox>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>

#include "crate/ui/crateaddserverwidget.h"
#include "crate/ui/dialogs/cratedialog.h"
#include "crate/ui/dialogs/dialogscrim.h"
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
        // Other tests must not run under Crate's dialog style
        crate::applyStyles(nullptr, QString(), QString());
    }
};

TEST_F(CrateDialogsTest, DefaultStyleKeepsMixxxDefaultRules) {
    // Upstream's default.qss gives every skin a few images and colours (the
    // library preview button, the BPM lock, the cue menu, the colour
    // picker, the search's clear button): Crate's must keep each of those
    // rules, so that a sync that adds one fails here
    const QString inherited = readText(
            QDir(resourcePath()).filePath(QStringLiteral("skins/default.qss")));
    const QString crate = generated(QStringLiteral("default.qss"));
    ASSERT_FALSE(inherited.isEmpty());
    ASSERT_FALSE(crate.isEmpty());
    QSet<QString> crateSelectors;
    const auto crateRules = qssRules(crate);
    for (const auto& [selectors, body] : crateRules) {
        for (const QString& selector : selectors) {
            crateSelectors.insert(selector);
        }
    }
    int checked = 0;
    const auto inheritedRules = qssRules(inherited);
    for (const auto& [selectors, body] : inheritedRules) {
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
    const QString style = crate::defaultStyleSheet(resourcePath()) +
            crate::dialogStyleSheet(resourcePath());
    ASSERT_FALSE(crate::defaultStyleSheet(resourcePath()).isEmpty());
    ASSERT_FALSE(crate::dialogStyleSheet(resourcePath()).isEmpty());
    // Solar icons and generated controls only, no upstream images
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
    Warnings warnings;
    QWidget mainWindow;
    ASSERT_TRUE(crate::applyApplicationStyle(&mainWindow, resourcePath()));
    // The main window's style, as upstream's default.qss, and nothing on the
    // application, which would put every widget through QSS
    EXPECT_FALSE(mainWindow.styleSheet().isEmpty());
    EXPECT_TRUE(application()->styleSheet().isEmpty());
    // About and Preferences have no parent: a style on the main window
    // would not reach them
    QDialog dialog;
    auto* pButton = new QPushButton(QStringLiteral("OK"), &dialog);
    auto* pLabel = crate::monoLabel(QStringLiteral("Servers"), QStringLiteral("MonoTest"), &dialog);
    dialog.ensurePolished();
    EXPECT_FALSE(dialog.styleSheet().isEmpty());
    EXPECT_QSTRING_EQ(QStringLiteral("Roboto"), pButton->font().family());
    EXPECT_QSTRING_EQ(QStringLiteral("JetBrains Mono"), pLabel->font().family());
    EXPECT_EQ(QFont::AllUppercase, pLabel->font().capitalization());
    for (const QString& message : std::as_const(warnings.messages)) {
        EXPECT_FALSE(message.contains(QLatin1String("Could not parse"))) << message.toStdString();
    }
    // Outside dialogs, the buttons are the skin's
    auto* pSkinButton = new QPushButton(QStringLiteral("Play"), &mainWindow);
    pSkinButton->ensurePolished();
    EXPECT_NE(QStringLiteral("Roboto"), pSkinButton->font().family());
}

// Plan "Own brand", BR.PR3: upstream's message boxes ("No Output Devices", the
// quit confirmation...) show a Solar icon of their kind, not the platform's
TEST_F(CrateDialogsTest, MessageBoxesUseSolarIcons) {
    QWidget mainWindow;
    ASSERT_TRUE(crate::applyApplicationStyle(&mainWindow, resourcePath()));
    const struct {
        QMessageBox::Icon icon;
        const char* solar;
    } kinds[] = {
            {QMessageBox::Question, "question-circle_active"},
            {QMessageBox::Information, "info-circle_active"},
            {QMessageBox::Warning, "danger-triangle_warning"},
            {QMessageBox::Critical, "close-circle_danger"},
    };
    for (const auto& kind : kinds) {
        QMessageBox box(kind.icon, QStringLiteral("Title"), QStringLiteral("Text"));
        box.ensurePolished();
        EXPECT_QSTRING_EQ(QString::fromLatin1(kind.solar), box.property("crateIcon").toString());
        EXPECT_FALSE(box.iconPixmap().isNull()) << kind.solar;
        EXPECT_TRUE(QFile::exists(crate::designPath() + QStringLiteral("icons/") +
                QLatin1String(kind.solar) + QStringLiteral(".svg")))
                << kind.solar;
    }
    // Nor the platform's icons on the buttons
    {
        QMessageBox confirm(QMessageBox::Question,
                QStringLiteral("Confirm Exit"),
                QStringLiteral("Exit?"),
                QMessageBox::Yes | QMessageBox::No);
        confirm.ensurePolished();
        const QList<QAbstractButton*> buttons = confirm.buttons();
        for (QAbstractButton* pButton : buttons) {
            EXPECT_TRUE(pButton->icon().isNull()) << qPrintable(pButton->text());
        }
    }
    // Without an icon it stays without one
    QMessageBox plain(QMessageBox::NoIcon, QStringLiteral("Title"), QStringLiteral("Text"));
    plain.ensurePolished();
    EXPECT_FALSE(plain.property("crateIcon").isValid());
}

TEST_F(CrateDialogsTest, SplashShowsTheCrateBrand) {
    Warnings warnings;
    // Upstream's own splash, with the upstream logo
    LaunchImage splash(nullptr, QString());
    EXPECT_TRUE(splash.styleSheet().contains(QLatin1String("mixxx-icon-logo")));
    crate::styleLaunchImage(&splash, resourcePath());
    EXPECT_FALSE(splash.styleSheet().contains(QLatin1String("mixxx")));
    EXPECT_TRUE(splash.styleSheet().contains(QLatin1String("crate-logo.svg")));
    auto* pName = splash.findChild<QLabel*>(QStringLiteral("CrateSplashName"));
    ASSERT_NE(nullptr, pName);
    EXPECT_QSTRING_EQ(QStringLiteral("Decks"), pName->text());
    EXPECT_NE(nullptr, splash.findChild<QLabel*>(QStringLiteral("CrateSplashLogo")));
    // Painted on the app's background, not upstream's grey
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
    // How far the arc is from the accent on one side: the nearest pixel within
    // 2 px of the radius, as antialiasing blends the thin arc with what is
    // under it (an exact colour at one pixel failed with a thinner arc).
    const auto distanceToAccent = [&](int side) {
        int nearest = std::numeric_limits<int>::max();
        for (int offset = radius - 2; offset <= radius + 2; ++offset) {
            const QColor pixel = image.pixelColor(centre + QPoint(side * offset, 0));
            nearest = std::min(nearest,
                    std::abs(pixel.red() - accent.red()) +
                            std::abs(pixel.green() - accent.green()) +
                            std::abs(pixel.blue() - accent.blue()));
        }
        return nearest;
    };
    // From the top clockwise: the right half is lit, the left is not
    EXPECT_LT(distanceToAccent(1), distanceToAccent(-1) / 2);
    EXPECT_DOUBLE_EQ(1.0, (gauge.setValue(3), gauge.value()));
}

TEST_F(CrateDialogsTest, SpinBoxesInDialogsShowTheirValue) {
    ASSERT_TRUE(crate::applyApplicationStyle(nullptr, resourcePath()));
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

// TEST_GAP: that the scrim covers the OpenGL waveforms and stays under the
// dialog is the compositor's or the window manager's doing: offscreen there
// is neither. It was checked by recording the headless Mutter of
// tools/crate-design/skin-screenshots.sh through its ScreenCast API, with the
// app on Wayland (child window) and on X11 through Xwayland (top-level);
// Windows and macOS are not checked.

/// The scrim's window while one dims the main window, nullptr otherwise.
QWindow* scrimWindow() {
    const crate::DialogScrim* pScrim = crate::blockingDialogScrim();
    return pScrim && pScrim->window() && pScrim->window()->isVisible() ? pScrim->window()
                                                                       : nullptr;
}

/// A main window on screen with Crate's styles, as MixxxMainWindow sets them.
class ScrimMainWindow {
  public:
    ScrimMainWindow() {
        window.setGeometry(120, 80, 1280, 720);
        EXPECT_TRUE(crate::applyApplicationStyle(&window, resourcePath()));
        window.show();
        QCoreApplication::processEvents();
    }
    QWidget window;
};

TEST_F(CrateDialogsTest, BlockingDialogsDimTheMainWindow) {
    ScrimMainWindow main;
    EXPECT_EQ(nullptr, scrimWindow());

    // A confirmation as upstream opens one: open() makes it modal to the main
    // window. The scrim covers the whole main window, below the dialog
    QMessageBox confirm(QMessageBox::Question,
            QStringLiteral("Confirm Exit"),
            QStringLiteral("A deck is currently playing. Exit Mixxx?"),
            QMessageBox::Yes | QMessageBox::No,
            &main.window);
    confirm.open();
    QWindow* pScrim = scrimWindow();
    ASSERT_NE(nullptr, pScrim);
    EXPECT_EQ(main.window.frameGeometry(), pScrim->geometry());
    EXPECT_QSTRING_EQ(QStringLiteral("CrateDialogScrim"), pScrim->objectName());
    EXPECT_TRUE(pScrim->flags().testFlag(Qt::WindowTransparentForInput));
    EXPECT_TRUE(pScrim->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
    // The mockup's: the app's background, translucent (the scrim token)
    const QColor colour = crate::blockingDialogScrim()->colour();
    EXPECT_EQ(crate::DialogMetrics::installed().colour("CrateScrim"), colour);
    EXPECT_EQ(crate::DialogMetrics::installed().colour("CrateBgApp").rgb(),
            QColor(colour.rgb()).rgb());
    EXPECT_GT(colour.alpha(), 0);
    EXPECT_LT(colour.alpha(), 255);
    // Painted all over with it
    QCoreApplication::processEvents();
    ASSERT_NE(nullptr, pScrim->screen());
    const QImage painted = pScrim->screen()->grabWindow(pScrim->winId()).toImage();
    ASSERT_FALSE(painted.isNull());
    for (const QPoint at : {QPoint(1, 1),
                 QPoint(painted.width() / 2, painted.height() / 2),
                 QPoint(painted.width() - 2, painted.height() - 2)}) {
        const QColor pixel = painted.pixelColor(at);
        if (pixel.alpha() == 255) {
            // Qt 6.4's offscreen platform grabs windows opaque: the scrim
            // reads as its colour over black
            EXPECT_NEAR(colour.red() * colour.alphaF(), pixel.red(), 2);
            EXPECT_NEAR(colour.blue() * colour.alphaF(), pixel.blue(), 2);
        } else {
            EXPECT_NEAR(colour.alpha(), pixel.alpha(), 1);
            EXPECT_NEAR(colour.red(), pixel.red(), 2);
            EXPECT_NEAR(colour.blue(), pixel.blue(), 2);
        }
    }

    // It follows the main window
    main.window.move(200, 140);
    main.window.resize(1000, 600);
    QCoreApplication::processEvents();
    EXPECT_EQ(main.window.frameGeometry(), pScrim->geometry());

    confirm.done(QMessageBox::No);
    EXPECT_EQ(nullptr, scrimWindow());

    // Modal to the application, as exec() makes it; gone on accept, reject
    // and close
    QDialog dialog(&main.window);
    dialog.setModal(true);
    for (const auto& close : std::initializer_list<std::function<void()>>{
                 [&dialog] { dialog.accept(); },
                 [&dialog] { dialog.reject(); },
                 [&dialog] { dialog.close(); }}) {
        dialog.show();
        EXPECT_NE(nullptr, scrimWindow());
        close();
        EXPECT_EQ(nullptr, scrimWindow());
    }
    bool dimmed = false;
    QTimer::singleShot(0, &dialog, [&dialog, &dimmed] {
        dimmed = scrimWindow() != nullptr;
        dialog.reject();
    });
    dialog.exec();
    EXPECT_TRUE(dimmed);
    EXPECT_EQ(nullptr, scrimWindow());

    // Deleted while it shows
    auto* pDeleted = new QDialog(&main.window);
    pDeleted->open();
    EXPECT_NE(nullptr, scrimWindow());
    delete pDeleted;
    EXPECT_EQ(nullptr, scrimWindow());
}

TEST_F(CrateDialogsTest, DialogsThatDoNotBlockLeaveTheMainWindow) {
    ScrimMainWindow main;
    // Not modal
    QDialog modeless(&main.window);
    modeless.show();
    EXPECT_EQ(nullptr, scrimWindow());
    modeless.hide();
    // Modal to another window
    QWidget other;
    other.show();
    QDialog overOther(&other);
    overOther.open();
    EXPECT_EQ(nullptr, scrimWindow());
    overOther.reject();
    // A native dialog: nothing of it shows, and it is not Crate's
    QDialog native(&main.window);
    native.setAttribute(Qt::WA_DontShowOnScreen);
    native.open();
    EXPECT_EQ(nullptr, scrimWindow());
    native.reject();
}

TEST_F(CrateDialogsTest, ScrimShowsOnlyWithTheMainWindow) {
    ScrimMainWindow main;
    QDialog dialog(&main.window);
    dialog.setWindowModality(Qt::ApplicationModal);
    main.window.hide();
    dialog.show();
    EXPECT_EQ(nullptr, scrimWindow());
    // The main window comes back under the open dialog
    main.window.show();
    QCoreApplication::processEvents();
    EXPECT_NE(nullptr, scrimWindow());
    main.window.showMinimized();
    QCoreApplication::processEvents();
    EXPECT_EQ(nullptr, scrimWindow());
    main.window.showNormal();
    QCoreApplication::processEvents();
    EXPECT_NE(nullptr, scrimWindow());
    dialog.hide();
    EXPECT_EQ(nullptr, scrimWindow());

    // Without Crate's dialog style nothing is watched
    crate::applyStyles(nullptr, QString(), QString());
    EXPECT_EQ(nullptr, crate::blockingDialogScrim());
    dialog.show();
    EXPECT_EQ(nullptr, scrimWindow());
    dialog.hide();
}

TEST_F(CrateDialogsTest, ScrimIsAChildWindowOnWayland) {
    using Kind = crate::DialogScrim::Kind;
    EXPECT_EQ(Kind::ChildWindow, crate::DialogScrim::kindForPlatform(QStringLiteral("wayland")));
    EXPECT_EQ(Kind::ChildWindow,
            crate::DialogScrim::kindForPlatform(QStringLiteral("wayland-egl")));
    for (const char* platform : {"xcb", "windows", "cocoa", "offscreen"}) {
        EXPECT_EQ(Kind::TopLevel, crate::DialogScrim::kindForPlatform(QLatin1String(platform)))
                << platform;
    }

    QWidget mainWindow;
    mainWindow.setGeometry(120, 80, 1280, 720);
    mainWindow.show();
    QCoreApplication::processEvents();
    QWindow* pMainHandle = mainWindow.windowHandle();
    ASSERT_NE(nullptr, pMainHandle);
    crate::DialogScrim scrim(&mainWindow, Kind::ChildWindow);
    EXPECT_FALSE(scrim.isActive());
    scrim.setActive(true);
    ASSERT_NE(nullptr, scrim.window());
    EXPECT_EQ(pMainHandle, scrim.window()->parent());
    EXPECT_TRUE(scrim.window()->isVisible());
    EXPECT_EQ(QRect(QPoint(0, 0), mainWindow.size()), scrim.window()->geometry());
    mainWindow.resize(1000, 600);
    QCoreApplication::processEvents();
    EXPECT_EQ(QRect(QPoint(0, 0), mainWindow.size()), scrim.window()->geometry());

    // A native child created after it, as a waveform of a reloaded skin:
    // the next scrim is a new window, stacked above it
    scrim.setActive(false);
    EXPECT_EQ(nullptr, scrim.window());
    QWindow waveform(pMainHandle);
    waveform.show();
    scrim.setActive(true);
    ASSERT_NE(nullptr, scrim.window());
    QList<QWindow*> children;
    for (QObject* pChild : pMainHandle->children()) {
        if (auto* pWindow = qobject_cast<QWindow*>(pChild)) {
            children.append(pWindow);
        }
    }
    ASSERT_FALSE(children.isEmpty());
    EXPECT_EQ(scrim.window(), children.last());
    EXPECT_GT(children.indexOf(scrim.window()), children.indexOf(&waveform));

    // A native child that shows while the scrim is up, as a waveform when a
    // controller switches to four decks under a dialog: on Wayland it gets a
    // new subsurface above the scrim, so the scrim goes above it again
    const auto lastChild = [pMainHandle] {
        QWindow* pLast = nullptr;
        for (QObject* pChild : pMainHandle->children()) {
            if (auto* pWindow = qobject_cast<QWindow*>(pChild)) {
                pLast = pWindow;
            }
        }
        return pLast;
    };
    waveform.hide();
    QCoreApplication::processEvents();
    EXPECT_EQ(scrim.window(), lastChild());
    QWindow later(pMainHandle);
    later.show();
    waveform.show();
    QCoreApplication::processEvents();
    ASSERT_TRUE(scrim.isActive());
    EXPECT_EQ(scrim.window(), lastChild());
    scrim.setActive(false);
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

// V3.PR3: Decks' own dialogs have the first start's grammar: the head's
// MonoLabel, a display title, sections over a 1 px rule without boxes, mono
// never bold, and Crate buttons at the foot instead of a button box
TEST_F(CrateDialogsTest, CrateDialogHasTheFirstStartsGrammar) {
    ASSERT_TRUE(crate::applyApplicationStyle(nullptr, resourcePath()));
    crate::CrateDialog dialog(QStringLiteral("Music Path"),
            QStringLiteral("Music Path"),
            QStringLiteral("A path of tracks between two tracks."));
    QVBoxLayout* pRows = dialog.addSection(QStringLiteral("Tracks"));
    pRows->addWidget(new QSpinBox(&dialog));
    dialog.addSection(QStringLiteral("Destination"))
            ->addWidget(new QRadioButton(QStringLiteral("Auto DJ queue"), &dialog));
    dialog.addAction(QStringLiteral("Make the path"),
            QStringLiteral("crateConfirm"),
            crate::CrateDialog::Action::Primary);
    dialog.addAction(QStringLiteral("Cancel"),
            QStringLiteral("crateCancel"),
            crate::CrateDialog::Action::Cancel);
    dialog.show();
    QCoreApplication::processEvents();
    dialog.layout()->activate();

    const crate::DialogMetrics& metrics = crate::DialogMetrics::installed();
    EXPECT_EQ(metrics.px("firstRunWidth"), dialog.width());
    EXPECT_TRUE(dialog.findChildren<QGroupBox*>().isEmpty());
    EXPECT_TRUE(dialog.findChildren<QDialogButtonBox*>().isEmpty());
    auto* pKicker = dialog.findChild<QLabel*>(QStringLiteral("MonoDialogKicker"));
    ASSERT_NE(nullptr, pKicker);
    EXPECT_QSTRING_EQ(QStringLiteral("accent"), pKicker->property("crateTone").toString());
    auto* pHeading = dialog.findChild<QLabel*>(QStringLiteral("CrateDialogHeading"));
    ASSERT_NE(nullptr, pHeading);
    EXPECT_QSTRING_EQ(metrics.variables().value(QStringLiteral("CrateFontDisplay")),
            pHeading->font().family());

    const QList<QFrame*> sections =
            dialog.findChildren<QFrame*>(QStringLiteral("CrateDialogSection"));
    ASSERT_EQ(2, sections.size());
    for (const QFrame* pSection : sections) {
        EXPECT_EQ(QFrame::NoFrame, pSection->frameShape());
        auto* pHead = qobject_cast<QLabel*>(pSection->layout()->itemAt(0)->widget());
        ASSERT_NE(nullptr, pHead);
        EXPECT_QSTRING_EQ(QStringLiteral("MonoDialogSection"), pHead->objectName());
        EXPECT_QSTRING_EQ(QStringLiteral("accent"), pHead->property("crateTone").toString());
        EXPECT_EQ(dialog.contentWidth(), pSection->width());
    }
    // The rule is the section's top border, in the border token
    const QString qss = crate::dialogStyleSheet(resourcePath());
    const qsizetype rule = qss.indexOf(QStringLiteral("#CrateDialogSection {"));
    ASSERT_GE(rule, 0);
    EXPECT_TRUE(qss.mid(rule, qss.indexOf(u'}', rule) - rule)
                    .contains(QStringLiteral("border-top")));

    const QString mono = metrics.variables().value(QStringLiteral("CrateFontMono"));
    int monoWidgets = 0;
    const QList<QWidget*> widgets = dialog.findChildren<QWidget*>();
    for (const QWidget* pWidget : widgets) {
        if (pWidget->font().family() == mono) {
            ++monoWidgets;
            EXPECT_LE(pWidget->font().weight(), QFont::Normal)
                    << pWidget->objectName().toStdString();
        }
    }
    EXPECT_GE(monoWidgets, 3);

    auto* pConfirm = dialog.findChild<QPushButton*>(QStringLiteral("crateConfirm"));
    auto* pCancel = dialog.findChild<QPushButton*>(QStringLiteral("crateCancel"));
    ASSERT_TRUE(pConfirm && pCancel);
    EXPECT_QSTRING_EQ(QStringLiteral("primary"), pConfirm->property("crateButton").toString());
    EXPECT_TRUE(pConfirm->isDefault());
    EXPECT_QSTRING_EQ(QStringLiteral("ghost"), pCancel->property("crateButton").toString());
    // Left to right in the order they were added
    EXPECT_LT(pConfirm->x(), pCancel->x());
}

// A confirmation names its action; red and never the default button only
// when it destroys something. No platform icons on its buttons
TEST_F(CrateDialogsTest, ConfirmationsNameTheirActionAndDestroyOnlyInRed) {
    for (const bool destructive : {false, true}) {
        crate::Confirmation confirmation;
        confirmation.context = QStringLiteral("Cache");
        confirmation.title = QStringLiteral("Clear the cache?");
        confirmation.text = QStringLiteral("Delete the cached tracks from this computer?");
        confirmation.action = QStringLiteral("Clear cache");
        confirmation.destructive = destructive;
        std::unique_ptr<crate::CrateDialog> pDialog(crate::createConfirmation(confirmation));
        auto* pConfirm = pDialog->findChild<QPushButton*>(QStringLiteral("crateConfirm"));
        auto* pCancel = pDialog->findChild<QPushButton*>(QStringLiteral("crateCancel"));
        ASSERT_TRUE(pConfirm && pCancel);
        EXPECT_QSTRING_EQ(QStringLiteral("Clear cache"), pConfirm->text());
        EXPECT_QSTRING_EQ(QStringLiteral("Cancel"), pCancel->text());
        EXPECT_QSTRING_EQ(destructive ? QStringLiteral("danger") : QStringLiteral("primary"),
                pConfirm->property("crateButton").toString());
        EXPECT_EQ(!destructive, pConfirm->isDefault());
        // Beside a destructive action, Enter cancels (Diego, 2026-11-19)
        EXPECT_EQ(destructive, pCancel->isDefault());
        EXPECT_QSTRING_EQ(QStringLiteral("ghost"), pCancel->property("crateButton").toString());
        EXPECT_EQ(crate::CrateDialog::Size::Compact, pDialog->size());
        EXPECT_TRUE(pConfirm->icon().isNull());
        EXPECT_TRUE(pCancel->icon().isNull());
        EXPECT_FALSE(pConfirm->autoDefault());

        pConfirm->click();
        EXPECT_EQ(QDialog::Accepted, pDialog->result());
        pDialog->setResult(-1);
        pCancel->click();
        EXPECT_EQ(QDialog::Rejected, pDialog->result());
    }
    crate::Confirmation notNow;
    notNow.action = QStringLiteral("Download");
    notNow.cancel = QStringLiteral("Not now");
    std::unique_ptr<crate::CrateDialog> pDialog(crate::createConfirmation(notNow));
    EXPECT_QSTRING_EQ(QStringLiteral("Not now"),
            pDialog->findChild<QPushButton*>(QStringLiteral("crateCancel"))->text());
}

// One question is compact: narrower, with a title of about 22 px; dialogs
// with fields keep the first start's size (Diego, 2026-11-19)
TEST_F(CrateDialogsTest, ConfirmationsAreCompactAndFormsKeepTheFirstStartsSize) {
    ASSERT_TRUE(crate::applyApplicationStyle(nullptr, resourcePath()));
    const crate::DialogMetrics& metrics = crate::DialogMetrics::installed();
    ASSERT_GT(metrics.px("confirmWidth"), 0);
    ASSERT_LT(metrics.px("confirmWidth"), metrics.px("firstRunWidth"));
    crate::Confirmation confirmation;
    confirmation.context = QStringLiteral("Servers");
    confirmation.title = QStringLiteral("Remove dev.crate.local?");
    confirmation.text = QStringLiteral("Its cached tracks stay.");
    confirmation.action = QStringLiteral("Sign out and remove");
    confirmation.destructive = true;
    std::unique_ptr<crate::CrateDialog> pCompact(crate::createConfirmation(confirmation));
    crate::Choice choice;
    choice.title = QStringLiteral("Export a playlist");
    choice.label = QStringLiteral("Playlist");
    choice.action = QStringLiteral("Export");
    std::unique_ptr<crate::CrateDialog> pForm(crate::createChoice(choice));
    for (crate::CrateDialog* pDialog : {pCompact.get(), pForm.get()}) {
        pDialog->show();
        QCoreApplication::processEvents();
        pDialog->layout()->activate();
    }
    EXPECT_EQ(metrics.px("confirmWidth"), pCompact->width());
    EXPECT_EQ(metrics.px("firstRunWidth"), pForm->width());
    const auto titleSize = [](const QDialog* pDialog) {
        const QLabel* pTitle = pDialog->findChild<QLabel*>(QStringLiteral("CrateDialogHeading"));
        return pTitle ? pTitle->font().pixelSize() : 0;
    };
    EXPECT_EQ(metrics.px("fontSizeConfirmTitle"), titleSize(pCompact.get()));
    EXPECT_EQ(metrics.px("fontSizeFirstRunTitle"), titleSize(pForm.get()));
    EXPECT_EQ(crate::CrateDialog::Size::Form, pForm->size());
    const QLabel* pLogo = pCompact->findChild<QLabel*>(QStringLiteral("CrateDialogLogo"));
    ASSERT_NE(nullptr, pLogo);
    EXPECT_EQ(metrics.px("confirmLogoWidth"),
            qRound(pLogo->pixmap().width() / pLogo->pixmap().devicePixelRatio()));
}

TEST_F(CrateDialogsTest, ChoiceOffersItsItems) {
    crate::Choice choice;
    choice.context = QStringLiteral("Export to Crate");
    choice.title = QStringLiteral("Export a playlist");
    choice.label = QStringLiteral("Playlist");
    choice.items = {QStringLiteral("Warm-up"), QStringLiteral("Peak time")};
    choice.action = QStringLiteral("Export");
    std::unique_ptr<crate::CrateDialog> pDialog(crate::createChoice(choice));
    QComboBox* pBox = crate::choiceBox(pDialog.get());
    ASSERT_NE(nullptr, pBox);
    EXPECT_EQ(2, pBox->count());
    EXPECT_QSTRING_EQ(QStringLiteral("Warm-up"), pBox->currentText());
    auto* pSection = pDialog->findChild<QFrame*>(QStringLiteral("CrateDialogSection"));
    ASSERT_NE(nullptr, pSection);
    EXPECT_EQ(pSection, pBox->parentWidget());
    EXPECT_QSTRING_EQ(QStringLiteral("Playlist"),
            pSection->findChild<QLabel*>(QStringLiteral("MonoDialogSection"))->text());
    auto* pExport = pDialog->findChild<QPushButton*>(QStringLiteral("crateConfirm"));
    ASSERT_NE(nullptr, pExport);
    EXPECT_TRUE(pExport->isEnabled());

    choice.items.clear();
    std::unique_ptr<crate::CrateDialog> pEmpty(crate::createChoice(choice));
    EXPECT_FALSE(pEmpty->findChild<QPushButton*>(QStringLiteral("crateConfirm"))->isEnabled());
}

} // namespace
