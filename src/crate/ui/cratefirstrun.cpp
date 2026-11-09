#include "crate/ui/cratefirstrun.h"

#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QStyle>
#include <QVBoxLayout>

#include "crate/crateservers.h"
#include "crate/ui/crateaddserverwidget.h"
#include "crate/ui/dialogs/dialogchrome.h"
#include "crate/ui/dialogs/dialogstyle.h"

namespace crate {

namespace {

const ConfigKey kFirstRunOfferedKey(
        QStringLiteral("[CrateDecks]"), QStringLiteral("FirstRunOffered"));

QString translate(const char* text) {
    return QCoreApplication::translate("crate::FirstRun", text);
}

} // namespace

bool shouldOfferFirstRun(const UserSettingsPointer& pConfig, const CrateServers& servers) {
    if (!pConfig || !servers.sessions().isEmpty()) {
        return false;
    }
    return pConfig->getValueString(kFirstRunOfferedKey) != QLatin1String("1");
}

void markFirstRunOffered(const UserSettingsPointer& pConfig) {
    if (pConfig) {
        pConfig->set(kFirstRunOfferedKey, ConfigValue(QStringLiteral("1")));
    }
}

QDialog* createFirstRunDialog(CrateServers* pServers, QWidget* pParent) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pDialog = new QDialog(pParent);
    pDialog->setObjectName(QStringLiteral("CrateFirstRun"));
    pDialog->setWindowTitle(translate("Connect to Crate"));
    auto* pLayout = new QVBoxLayout(pDialog);
    pLayout->setContentsMargins(metrics.px("firstRunPaddingX"),
            metrics.px("firstRunPaddingTop"),
            metrics.px("firstRunPaddingX"),
            metrics.px("firstRunPaddingBottom"));
    pLayout->setSpacing(metrics.px("firstRunGap"));
    pLayout->setSizeConstraint(QLayout::SetFixedSize);
    new DialogChrome(pDialog);

    // The logo and "FIRST START  STEP 1 OF 3"
    auto* pHead = new QHBoxLayout();
    pHead->setSpacing(metrics.px("firstRunHeadGap"));
    auto* pLogo = new QLabel(pDialog);
    pLogo->setObjectName(QStringLiteral("CrateFirstRunLogo"));
    pLogo->setPixmap(logoPixmap(metrics.px("firstRunLogoWidth"),
            metrics.px("firstRunLogoHeight"),
            pDialog->devicePixelRatioF()));
    pHead->addWidget(pLogo);
    auto* pKicker = new QHBoxLayout();
    pKicker->setSpacing(metrics.px("helperGap"));
    pKicker->addWidget(monoLabel(translate("First start"),
            QStringLiteral("MonoFirstRunKicker"),
            pDialog,
            true));
    QLabel* pStep = monoLabel(QString(), QStringLiteral("MonoFirstRunStep"), pDialog);
    pKicker->addWidget(pStep);
    pHead->addLayout(pKicker);
    pHead->addStretch(1);
    pLayout->addLayout(pHead);

    auto* pTitle = new QLabel(translate("Connect your Crate"), pDialog);
    pTitle->setObjectName(QStringLiteral("CrateFirstRunTitle"));
    pLayout->addWidget(pTitle);
    setDisplayAxes(pTitle);
    auto* pIntro = new QLabel(pDialog);
    pIntro->setObjectName(QStringLiteral("CrateFirstRunIntro"));
    setParagraph(pIntro,
            translate("Crate Decks uses your Crate server as its library: your tracks, "
                      "Crate's analysis, recommendations and Smart Mix, right in the decks."),
            "fontSizeBody");
    pIntro->setFixedWidth(metrics.px("firstRunWidth") - 2 * metrics.px("firstRunPaddingX"));
    pLayout->addWidget(pIntro);

    // The three steps as lanes, the current and the done ones in accentHi
    auto* pSteps = new QHBoxLayout();
    pSteps->setSpacing(metrics.px("stepsGap"));
    const char* names[AddServerWidget::kStepCount] = {
            QT_TRANSLATE_NOOP("crate::FirstRun", "Address"),
            QT_TRANSLATE_NOOP("crate::FirstRun", "Check"),
            QT_TRANSLATE_NOOP("crate::FirstRun", "Sign in"),
    };
    QList<QLabel*> stepLabels;
    QList<QFrame*> stepLanes;
    for (int i = 0; i < AddServerWidget::kStepCount; ++i) {
        auto* pColumn = new QVBoxLayout();
        pColumn->setSpacing(metrics.px("stepGap"));
        QLabel* pLabel = monoLabel(QStringLiteral("%1 · %2").arg(i + 1).arg(translate(names[i])),
                QStringLiteral("MonoStep"),
                pDialog);
        auto* pLane = new QFrame(pDialog);
        pLane->setObjectName(QStringLiteral("CrateStepLane"));
        pColumn->addWidget(pLabel);
        pColumn->addWidget(pLane);
        pSteps->addLayout(pColumn, 1);
        stepLabels.append(pLabel);
        stepLanes.append(pLane);
    }
    pLayout->addLayout(pSteps);

    auto* pAddServer = new AddServerWidget(pServers, pDialog);
    pAddServer->setCancelText(translate("Not now"));
    pAddServer->addTrailing(monoLabel(translate("Later: Preferences · Crate"),
            QStringLiteral("MonoFirstRunLater"),
            pDialog));
    pLayout->addWidget(pAddServer);

    const auto showStep = [pStep, stepLabels, stepLanes](int number) {
        pStep->setText(translate("Step %1 of %2")
                        .arg(QString::number(number),
                                QString::number(AddServerWidget::kStepCount)));
        for (int i = 0; i < stepLabels.size(); ++i) {
            const bool on = i < number;
            stepLabels[i]->setProperty("crateTone", on ? QStringLiteral("accent") : QString());
            stepLanes[i]->setProperty("current", on);
            for (QWidget* pWidget : {static_cast<QWidget*>(stepLabels[i]),
                         static_cast<QWidget*>(stepLanes[i])}) {
                pWidget->style()->unpolish(pWidget);
                pWidget->style()->polish(pWidget);
            }
        }
    };
    QObject::connect(pAddServer, &AddServerWidget::stepNumberChanged, pDialog, showStep);
    showStep(1);

    QObject::connect(pAddServer, &AddServerWidget::serverAdded, pDialog, &QDialog::accept);
    QObject::connect(pAddServer, &AddServerWidget::cancelled, pDialog, &QDialog::reject);
    return pDialog;
}

void offerFirstRun(const UserSettingsPointer& pConfig, CrateServers* pServers, QWidget* pParent) {
    if (!pServers || QGuiApplication::platformName() == QLatin1String("offscreen") ||
            !shouldOfferFirstRun(pConfig, *pServers)) {
        return;
    }
    markFirstRunOffered(pConfig);

    QDialog* pDialog = createFirstRunDialog(pServers, pParent);
    pDialog->exec();
    delete pDialog;
}

} // namespace crate
