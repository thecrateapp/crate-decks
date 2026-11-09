#include "crate/ui/dialogs/dialogstyle.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QBoxLayout>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QSvgRenderer>
#include <cmath>

#include "crate/appidentity.h"
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

/// What the application's style cannot do with QSS, done as widgets are
/// polished:
/// - menus are windows of their own: a translucent window lets the QSS
///   border radius show. It must be set before the menu's window exists,
///   and polishing comes before the first show;
/// - Qt lays a spin box's text out on the box's border and ignores its
///   padding: in dialogs, the text gets the field's padding as margins.
class PolishFixes : public QObject {
  public:
    using QObject::QObject;

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() != QEvent::Polish) {
            return QObject::eventFilter(pWatched, pEvent);
        }
        if (auto* pMenu = qobject_cast<QMenu*>(pWatched)) {
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

void applyApplicationStyle(const QString& styleSheet) {
    auto* pApp = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!pApp) {
        return;
    }
    static PolishFixes* s_pPolishFixes = nullptr;
    if (!s_pPolishFixes) {
        s_pPolishFixes = new PolishFixes(pApp);
        pApp->installEventFilter(s_pPolishFixes);
    }
    if (pApp->styleSheet() != styleSheet) {
        pApp->setStyleSheet(styleSheet);
    }
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

void setDisplayAxes(QLabel* pLabel) {
    if (!pLabel) {
        return;
    }
    pLabel->ensurePolished();
    QFont font = pLabel->font();
    const qreal pixelSize =
            font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
    font.setVariableAxis(QFont::Tag("wght"), static_cast<float>(font.weight()));
    font.setVariableAxis(QFont::Tag("opsz"), static_cast<float>(pixelSize));
    pLabel->setFont(font);
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
