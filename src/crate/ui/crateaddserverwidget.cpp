#include "crate/ui/crateaddserverwidget.h"

#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "crate/crateaddserverflow.h"

namespace crate {

namespace {

// Pages of m_pSteps, one per AddServerFlow::Step that needs its own page.
constexpr int kAddressPage = 0;
constexpr int kLocalNetworkPage = 1;
constexpr int kBusyPage = 2;
constexpr int kSignInPage = 3;

QPushButton* button(const QString& text, const char* objectName, QWidget* pParent) {
    auto* pButton = new QPushButton(text, pParent);
    pButton->setObjectName(QLatin1String(objectName));
    return pButton;
}

} // namespace

AddServerWidget::AddServerWidget(CrateServers* pServers, QWidget* parent)
        : QWidget(parent),
          m_pFlow(new AddServerFlow(pServers, this)),
          m_pSteps(new QStackedWidget(this)),
          m_pAddress(new QLineEdit(this)),
          m_pLocalNetworkWarning(new QLabel(this)),
          m_pEmail(new QLineEdit(this)),
          m_pPassword(new QLineEdit(this)),
          m_pError(new QLabel(this)) {
    // Address.
    auto* pAddressPage = new QWidget(m_pSteps);
    auto* pAddressLayout = new QHBoxLayout(pAddressPage);
    m_pAddress->setObjectName(QStringLiteral("crateServerAddress"));
    m_pAddress->setPlaceholderText(tr("crate.example.com"));
    QPushButton* pConnect = button(tr("Connect"), "crateConnect", pAddressPage);
    pAddressLayout->addWidget(new QLabel(tr("Server address:"), pAddressPage));
    pAddressLayout->addWidget(m_pAddress, 1);
    pAddressLayout->addWidget(pConnect);
    m_pSteps->insertWidget(kAddressPage, pAddressPage);

    // Plain HTTP on the local network.
    auto* pLocalNetworkPage = new QWidget(m_pSteps);
    auto* pLocalNetworkLayout = new QVBoxLayout(pLocalNetworkPage);
    m_pLocalNetworkWarning->setWordWrap(true);
    QPushButton* pConnectAnyway =
            button(tr("Connect without encryption"), "crateConnectAnyway", pLocalNetworkPage);
    pLocalNetworkLayout->addWidget(m_pLocalNetworkWarning);
    pLocalNetworkLayout->addWidget(pConnectAnyway, 0, Qt::AlignLeft);
    m_pSteps->insertWidget(kLocalNetworkPage, pLocalNetworkPage);

    // Waiting for the server or for the browser.
    auto* pBusyPage = new QLabel(m_pSteps);
    pBusyPage->setObjectName(QStringLiteral("crateBusy"));
    pBusyPage->setWordWrap(true);
    m_pSteps->insertWidget(kBusyPage, pBusyPage);

    // Sign-in.
    auto* pSignInPage = new QWidget(m_pSteps);
    auto* pSignInLayout = new QVBoxLayout(pSignInPage);
    auto* pForm = new QFormLayout();
    m_pEmail->setObjectName(QStringLiteral("crateEmail"));
    m_pPassword->setObjectName(QStringLiteral("cratePassword"));
    m_pPassword->setEchoMode(QLineEdit::Password);
    pForm->addRow(tr("Email:"), m_pEmail);
    pForm->addRow(tr("Password:"), m_pPassword);
    auto* pSignInButtons = new QHBoxLayout();
    QPushButton* pSignIn = button(tr("Sign in"), "crateSignIn", pSignInPage);
    QPushButton* pGoogle = button(tr("Sign in with Google"), "crateSignInGoogle", pSignInPage);
    QPushButton* pApple = button(tr("Sign in with Apple"), "crateSignInApple", pSignInPage);
    pSignInButtons->addWidget(pSignIn);
    pSignInButtons->addWidget(pGoogle);
    pSignInButtons->addWidget(pApple);
    pSignInButtons->addStretch();
    pSignInLayout->addLayout(pForm);
    pSignInLayout->addLayout(pSignInButtons);
    m_pSteps->insertWidget(kSignInPage, pSignInPage);

    m_pError->setObjectName(QStringLiteral("crateError"));
    m_pError->setWordWrap(true);
    QPushButton* pCancel = button(tr("Cancel"), "crateCancel", this);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(m_pSteps);
    pLayout->addWidget(m_pError);
    pLayout->addWidget(pCancel, 0, Qt::AlignRight);

    connect(pConnect, &QPushButton::clicked, this, [this] {
        m_pFlow->setAddress(m_pAddress->text());
    });
    connect(m_pAddress, &QLineEdit::returnPressed, pConnect, &QPushButton::click);
    connect(pConnectAnyway, &QPushButton::clicked, m_pFlow, &AddServerFlow::confirmLocalNetwork);
    connect(pSignIn, &QPushButton::clicked, this, [this] {
        // The password is not kept by this widget once it is sent.
        const QString password = m_pPassword->text();
        m_pPassword->clear();
        m_pFlow->loginWithPassword(m_pEmail->text().trimmed(), password);
    });
    connect(m_pPassword, &QLineEdit::returnPressed, pSignIn, &QPushButton::click);
    connect(pGoogle, &QPushButton::clicked, this, [this] {
        m_pFlow->loginWithBrowser(QStringLiteral("google"));
    });
    connect(pApple, &QPushButton::clicked, this, [this] {
        m_pFlow->loginWithBrowser(QStringLiteral("apple"));
    });
    connect(pCancel, &QPushButton::clicked, this, [this] {
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
    switch (m_pFlow->step()) {
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
}

} // namespace crate

#include "moc_crateaddserverwidget.cpp"
