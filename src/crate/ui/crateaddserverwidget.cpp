#include "crate/ui/crateaddserverwidget.h"

#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "crate/ui/dialogs/dialogstyle.h"

namespace crate {

namespace {

// Pages of m_pSteps, one per AddServerFlow::Step that needs its own page.
constexpr int kAddressPage = 0;
constexpr int kLocalNetworkPage = 1;
constexpr int kBusyPage = 2;
constexpr int kSignInPage = 3;

QPushButton* button(const QString& text,
        const char* objectName,
        const char* variant,
        QWidget* pParent) {
    auto* pButton = new QPushButton(text, pParent);
    pButton->setObjectName(QLatin1String(objectName));
    setButtonVariant(pButton, variant, true);
    return pButton;
}

/// A page of fields: each field under its MonoLabel (recipes.css
/// .crate-form-field).
QVBoxLayout* fieldsLayout(QWidget* pPage) {
    auto* pLayout = new QVBoxLayout(pPage);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(DialogMetrics::installed().px("fieldGap"));
    return pLayout;
}

/// Keeps an icon at the left of its field, centred, as the field resizes.
class FieldIconPlacer : public QObject {
  public:
    FieldIconPlacer(QWidget* pIcon, int left, int size, QObject* pParent)
            : QObject(pParent),
              m_pIcon(pIcon),
              m_left(left),
              m_size(size) {
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() == QEvent::Resize || pEvent->type() == QEvent::Show) {
            const auto* pField = static_cast<QWidget*>(pWatched);
            m_pIcon->setGeometry(m_left, (pField->height() - m_size) / 2, m_size, m_size);
        }
        return QObject::eventFilter(pWatched, pEvent);
    }

  private:
    QWidget* m_pIcon;
    const int m_left;
    const int m_size;
};

QLabel* plainLabel(const char* objectName, QWidget* pParent) {
    auto* pLabel = new QLabel(pParent);
    pLabel->setObjectName(QLatin1String(objectName));
    // Server messages are shown as they are, never as rich text
    pLabel->setTextFormat(Qt::PlainText);
    pLabel->setWordWrap(true);
    return pLabel;
}

} // namespace

int AddServerWidget::stepNumber(AddServerFlow::Step step) {
    switch (step) {
    case AddServerFlow::Step::EnterAddress:
        return 1;
    case AddServerFlow::Step::ConfirmLocalNetwork:
    case AddServerFlow::Step::CheckingServer:
        return 2;
    case AddServerFlow::Step::SignIn:
    case AddServerFlow::Step::SigningIn:
    case AddServerFlow::Step::Done:
        return 3;
    }
    return 1;
}

AddServerWidget::AddServerWidget(CrateServers* pServers, QWidget* parent)
        : QWidget(parent),
          m_pFlow(new AddServerFlow(pServers, this)),
          m_pSteps(new QStackedWidget(this)),
          m_pAddress(new QLineEdit(this)),
          m_pLocalNetworkWarning(plainLabel("crateLocalNetworkWarning", this)),
          m_pEmail(new QLineEdit(this)),
          m_pPassword(new QLineEdit(this)),
          m_pError(plainLabel("crateError", this)),
          m_pActions(new QHBoxLayout()),
          m_pConnect(button(tr("Connect"), "crateConnect", "primary", this)),
          m_pConnectAnyway(button(
                  tr("Connect without encryption"), "crateConnectAnyway", "primary", this)),
          m_pSignIn(button(tr("Sign in"), "crateSignIn", "primary", this)),
          m_pGoogle(button(tr("Sign in with Google"), "crateSignInGoogle", "ghost", this)),
          m_pApple(button(tr("Sign in with Apple"), "crateSignInApple", "ghost", this)),
          m_pCancel(button(tr("Cancel"), "crateCancel", "ghost", this)) {
    const DialogMetrics& metrics = DialogMetrics::installed();

    // Address: the field with the server icon and the HTTPS helper
    auto* pAddressPage = new QWidget(m_pSteps);
    QVBoxLayout* pAddressLayout = fieldsLayout(pAddressPage);
    pAddressLayout->addWidget(
            monoLabel(tr("Server address"), QStringLiteral("MonoFieldLabel"), pAddressPage));
    m_pAddress->setObjectName(QStringLiteral("crateServerAddress"));
    m_pAddress->setPlaceholderText(tr("crate.example.com"));
    // The server icon inside the field, as the mockup's .field: the text
    // starts after it
    auto* pIcon = new QLabel(m_pAddress);
    pIcon->setObjectName(QStringLiteral("CrateFieldIcon"));
    const int iconSize = metrics.px("iconMd");
    const int iconLeft = metrics.px("fieldPaddingX") - metrics.px("strokeHairline");
    m_pAddress->setTextMargins(iconSize + metrics.px("fieldIconGap"), 0, 0, 0);
    m_pAddress->installEventFilter(new FieldIconPlacer(pIcon, iconLeft, iconSize, m_pAddress));
    pAddressLayout->addWidget(m_pAddress);
    auto* pHelper = plainLabel("CrateFormHelper", pAddressPage);
    pHelper->setText(
            tr("HTTPS by default. HTTP only on this computer, or on your local network "
               "after confirming."));
    pAddressLayout->addWidget(pHelper);
    m_pSteps->insertWidget(kAddressPage, pAddressPage);

    // Plain HTTP on the local network
    auto* pLocalNetworkPage = new QWidget(m_pSteps);
    QVBoxLayout* pLocalNetworkLayout = fieldsLayout(pLocalNetworkPage);
    pLocalNetworkLayout->addWidget(m_pLocalNetworkWarning);
    pLocalNetworkLayout->addStretch(1);
    m_pSteps->insertWidget(kLocalNetworkPage, pLocalNetworkPage);

    // Waiting for the server or for the browser
    auto* pBusyPage = plainLabel("crateBusy", m_pSteps);
    m_pSteps->insertWidget(kBusyPage, pBusyPage);

    // Sign-in
    auto* pSignInPage = new QWidget(m_pSteps);
    QVBoxLayout* pSignInLayout = fieldsLayout(pSignInPage);
    m_pEmail->setObjectName(QStringLiteral("crateEmail"));
    m_pPassword->setObjectName(QStringLiteral("cratePassword"));
    m_pPassword->setEchoMode(QLineEdit::Password);
    pSignInLayout->addWidget(monoLabel(tr("Email"), QStringLiteral("MonoFieldLabel"), pSignInPage));
    pSignInLayout->addWidget(m_pEmail);
    pSignInLayout->addWidget(
            monoLabel(tr("Password"), QStringLiteral("MonoFieldLabel"), pSignInPage));
    pSignInLayout->addWidget(m_pPassword);
    m_pSteps->insertWidget(kSignInPage, pSignInPage);

    // One row of actions under the steps
    m_pActions->setContentsMargins(0, metrics.px("firstRunActionsTop"), 0, 0);
    m_pActions->setSpacing(metrics.px("dialogButtonGap"));
    for (QPushButton* pButton :
            {m_pConnect, m_pConnectAnyway, m_pSignIn, m_pGoogle, m_pApple, m_pCancel}) {
        m_pActions->addWidget(pButton);
    }
    m_pActions->addStretch(1);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(metrics.px("firstRunGap"));
    pLayout->addWidget(m_pSteps);
    pLayout->addWidget(m_pError);
    pLayout->addLayout(m_pActions);

    connect(m_pConnect, &QPushButton::clicked, this, [this] {
        m_pFlow->setAddress(m_pAddress->text());
    });
    connect(m_pAddress, &QLineEdit::returnPressed, m_pConnect, &QPushButton::click);
    connect(m_pConnectAnyway,
            &QPushButton::clicked,
            m_pFlow,
            &AddServerFlow::confirmLocalNetwork);
    connect(m_pSignIn, &QPushButton::clicked, this, [this] {
        // The password is not kept by this widget once it is sent.
        const QString password = m_pPassword->text();
        m_pPassword->clear();
        m_pFlow->loginWithPassword(m_pEmail->text().trimmed(), password);
    });
    connect(m_pPassword, &QLineEdit::returnPressed, m_pSignIn, &QPushButton::click);
    connect(m_pGoogle, &QPushButton::clicked, this, [this] {
        m_pFlow->loginWithBrowser(QStringLiteral("google"));
    });
    connect(m_pApple, &QPushButton::clicked, this, [this] {
        m_pFlow->loginWithBrowser(QStringLiteral("apple"));
    });
    connect(m_pCancel, &QPushButton::clicked, this, [this] {
        reset();
        emit cancelled();
    });
    connect(m_pFlow, &AddServerFlow::stepChanged, this, &AddServerWidget::showStep);
    connect(m_pFlow, &AddServerFlow::openBrowser, this, [](const QUrl& url) {
        QDesktopServices::openUrl(url);
    });
    connect(m_pFlow, &AddServerFlow::serverAdded, this, [this](const QString& serverId) {
        reset();
        emit serverAdded(serverId);
    });
    showStep();
}

void AddServerWidget::setCancelText(const QString& text) {
    m_pCancel->setText(text);
}

void AddServerWidget::addTrailing(QWidget* pWidget) {
    m_pActions->addWidget(pWidget);
}

void AddServerWidget::reset() {
    m_pFlow->cancel();
    m_pAddress->clear();
    m_pEmail->clear();
    m_pPassword->clear();
    showStep();
}

void AddServerWidget::showStep() {
    m_pError->setText(m_pFlow->error());
    m_pError->setVisible(!m_pFlow->error().isEmpty());
    auto* pBusy = qobject_cast<QLabel*>(m_pSteps->widget(kBusyPage));
    const AddServerFlow::Step step = m_pFlow->step();
    switch (step) {
    case AddServerFlow::Step::EnterAddress:
    case AddServerFlow::Step::Done:
        m_pSteps->setCurrentIndex(kAddressPage);
        break;
    case AddServerFlow::Step::ConfirmLocalNetwork:
        m_pLocalNetworkWarning->setText(
                tr("%1 is on your local network but does not use HTTPS: your password "
                   "and your session would cross the network without encryption. "
                   "Connect only if you trust this network.")
                        .arg(m_pFlow->origin().toString()));
        m_pSteps->setCurrentIndex(kLocalNetworkPage);
        break;
    case AddServerFlow::Step::CheckingServer:
        pBusy->setText(tr("Checking %1…").arg(m_pFlow->origin().toString()));
        m_pSteps->setCurrentIndex(kBusyPage);
        break;
    case AddServerFlow::Step::SignIn:
        m_pSteps->setCurrentIndex(kSignInPage);
        break;
    case AddServerFlow::Step::SigningIn:
        pBusy->setText(tr("Signing in… If a browser window opened, finish there."));
        m_pSteps->setCurrentIndex(kBusyPage);
        break;
    }
    // Each step shows its own actions; the stack is as tall as its page
    const bool address =
            step == AddServerFlow::Step::EnterAddress || step == AddServerFlow::Step::Done;
    const bool signIn = step == AddServerFlow::Step::SignIn;
    m_pConnect->setVisible(address);
    m_pConnectAnyway->setVisible(step == AddServerFlow::Step::ConfirmLocalNetwork);
    m_pSignIn->setVisible(signIn);
    m_pGoogle->setVisible(signIn);
    m_pApple->setVisible(signIn);
    m_pConnect->setDefault(address);
    m_pSignIn->setDefault(signIn);
    for (int page = 0; page < m_pSteps->count(); ++page) {
        m_pSteps->widget(page)->setSizePolicy(QSizePolicy::Preferred,
                page == m_pSteps->currentIndex() ? QSizePolicy::Preferred
                                                 : QSizePolicy::Ignored);
    }
    m_pSteps->adjustSize();
    emit stepNumberChanged(stepNumber(step));
}

} // namespace crate

#include "moc_crateaddserverwidget.cpp"
