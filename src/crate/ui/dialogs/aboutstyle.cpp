#include "crate/ui/dialogs/aboutstyle.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QVBoxLayout>

#include "crate/appidentity.h"
#include "crate/ui/dialogs/dialogchrome.h"
#include "crate/ui/dialogs/dialogstyle.h"

namespace crate {

namespace {

QString translate(const char* text) {
    return QCoreApplication::translate("crate::About", text);
}

/// The links of About's rich text in accentHi, the admin's link colour.
void recolourLinks(QLabel* pLabel, const QColor& colour) {
    if (!pLabel) {
        return;
    }
    static const QRegularExpression kLinkColour(QStringLiteral("color:#[0-9a-fA-F]{6};"));
    QString text = pLabel->text();
    text.replace(kLinkColour, QStringLiteral("color:%1;").arg(colour.name()));
    pLabel->setText(text);
}

} // namespace

void styleAbout(QDialog* pAbout) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pLayout = pAbout ? qobject_cast<QVBoxLayout*>(pAbout->layout()) : nullptr;
    auto* pHeader = pAbout ? pAbout->findChild<QHBoxLayout*>(QStringLiteral("horizontalLayout"))
                           : nullptr;
    auto* pVersion = pAbout ? pAbout->findChild<QLabel*>(QStringLiteral("version_label"))
                            : nullptr;
    auto* pInfo = pAbout ? pAbout->findChild<QWidget*>(QStringLiteral("widget")) : nullptr;
    if (!metrics.isValid() || !pLayout || !pHeader || !pVersion || !pInfo) {
        return;
    }
    pLayout->setContentsMargins(metrics.px("firstRunPaddingX"),
            metrics.px("firstRunPaddingTop"),
            metrics.px("firstRunPaddingX"),
            metrics.px("firstRunPaddingBottom"));
    pLayout->setSpacing(metrics.px("firstRunGap"));
    new DialogChrome(pAbout);
    // As tall as its content, at the width of the first start
    pLayout->setSizeConstraint(QLayout::SetFixedSize);
    const int contentWidth = metrics.px("aboutWidth") - 2 * metrics.px("firstRunPaddingX");

    // The logo, "ABOUT" and the version, as the first start's head
    if (auto* pIcon = pAbout->findChild<QLabel*>(QStringLiteral("crate_app_icon"))) {
        pIcon->setPixmap(logoPixmap(metrics.px("firstRunLogoWidth"),
                metrics.px("firstRunLogoHeight"),
                pAbout->devicePixelRatioF()));
    }
    pHeader->setSpacing(metrics.px("firstRunHeadGap"));
    if (pVersion->parentWidget() && pVersion->parentWidget()->layout()) {
        pVersion->parentWidget()->layout()->removeWidget(pVersion);
    }
    auto* pKicker = new QHBoxLayout();
    pKicker->setSpacing(metrics.px("helperGap"));
    pKicker->addWidget(
            monoLabel(translate("About"), QStringLiteral("MonoAboutKicker"), pAbout, true));
    pKicker->addWidget(pVersion);
    const int stretch = pHeader->count() - 1;
    pHeader->insertLayout(stretch >= 0 ? stretch : 0, pKicker);

    // The name, then the credits, then the version rows
    auto* pTitle = new QLabel(QString::fromUtf8(kApplicationName), pAbout);
    pTitle->setObjectName(QStringLiteral("CrateAboutTitle"));
    pLayout->insertWidget(1, pTitle);
    setDisplayAxes(pTitle);
    pLayout->removeWidget(pInfo);
    const int tabsIndex = pLayout->indexOf(pAbout->findChild<QTabWidget*>(QStringLiteral("tabs")));
    pLayout->insertWidget(tabsIndex >= 0 ? tabsIndex : pLayout->count(), pInfo);
    if (pInfo->layout()) {
        pInfo->layout()->setContentsMargins(0, 0, 0, 0);
        pInfo->layout()->setSpacing(metrics.px("aboutMetaGap"));
    }
    pInfo->setFixedWidth(contentWidth);
    for (QLabel* pValue : pInfo->findChildren<QLabel*>()) {
        pValue->setWordWrap(true);
    }
    if (auto* pTabs = pAbout->findChild<QTabWidget*>(QStringLiteral("tabs"))) {
        pTabs->setFixedSize(contentWidth, metrics.px("aboutTextHeight"));
        pTabs->setDocumentMode(true);
    }

    const QColor accent = metrics.colour("CrateAccentHi");
    for (const char* name : {"crate_based_on", "crate_attribution", "textWebsiteLink"}) {
        recolourLinks(pAbout->findChild<QLabel*>(QLatin1String(name)), accent);
    }
    for (const char* name : {"crate_based_on", "crate_attribution"}) {
        if (auto* pLabel = pAbout->findChild<QLabel*>(QLatin1String(name))) {
            pLabel->setFixedWidth(contentWidth);
        }
    }

    // The actions: Donate (Solar's heart) as a ghost button, OK as the action
    if (auto* pDonate = pAbout->findChild<QPushButton*>(QStringLiteral("btnDonate"))) {
        pDonate->setIcon(QIcon(designPath() + QStringLiteral("icons/heart_idle.svg")));
        setButtonVariant(pDonate, "ghost", true);
    }
    if (auto* pButtons = pAbout->findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"))) {
        if (QPushButton* pOk = pButtons->button(QDialogButtonBox::Ok)) {
            // Without the mnemonic's underline, which the mockups do not
            // draw; Enter and Escape close About
            pOk->setText(pOk->text().remove(QLatin1Char('&')));
            setButtonVariant(pOk, "primary", true);
        }
    }
}

} // namespace crate
