#include "crate/ui/dlgprefcrate.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include "crate/client/cratecapabilities.h"
#include "crate/crateservers.h"
#include "crate/session/cratesession.h"
#include "crate/ui/crateaddserverwidget.h"

namespace crate {

namespace {

QString stateText(const Session& session) {
    switch (session.state()) {
    case Session::State::SignedIn:
        return session.profile().accountEmail.isEmpty()
                ? DlgPrefCrate::tr("Signed in")
                : DlgPrefCrate::tr("Signed in as %1").arg(session.profile().accountEmail);
    case Session::State::SigningIn:
        return DlgPrefCrate::tr("Connecting…");
    case Session::State::SignedOut:
        return DlgPrefCrate::tr("Signed out: sign out and add the server again");
    }
    return QString();
}

QString featuresText(const QByteArray& capabilitiesJson) {
    const Result<Capabilities> capabilities =
            Capabilities::fromJson(QJsonDocument::fromJson(capabilitiesJson));
    if (!capabilities.ok()) {
        return QString();
    }
    const std::pair<DecksFeature, QString> names[] = {
            {DecksFeature::LibraryCursor, DlgPrefCrate::tr("library")},
            {DecksFeature::ProfilesBatch, DlgPrefCrate::tr("analysis")},
            {DecksFeature::Beatmatch, DlgPrefCrate::tr("beatmatch")},
            {DecksFeature::BassHandoff, DlgPrefCrate::tr("bass handoff")},
            {DecksFeature::LongBeatmatch, DlgPrefCrate::tr("long beatmatch")},
            {DecksFeature::Cues, DlgPrefCrate::tr("cues")},
    };
    QStringList available;
    for (const auto& [feature, name] : names) {
        if (capabilities.value().has(feature)) {
            available.append(name);
        }
    }
    return available.isEmpty()
            ? DlgPrefCrate::tr("No Crate Decks features announced yet.")
            : DlgPrefCrate::tr("Features: %1").arg(available.join(QStringLiteral(", ")));
}

} // namespace

DlgPrefCrate::DlgPrefCrate(QWidget* pParent, CrateServers* pServers)
        : DlgPreferencePage(pParent),
          m_pServers(pServers),
          m_pServerList(new QListWidget(this)),
          m_pStatus(new QLabel(this)),
          m_pKeychainWarning(new QLabel(this)),
          m_pAdd(new QPushButton(tr("Add a server…"), this)),
          m_pRemove(new QPushButton(tr("Sign out and remove"), this)),
          m_pAddServer(new AddServerWidget(pServers, this)) {
    m_pServerList->setObjectName(QStringLiteral("crateServers"));
    m_pAdd->setObjectName(QStringLiteral("crateAddServer"));
    m_pRemove->setObjectName(QStringLiteral("crateRemoveServer"));
    m_pStatus->setObjectName(QStringLiteral("crateServerStatus"));
    m_pStatus->setTextFormat(Qt::PlainText);
    m_pKeychainWarning->setObjectName(QStringLiteral("crateKeychainWarning"));
    m_pStatus->setWordWrap(true);
    m_pKeychainWarning->setWordWrap(true);
    m_pKeychainWarning->setText(
            tr("The system keychain is not available: you will have to sign in again "
               "after Crate Decks restarts."));
    m_pKeychainWarning->hide();

    auto* pServersBox = new QGroupBox(tr("Crate servers"), this);
    auto* pServersLayout = new QVBoxLayout(pServersBox);
    auto* pButtons = new QHBoxLayout();
    pButtons->addWidget(m_pAdd);
    pButtons->addWidget(m_pRemove);
    pButtons->addStretch();
    pServersLayout->addWidget(m_pServerList);
    pServersLayout->addWidget(m_pStatus);
    pServersLayout->addLayout(pButtons);

    auto* pAddBox = new QGroupBox(tr("Add a Crate server"), this);
    pAddBox->setObjectName(QStringLiteral("crateAddServerBox"));
    auto* pAddLayout = new QVBoxLayout(pAddBox);
    pAddLayout->addWidget(m_pAddServer);
    pAddBox->hide();

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(pServersBox);
    pLayout->addWidget(pAddBox);
    pLayout->addWidget(m_pKeychainWarning);
    pLayout->addStretch();

    connect(m_pAdd, &QPushButton::clicked, pAddBox, &QWidget::show);
    connect(m_pAddServer, &AddServerWidget::cancelled, pAddBox, &QWidget::hide);
    connect(m_pAddServer, &AddServerWidget::serverAdded, pAddBox, &QWidget::hide);
    connect(m_pServerList,
            &QListWidget::currentRowChanged,
            this,
            &DlgPrefCrate::showSelectedServer);
    connect(m_pRemove, &QPushButton::clicked, this, [this] {
        const QListWidgetItem* pItem = m_pServerList->currentItem();
        if (pItem && m_pServers) {
            m_pServers->removeServer(pItem->data(Qt::UserRole).toString());
        }
    });
    if (m_pServers) {
        connect(m_pServers, &CrateServers::serversChanged, this, &DlgPrefCrate::slotUpdate);
    }
    slotUpdate();
}

void DlgPrefCrate::slotUpdate() {
    const QString selected = m_pServerList->currentItem()
            ? m_pServerList->currentItem()->data(Qt::UserRole).toString()
            : QString();
    m_pServerList->clear();
    if (!m_pServers) {
        showSelectedServer();
        return;
    }
    const QList<Session*> sessions = m_pServers->sessions();
    bool keychainAvailable = true;
    for (Session* pSession : sessions) {
        keychainAvailable = keychainAvailable && pSession->keychainAvailable();
        const ServerProfile& profile = pSession->profile();
        // A server being added is listed once it is saved.
        if (!m_pServers->isSaved(profile.id)) {
            continue;
        }
        const QString name =
                profile.displayName.isEmpty() ? profile.baseUrl.host() : profile.displayName;
        auto* pItem = new QListWidgetItem(
                QStringLiteral("%1 — %2").arg(name, profile.baseUrl.toString()), m_pServerList);
        pItem->setData(Qt::UserRole, profile.id);
        if (profile.id == selected) {
            m_pServerList->setCurrentItem(pItem);
        }
        connect(pSession,
                &Session::stateChanged,
                this,
                &DlgPrefCrate::showSelectedServer,
                Qt::UniqueConnection);
    }
    m_pKeychainWarning->setVisible(!keychainAvailable);
    if (!m_pServerList->currentItem() && m_pServerList->count() > 0) {
        m_pServerList->setCurrentRow(0);
    }
    showSelectedServer();
}

void DlgPrefCrate::showSelectedServer() {
    const QListWidgetItem* pItem = m_pServerList->currentItem();
    Session* pSession = pItem && m_pServers
            ? m_pServers->session(pItem->data(Qt::UserRole).toString())
            : nullptr;
    m_pRemove->setEnabled(pSession != nullptr);
    if (!pSession) {
        m_pStatus->setText(m_pServerList->count() == 0
                        ? tr("No Crate server yet. Add yours to browse your library and "
                             "mix with Smart Mix.")
                        : QString());
        return;
    }
    const QString features = featuresText(m_pServers->lastCapabilities(pSession->profile().id));
    m_pStatus->setText(features.isEmpty()
                    ? stateText(*pSession)
                    : stateText(*pSession) + QStringLiteral("\n") + features);
}

} // namespace crate

#include "moc_dlgprefcrate.cpp"
