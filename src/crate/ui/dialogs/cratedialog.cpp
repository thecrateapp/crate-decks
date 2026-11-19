#include "crate/ui/dialogs/cratedialog.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "crate/ui/dialogs/dialogchrome.h"
#include "crate/ui/dialogs/dialogstyle.h"
#include "moc_cratedialog.cpp"

namespace crate {

namespace {

const QString kChoiceName = QStringLiteral("crateChoice");

} // namespace

CrateDialog::CrateDialog(
        const QString& context, const QString& title, const QString& intro, QWidget* pParent)
        : QDialog(pParent),
          m_pBody(new QVBoxLayout()),
          m_pFoot(new QHBoxLayout()),
          m_pLater(nullptr) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    setObjectName(QStringLiteral("CrateDialog"));
    setWindowTitle(title);
    // The first start's padding and width (first-run)
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(metrics.px("firstRunPaddingX"),
            metrics.px("firstRunPaddingTop"),
            metrics.px("firstRunPaddingX"),
            metrics.px("firstRunPaddingBottom"));
    pLayout->setSpacing(metrics.px("firstRunGap"));
    pLayout->setSizeConstraint(QLayout::SetFixedSize);
    new DialogChrome(this);

    // Head: the logo and what the dialog is about
    auto* pHead = new QHBoxLayout();
    pHead->setSpacing(metrics.px("firstRunHeadGap"));
    auto* pLogo = new QLabel(this);
    pLogo->setObjectName(QStringLiteral("CrateDialogLogo"));
    pLogo->setPixmap(logoPixmap(metrics.px("firstRunLogoWidth"),
            metrics.px("firstRunLogoHeight"),
            devicePixelRatioF()));
    pHead->addWidget(pLogo);
    pHead->addWidget(monoLabel(context, QStringLiteral("MonoDialogKicker"), this, true));
    pHead->addStretch(1);
    pLayout->addLayout(pHead);

    auto* pTitle = new QLabel(title, this);
    pTitle->setObjectName(QStringLiteral("CrateDialogHeading"));
    pTitle->setTextFormat(Qt::PlainText);
    pLayout->addWidget(pTitle);
    setDisplayAxes(pTitle);
    if (!intro.isEmpty()) {
        auto* pIntro = new QLabel(this);
        pIntro->setObjectName(QStringLiteral("CrateDialogIntro"));
        setParagraph(pIntro, intro, "fontSizeBody");
        pIntro->setFixedWidth(contentWidth());
        pLayout->addWidget(pIntro);
    }

    m_pBody->setContentsMargins(0, 0, 0, 0);
    m_pBody->setSpacing(0);
    pLayout->addLayout(m_pBody);

    m_pFoot->setContentsMargins(0, metrics.px("firstRunActionsTop"), 0, 0);
    m_pFoot->setSpacing(metrics.px("helperGap"));
    m_pFoot->addStretch(1);
    m_pLater = monoLabel(QString(), QStringLiteral("MonoDialogLater"), this);
    m_pLater->hide();
    m_pFoot->addWidget(m_pLater);
    pLayout->addLayout(m_pFoot);
}

int CrateDialog::contentWidth() const {
    const DialogMetrics& metrics = DialogMetrics::installed();
    return metrics.px("firstRunWidth") - 2 * metrics.px("firstRunPaddingX");
}

QVBoxLayout* CrateDialog::addSection(const QString& label) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    // decks.css .psec: a rule over the section, its label and its rows
    auto* pSection = new QFrame(this);
    pSection->setObjectName(QStringLiteral("CrateDialogSection"));
    pSection->setFixedWidth(contentWidth());
    auto* pLayout = new QVBoxLayout(pSection);
    pLayout->setContentsMargins(0,
            metrics.px("prefsSectionPaddingTop"),
            0,
            metrics.px("prefsSectionPaddingBottom"));
    pLayout->setSpacing(metrics.px("prefsSectionLabelGap"));
    pLayout->addWidget(monoLabel(label, QStringLiteral("MonoDialogSection"), pSection, true));
    auto* pRows = new QVBoxLayout();
    pRows->setContentsMargins(0, 0, 0, 0);
    pRows->setSpacing(metrics.px("fieldGap"));
    pLayout->addLayout(pRows);
    m_pBody->addWidget(pSection);
    return pRows;
}

QPushButton* CrateDialog::addAction(const QString& text, const QString& objectName, Action action) {
    auto* pButton = new QPushButton(text, this);
    pButton->setObjectName(objectName);
    switch (action) {
    case Action::Primary:
        setButtonVariant(pButton, "primary");
        pButton->setDefault(true);
        connect(pButton, &QPushButton::clicked, this, &QDialog::accept);
        break;
    case Action::Danger:
        setButtonVariant(pButton, "danger");
        connect(pButton, &QPushButton::clicked, this, &QDialog::accept);
        break;
    case Action::Cancel:
        setButtonVariant(pButton, "ghost");
        connect(pButton, &QPushButton::clicked, this, &QDialog::reject);
        break;
    }
    // Left to right, before the stretch and the "later" label
    m_pFoot->insertWidget(m_actions++, pButton);
    return pButton;
}

void CrateDialog::setLater(const QString& text) {
    m_pLater->setText(text);
    m_pLater->setVisible(!text.isEmpty());
}

CrateDialog* createConfirmation(const Confirmation& confirmation, QWidget* pParent) {
    auto* pDialog = new CrateDialog(
            confirmation.context, confirmation.title, confirmation.text, pParent);
    pDialog->addAction(confirmation.action,
            QStringLiteral("crateConfirm"),
            confirmation.destructive ? CrateDialog::Action::Danger
                                     : CrateDialog::Action::Primary);
    pDialog->addAction(
            confirmation.cancel.isEmpty() ? CrateDialog::tr("Cancel") : confirmation.cancel,
            QStringLiteral("crateCancel"),
            CrateDialog::Action::Cancel);
    return pDialog;
}

CrateDialog* createChoice(const Choice& choice, QWidget* pParent) {
    auto* pDialog = new CrateDialog(choice.context, choice.title, choice.text, pParent);
    QVBoxLayout* pRows = pDialog->addSection(choice.label);
    auto* pBox = new QComboBox(pDialog);
    pBox->setObjectName(kChoiceName);
    pBox->addItems(choice.items);
    pBox->setAccessibleName(choice.label);
    pRows->addWidget(pBox);
    QPushButton* pConfirm = pDialog->addAction(
            choice.action, QStringLiteral("crateConfirm"), CrateDialog::Action::Primary);
    pConfirm->setEnabled(!choice.items.isEmpty());
    pDialog->addAction(
            CrateDialog::tr("Cancel"), QStringLiteral("crateCancel"), CrateDialog::Action::Cancel);
    return pDialog;
}

QComboBox* choiceBox(const QDialog* pDialog) {
    return pDialog ? pDialog->findChild<QComboBox*>(kChoiceName) : nullptr;
}

} // namespace crate
