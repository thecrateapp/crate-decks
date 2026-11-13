#include "crate/ui/dialogs/dialogstyle.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBoxLayout>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontInfo>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QSvgRenderer>
#include <cmath>

#include "crate/appidentity.h"
#include "crate/ui/dialogs/dialogscrim.h"
#include "preferences/configobject.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::dialogs");

const QString kVarsXml = QStringLiteral("skins/CrateDecks/generated/vars.xml");
const QString kGenerated = QStringLiteral("skins/CrateDecks/generated/");

QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        kLogger.warning() << "Cannot read" << path;
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

/// The Solar icon of a message box's kind instead of the platform's (plan
/// "Own brand", BR.PR3): question and information in accentHi, a warning in
/// the warning tone, an error in the danger tone. Its name is the box's
/// "crateIcon" property, for tests. Its buttons lose the platform's icons.
void setSolarIcon(QMessageBox* pBox) {
    const char* name = nullptr;
    switch (pBox->icon()) {
    case QMessageBox::Question:
        name = "question-circle_active";
        break;
    case QMessageBox::Information:
        name = "info-circle_active";
        break;
    case QMessageBox::Warning:
        name = "danger-triangle_warning";
        break;
    case QMessageBox::Critical:
        name = "close-circle_danger";
        break;
    case QMessageBox::NoIcon:
        return;
    }
    const int size = DialogMetrics::installed().px("messageIconSize");
    const QIcon icon(designPath() + QStringLiteral("icons/") + QLatin1String(name) +
            QStringLiteral(".svg"));
    pBox->setIconPixmap(icon.pixmap(QSize(size, size), pBox->devicePixelRatioF()));
    pBox->setProperty("crateIcon", QLatin1String(name));
    // Nor the platform's icons on its buttons (the red No, the green Yes)
    const QList<QAbstractButton*> buttons = pBox->buttons();
    for (QAbstractButton* pButton : buttons) {
        pButton->setIcon(QIcon());
    }
}

/// What the application's style cannot do with QSS, done as widgets are
/// polished:
/// - menus are windows of their own: a translucent window lets the QSS
///   border radius show. It must be set before the menu's window exists,
///   and polishing comes before the first show;
/// - Qt lays a spin box's text out on the box's border and ignores its
///   padding: in dialogs, the text gets the field's padding as margins.
/// - each dialog gets the dialog style as its own. The filter sees the
///   Polish event before the dialog handles it, so the dialog and then its
///   children are polished with it;
/// - message boxes get a Solar icon instead of the platform's.
class PolishFixes : public QObject {
  public:
    using QObject::QObject;

    QString dialogStyleSheet;

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() != QEvent::Polish) {
            return QObject::eventFilter(pWatched, pEvent);
        }
        if (auto* pDialog = qobject_cast<QDialog*>(pWatched)) {
            if (pDialog->styleSheet().isEmpty() && !dialogStyleSheet.isEmpty()) {
                pDialog->setStyleSheet(dialogStyleSheet);
            }
            if (auto* pBox = qobject_cast<QMessageBox*>(pDialog);
                    pBox && !dialogStyleSheet.isEmpty()) {
                setSolarIcon(pBox);
            }
        } else if (auto* pMenu = qobject_cast<QMenu*>(pWatched)) {
            if (!pMenu->testAttribute(Qt::WA_WState_Created)) {
                pMenu->setAttribute(Qt::WA_TranslucentBackground);
                pMenu->setWindowFlag(Qt::NoDropShadowWindowHint);
            }
        } else if (auto* pSpinBox = qobject_cast<QAbstractSpinBox*>(pWatched)) {
            if (qobject_cast<QDialog*>(pSpinBox->window())) {
                if (auto* pEdit = pSpinBox->findChild<QLineEdit*>()) {
                    const DialogMetrics& metrics = DialogMetrics::installed();
                    const int padding = metrics.px("fieldPaddingX") - metrics.px("strokeHairline");
                    pEdit->setTextMargins(padding, 0, padding, 0);
                }
            }
        }
        return QObject::eventFilter(pWatched, pEvent);
    }
};

} // namespace

const DialogMetrics& DialogMetrics::installed() {
    static const DialogMetrics kMetrics =
            fromVariables(readSkinVariables(resourcePath() + kVarsXml));
    return kMetrics;
}

DialogMetrics DialogMetrics::fromVariables(const SkinVariables& variables) {
    DialogMetrics metrics;
    metrics.m_variables = variables;
    return metrics;
}

double DialogMetrics::number(const char* name) const {
    QString variable = QStringLiteral("CrateDlg") + QLatin1String(name);
    variable[8] = variable[8].toUpper();
    bool ok = false;
    const double value = m_variables.value(variable).toDouble(&ok);
    if (!ok && isValid()) {
        kLogger.warning() << "No dialog metric" << variable << "in generated/vars.xml";
    }
    return ok ? value : 0;
}

int DialogMetrics::px(const char* name) const {
    return static_cast<int>(std::lround(number(name)));
}

QColor DialogMetrics::colour(const char* variable) const {
    return QColor(m_variables.value(QLatin1String(variable)));
}

QString resourcePath() {
    QString path = ConfigObject<ConfigValue>::computeResourcePath();
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }
    return path;
}

QString designPath() {
    return resourcePath() + kGenerated;
}

QString absoluteSkinUrls(const QString& styleSheet, const QString& resourcePath) {
    QString skins = QDir(resourcePath).filePath(QStringLiteral("skins"));
    if (!skins.endsWith(QLatin1Char('/'))) {
        skins += QLatin1Char('/');
    }
    QString result = styleSheet;
    result.replace(QStringLiteral("url(skins:"), QStringLiteral("url(") + skins);
    return result;
}

QString defaultStyleSheet(const QString& resourcePath) {
    const QString style = readFile(QDir(resourcePath).filePath(kGenerated + "default.qss"));
    return style.isEmpty() ? QString() : absoluteSkinUrls(style, resourcePath);
}

QString dialogStyleSheet(const QString& resourcePath) {
    const QString style = readFile(QDir(resourcePath).filePath(kGenerated + "dialogs.qss"));
    return style.isEmpty() ? QString() : absoluteSkinUrls(style, resourcePath);
}

void applyStyles(QWidget* pMainWindow, const QString& styleSheet, const QString& dialogStyleSheet) {
    auto* pApp = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!pApp) {
        return;
    }
    static PolishFixes* s_pPolishFixes = nullptr;
    if (!s_pPolishFixes) {
        s_pPolishFixes = new PolishFixes(pApp);
        pApp->installEventFilter(s_pPolishFixes);
    }
    // A dialog's own style replaces its parents': it needs the menus and
    // Upstream's rules too
    s_pPolishFixes->dialogStyleSheet =
            dialogStyleSheet.isEmpty() ? QString() : styleSheet + dialogStyleSheet;
    if (pMainWindow && pMainWindow->styleSheet() != styleSheet) {
        pMainWindow->setStyleSheet(styleSheet);
    }
    // Blocking dialogs dim the main window behind them (design 14.3)
    watchBlockingDialogs(dialogStyleSheet.isEmpty() ? nullptr : pMainWindow);
}

bool applyApplicationStyle(QWidget* pMainWindow, const QString& resourcePath) {
    const QString style = defaultStyleSheet(resourcePath);
    const QString dialogs = dialogStyleSheet(resourcePath);
    if (style.isEmpty() || dialogs.isEmpty()) {
        return false;
    }
    applyStyles(pMainWindow, style, dialogs);
    return true;
}

void styleLaunchImage(QWidget* pLaunchImage, const QString& resourcePath) {
    if (!pLaunchImage) {
        return;
    }
    const QString style = readFile(QDir(resourcePath).filePath(kGenerated + "launch.qss"));
    if (style.isEmpty()) {
        return;
    }
    // LaunchImage's own label shows the logo; the name goes under it, and
    // the progress bar under both (skin/legacy/launchimage.cpp)
    auto* pLogo = pLaunchImage->findChild<QLabel*>();
    if (pLogo && pLogo->objectName().isEmpty()) {
        pLogo->setObjectName(QStringLiteral("CrateSplashLogo"));
        auto* pName = new QLabel(QString::fromUtf8(kApplicationName), pLaunchImage);
        pName->setObjectName(QStringLiteral("CrateSplashName"));
        pName->setAlignment(Qt::AlignCenter);
        const DialogMetrics& metrics = DialogMetrics::installed();
        const QList<QBoxLayout*> layouts = pLaunchImage->findChildren<QBoxLayout*>();
        for (QBoxLayout* pLayout : layouts) {
            const int logoIndex = pLayout->indexOf(pLogo);
            if (logoIndex < 0) {
                continue;
            }
            pLayout->setSpacing(0);
            pLayout->insertSpacing(logoIndex + 1, metrics.px("firstRunHeadGap"));
            pLayout->insertWidget(logoIndex + 2, pName, 0, Qt::AlignHCenter);
            pLayout->insertSpacing(logoIndex + 3, metrics.px("splashGap"));
            pLayout->setAlignment(pLogo, Qt::AlignHCenter);
            break;
        }
    }
    // LaunchImage set its own style while it laid itself out, before it was
    // polished; on a widget that is not polished yet, QWidget::setStyleSheet
    // keeps the rules it already parsed. Polished, it parses the new ones.
    pLaunchImage->ensurePolished();
    pLaunchImage->setStyleSheet(absoluteSkinUrls(style, resourcePath));
    setDisplayAxes(pLaunchImage->findChild<QLabel*>(QStringLiteral("CrateSplashName")));
}

QPixmap logoPixmap(int width, int height, qreal devicePixelRatio) {
    QSvgRenderer renderer(designPath() + QStringLiteral("crate-logo.svg"));
    const QSize size(static_cast<int>(std::ceil(width * devicePixelRatio)),
            static_cast<int>(std::ceil(height * devicePixelRatio)));
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    if (renderer.isValid()) {
        QPainter painter(&pixmap);
        renderer.render(&painter);
    }
    pixmap.setDevicePixelRatio(devicePixelRatio);
    return pixmap;
}

QLabel* monoLabel(const QString& text, const QString& objectName, QWidget* pParent, bool accent) {
    auto* pLabel = new QLabel(text, pParent);
    pLabel->setObjectName(objectName);
    pLabel->setTextFormat(Qt::PlainText);
    if (accent) {
        pLabel->setProperty("crateTone", QStringLiteral("accent"));
    }
    return pLabel;
}

void setParagraph(QLabel* pLabel, const QString& text, const char* fontSizeToken) {
    if (!pLabel) {
        return;
    }
    const DialogMetrics& metrics = DialogMetrics::installed();
    const int lineHeight = static_cast<int>(
            std::lround(metrics.number("lineHeightBody") * metrics.number(fontSizeToken)));
    pLabel->setTextFormat(Qt::RichText);
    pLabel->setWordWrap(true);
    pLabel->setText(QStringLiteral("<p style=\"margin:0; line-height:%1px\">%2</p>")
                    .arg(QString::number(lineHeight), text.toHtmlEscaped()));
}

namespace {

/// Gives a label its display axes when it first shows.
class DisplayAxesOnShow : public QObject {
  public:
    explicit DisplayAxesOnShow(QLabel* pLabel)
            : QObject(pLabel),
              m_pLabel(pLabel) {
        pLabel->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() == QEvent::Show) {
            m_pLabel->removeEventFilter(this);
            setDisplayAxes(m_pLabel);
            deleteLater();
        }
        return QObject::eventFilter(pWatched, pEvent);
    }

  private:
    QLabel* m_pLabel;
};

} // namespace

void setDisplayAxes(QLabel* pLabel) {
    if (!pLabel) {
        return;
    }
    if (!pLabel->isVisible()) {
        new DisplayAxesOnShow(pLabel);
        return;
    }
    pLabel->ensurePolished();
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    // Variable axes came with Qt 6.7; older Qt keeps the font's default
    // instance.
    QFont font = pLabel->font();
    const qreal pixelSize =
            font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
    font.setVariableAxis(QFont::Tag("wght"), static_cast<float>(font.weight()));
    font.setVariableAxis(QFont::Tag("opsz"), static_cast<float>(pixelSize));
    pLabel->setFont(font);
#endif
}

void setButtonVariant(QPushButton* pButton, const char* variant, bool full) {
    if (!pButton) {
        return;
    }
    pButton->setProperty("crateButton", QString::fromLatin1(variant));
    if (full) {
        pButton->setProperty("crateSize", QStringLiteral("md"));
    }
    pButton->setAutoDefault(false);
    pButton->setCursor(Qt::PointingHandCursor);
    if (pButton->style()) {
        pButton->style()->unpolish(pButton);
        pButton->style()->polish(pButton);
    }
}

} // namespace crate
