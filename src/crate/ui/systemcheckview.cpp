#include "crate/ui/systemcheckview.h"

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <cmath>

#include "crate/cratepreferences.h"
#include "crate/system/powerhelper.h"
#include "crate/system/systemcheck.h"
#include "crate/ui/dialogs/dialogstyle.h"
#include "moc_systemcheckview.cpp"

namespace crate {

namespace {

using system::CheckAction;
using system::CheckResult;
using system::CheckStatus;

QString translate(const char* text) {
    return QCoreApplication::translate("crate::SystemCheckView", text);
}

QLabel* textLabel(const QString& text, const char* objectName, QWidget* pParent) {
    auto* pLabel = new QLabel(text, pParent);
    pLabel->setObjectName(QLatin1String(objectName));
    // Driver and renderer names are shown as they are
    pLabel->setTextFormat(Qt::PlainText);
    pLabel->setWordWrap(true);
    return pLabel;
}

QString stateName(CheckStatus status) {
    switch (status) {
    case CheckStatus::Ok:
        return QStringLiteral("ok");
    case CheckStatus::Warning:
        return QStringLiteral("warning");
    case CheckStatus::Blocking:
        return QStringLiteral("blocking");
    case CheckStatus::Unavailable:
        break;
    }
    return QStringLiteral("unavailable");
}

QString stateText(CheckStatus status) {
    switch (status) {
    case CheckStatus::Ok:
        return translate("OK");
    case CheckStatus::Warning:
        return translate("Advice");
    case CheckStatus::Blocking:
        return translate("Blocking");
    case CheckStatus::Unavailable:
        break;
    }
    return translate("Not available");
}

void repolish(QWidget* pWidget) {
    pWidget->style()->unpolish(pWidget);
    pWidget->style()->polish(pWidget);
}

} // namespace

SystemCheckView::SystemCheckView(system::SystemCheck* pCheck,
        CratePreferences* pPreferences,
        system::PowerHelper* pPowerHelper,
        QWidget* pParent)
        : QWidget(pParent),
          m_pCheck(pCheck),
          m_pPreferences(pPreferences),
          m_pPowerHelper(pPowerHelper),
          m_pProgress(textLabel(QString(), "CrateFormHelper", this)),
          m_pRows(new QWidget(this)),
          m_pRowsLayout(new QVBoxLayout(m_pRows)) {
    setObjectName(QStringLiteral("CrateSystemCheck"));
    m_pProgress->setObjectName(QStringLiteral("crateSystemCheckProgress"));
    m_pRowsLayout->setContentsMargins(0, 0, 0, 0);
    m_pRowsLayout->setSpacing(0);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);
    pLayout->addWidget(m_pProgress);
    pLayout->addWidget(m_pRows);
    m_pProgress->hide();
    if (m_pCheck) {
        connect(m_pCheck, &system::SystemCheck::sampled, this, &SystemCheckView::showProgress);
        connect(m_pCheck,
                &system::SystemCheck::runningChanged,
                this,
                &SystemCheckView::showProgress);
        connect(m_pCheck,
                &system::SystemCheck::resultsChanged,
                this,
                &SystemCheckView::showResults);
    }
    showResults();
}

void SystemCheckView::showProgress() {
    const bool running = m_pCheck && m_pCheck->isRunning();
    m_pProgress->setVisible(running);
    m_pRows->setVisible(!running);
    if (!running) {
        return;
    }
    const qint64 left = std::max<qint64>(0, m_pCheck->durationMs() - m_pCheck->elapsedMs());
    m_pProgress->setText(translate("Measuring this computer for %1 s more…")
                    .arg(std::ceil(static_cast<double>(left) / 1000.0)));
}

void SystemCheckView::showResults() {
    while (QLayoutItem* pItem = m_pRowsLayout->takeAt(0)) {
        // Later: a row's own button can run the check again
        if (QWidget* pRow = pItem->widget()) {
            pRow->setParent(nullptr);
            pRow->deleteLater();
        }
        delete pItem;
    }
    const int count = m_pCheck ? static_cast<int>(m_pCheck->results().size()) : 0;
    for (int i = 0; i < count; ++i) {
        m_pRowsLayout->addWidget(rowWidget(i));
    }
    showProgress();
}

QWidget* SystemCheckView::rowWidget(int index) {
    const CheckResult result = m_pCheck->results().at(index);
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pRow = new QFrame(m_pRows);
    pRow->setObjectName(QStringLiteral("CrateCheckRow"));
    pRow->setProperty("crateCheck", result.id);
    auto* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(0, metrics.px("serverPaddingY"), 0, metrics.px("serverPaddingY"));
    pLayout->setSpacing(metrics.px("serverGap"));

    auto* pDot = new QLabel(pRow);
    pDot->setObjectName(QStringLiteral("CrateCheckDot"));
    pDot->setProperty("crateState", stateName(result.status));
    pLayout->addWidget(pDot, 0, Qt::AlignTop);

    auto* pText = new QVBoxLayout();
    pText->setSpacing(metrics.px("serverNameGap"));
    pText->addWidget(textLabel(result.title, "CrateCheckTitle", pRow));
    pText->addWidget(textLabel(result.detail, "CrateCheckDetail", pRow));
    if (!result.advice.isEmpty()) {
        pText->addSpacing(metrics.px("serverNameGap"));
        pText->addWidget(textLabel(result.advice, "CrateCheckAdvice", pRow));
    }
    pLayout->addLayout(pText, 1);

    auto* pSide = new QVBoxLayout();
    pSide->setSpacing(metrics.px("stepGap"));
    auto* pState = textLabel(stateText(result.status), "CrateStateBadge", pRow);
    pState->setWordWrap(false);
    pState->setProperty("crateState", stateName(result.status));
    pSide->addWidget(pState, 0, Qt::AlignRight);
    if (result.action != CheckAction::None) {
        auto* pAction = new QPushButton(pRow);
        pAction->setObjectName(QStringLiteral("crateCheckAction"));
        setButtonVariant(pAction, "primary");
        pSide->addWidget(pAction, 0, Qt::AlignRight);
        if (result.action == CheckAction::DiscreteGpu) {
            const bool on = m_pPreferences && m_pPreferences->discreteGpu();
            pAction->setText(on ? translate("On at the next start")
                                : translate("Draw with the discrete GPU"));
            pAction->setEnabled(!on && m_pPreferences);
            connect(pAction, &QPushButton::clicked, this, [this, pAction] {
                if (m_pPreferences) {
                    m_pPreferences->setDiscreteGpu(true);
                }
                pAction->setText(translate("On at the next start"));
                pAction->setEnabled(false);
            });
        } else {
            pAction->setText(translate("Install the helper…"));
            pAction->setEnabled(m_pPowerHelper && m_pPowerHelper->isAvailable());
            connect(pAction, &QPushButton::clicked, this, [this, pRow, pAction] {
                pAction->hide();
                askToInstallHelper(pRow);
            });
        }
    }
    pSide->addStretch(1);
    pLayout->addLayout(pSide);
    return pRow;
}

void SystemCheckView::askToInstallHelper(QWidget* pRow) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pConfirm = new QWidget(pRow);
    pConfirm->setObjectName(QStringLiteral("crateHelperConfirm"));
    auto* pLayout = new QVBoxLayout(pConfirm);
    pLayout->setContentsMargins(0, metrics.px("stepGap"), 0, 0);
    pLayout->setSpacing(metrics.px("stepGap"));
    QLabel* pMessage = textLabel(
            translate("Crate Decks installs a system service, crate-power-limit, that "
                      "raises the power limit on AC and restores it on battery. Your "
                      "administrator password is asked next. The values are in "
                      "/etc/default/crate-power-limit."),
            "CrateCheckAdvice",
            pConfirm);
    pLayout->addWidget(pMessage);
    auto* pButtons = new QHBoxLayout();
    pButtons->setSpacing(metrics.px("helperGap"));
    auto* pInstall = new QPushButton(translate("Install"), pConfirm);
    pInstall->setObjectName(QStringLiteral("crateHelperInstall"));
    setButtonVariant(pInstall, "primary");
    auto* pCancel = new QPushButton(translate("Cancel"), pConfirm);
    pCancel->setObjectName(QStringLiteral("crateHelperCancel"));
    setButtonVariant(pCancel, "ghost");
    pButtons->addWidget(pInstall);
    pButtons->addWidget(pCancel);
    pButtons->addStretch(1);
    pLayout->addLayout(pButtons);
    // Under the row's text, as wide as it
    if (auto* pRowLayout = qobject_cast<QHBoxLayout*>(pRow->layout())) {
        if (auto* pText = qobject_cast<QVBoxLayout*>(pRowLayout->itemAt(1)->layout())) {
            pText->addWidget(pConfirm);
        }
    }

    connect(pCancel, &QPushButton::clicked, this, [this] {
        showResults();
    });
    connect(pInstall, &QPushButton::clicked, this, [this, pInstall, pCancel, pMessage] {
        if (!m_pPowerHelper) {
            return;
        }
        pInstall->setEnabled(false);
        pCancel->setEnabled(false);
        pMessage->setText(translate("Installing…"));
        connect(
                m_pPowerHelper,
                &system::PowerHelper::finished,
                pMessage,
                [this, pMessage, pCancel](bool ok, const QString& output) {
                    if (ok) {
                        // Measure again with the new limit
                        if (m_pCheck) {
                            m_pCheck->start();
                        }
                        return;
                    }
                    pMessage->setObjectName(QStringLiteral("crateError"));
                    repolish(pMessage);
                    pMessage->setText(output.isEmpty()
                                    ? translate("The helper was not installed.")
                                    : translate("The helper was not installed: %1").arg(output));
                    pCancel->setEnabled(true);
                },
                Qt::SingleShotConnection);
        m_pPowerHelper->install();
    });
}

} // namespace crate
