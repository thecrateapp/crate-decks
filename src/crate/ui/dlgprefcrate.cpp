#include "crate/ui/dlgprefcrate.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include "crate/crateservers.h"
#include "crate/session/cratesession.h"
#include "crate/ui/crateaddserverwidget.h"
#include "crate/ui/crateprefsections.h"
#include "crate/ui/dialogs/dialogstyle.h"

namespace crate {

namespace {

QLabel* plainLabel(const QString& text, const char* objectName, QWidget* pParent) {
    auto* pLabel = new QLabel(text, pParent);
    pLabel->setObjectName(QLatin1String(objectName));
    // Server names and messages are shown as they are, never as rich text
    pLabel->setTextFormat(Qt::PlainText);
    return pLabel;
}

} // namespace

DlgPrefCrate::DlgPrefCrate(QWidget* pParent, CrateServers* pServers)
        : DlgPreferencePage(pParent),
          m_pServers(pServers),
          m_pServerList(new QListWidget(this)),
          m_pStatus(plainLabel(QString(), "crateServerStatus", this)),
          m_pKeychainWarning(plainLabel(QString(), "crateKeychainWarning", this)),
          m_pAdd(new QPushButton(tr("Add a server…"), this)),
          m_pAddBox(new QWidget(this)),
          m_pAddServer(new AddServerWidget(pServers, m_pAddBox)),
          m_pCache(new CacheSection(this)),
          m_pSmartMix(new SmartMixSection(this)),
          m_pColumns(new QWidget(this)) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    m_pServerList->setObjectName(QStringLiteral("crateServers"));
    m_pServerList->setSelectionMode(QAbstractItemView::NoSelection);
    m_pServerList->setFocusPolicy(Qt::NoFocus);
    m_pServerList->setFrameShape(QFrame::NoFrame);
    m_pServerList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pServerList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pAdd->setObjectName(QStringLiteral("crateAddServer"));
    setButtonVariant(m_pAdd, "primary");
    m_pStatus->setWordWrap(true);
    m_pKeychainWarning->setWordWrap(true);
    m_pKeychainWarning->setText(
            tr("The system keychain is not available: you will have to sign in again "
               "after Crate Decks restarts."));
    m_pKeychainWarning->hide();

    auto* pIntro = plainLabel(QString(), "CratePrefsIntro", this);
    setParagraph(pIntro,
            tr("Crate servers are the library and the brain of Crate Decks: your tracks, "
               "Crate's analysis, recommendations and Smart Mix. Sessions are kept in the "
               "system keychain; nothing is stored in mixxx.cfg."),
            "fontSizeCaption");
    pIntro->setMaximumWidth(metrics.px("prefsIntroMaxWidth"));

    // Servers
    auto* pServersSection = new PrefsSection(tr("Servers"), this);
    pServersSection->rows()->addWidget(m_pServerList);
    pServersSection->rows()->addWidget(m_pStatus);
    auto* pAddRow = new QHBoxLayout();
    pAddRow->setContentsMargins(0, metrics.px("addServerTop"), 0, 0);
    pAddRow->setSpacing(metrics.px("helperGap"));
    pAddRow->addWidget(m_pAdd);
    auto* pHelper = plainLabel(tr("HTTPS by default · HTTP only on this computer, or on the "
                                  "local network after confirming"),
            "CrateFormHelper",
            this);
    pAddRow->addWidget(pHelper, 1);
    pServersSection->rows()->addLayout(pAddRow);
    m_pAddBox->setObjectName(QStringLiteral("crateAddServerBox"));
    auto* pAddLayout = new QVBoxLayout(m_pAddBox);
    pAddLayout->setContentsMargins(0, metrics.px("addServerTop"), 0, 0);
    pAddLayout->addWidget(m_pAddServer);
    m_pAddBox->hide();
    pServersSection->rows()->addWidget(m_pAddBox);
    pServersSection->rows()->addWidget(m_pKeychainWarning);

    // Cache and Auto DJ and Smart Mix, side by side (decks.css .pcols),
    // hidden until F4 and F6 give them their data
    auto* pColumns = new QHBoxLayout(m_pColumns);
    pColumns->setContentsMargins(0, 0, 0, 0);
    pColumns->setSpacing(metrics.px("prefsColumnGap"));
    pColumns->addWidget(m_pCache, 115, Qt::AlignTop);
    pColumns->addWidget(m_pSmartMix, 100, Qt::AlignTop);
    m_pCache->hide();
    m_pSmartMix->hide();

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setSpacing(0);
    pLayout->addWidget(pIntro);
    pLayout->addSpacing(metrics.px("prefsIntroGap"));
    pLayout->addWidget(pServersSection);
    pLayout->addWidget(m_pColumns);
    pLayout->addStretch(1);

    connect(m_pAdd, &QPushButton::clicked, m_pAddBox, &QWidget::show);
    connect(m_pAddServer, &AddServerWidget::cancelled, m_pAddBox, &QWidget::hide);
    connect(m_pAddServer, &AddServerWidget::serverAdded, m_pAddBox, &QWidget::hide);
    if (m_pServers) {
        connect(m_pServers, &CrateServers::serversChanged, this, &DlgPrefCrate::slotUpdate);
    }
    slotUpdate();
}

void DlgPrefCrate::slotUpdate() {
    if (!m_pServers) {
        showRows({});
        return;
    }
    bool keychainAvailable = true;
    const QList<Session*> sessions = m_pServers->sessions();
    for (Session* pSession : sessions) {
        keychainAvailable = keychainAvailable && pSession->keychainAvailable();
        connect(pSession,
                &Session::stateChanged,
                this,
                &DlgPrefCrate::slotUpdate,
                Qt::UniqueConnection);
    }
    m_pKeychainWarning->setVisible(!keychainAvailable);
    showRows(serverRows(*m_pServers));
}

void DlgPrefCrate::showServerRows(const QList<ServerRow>& rows) {
    showRows(rows);
}

void DlgPrefCrate::showRows(const QList<ServerRow>& rows) {
    m_pServerList->clear();
    int height = 0;
    for (const ServerRow& row : rows) {
        auto* pItem = new QListWidgetItem(
                QStringLiteral("%1 — %2").arg(row.name, row.meta.value(0)), m_pServerList);
        pItem->setData(Qt::UserRole, row.id);
        QWidget* pRow = rowWidget(row);
        m_pServerList->setItemWidget(pItem, pRow);
        // Measured once it is inside the dialog, with the dialog's style
        pRow->ensurePolished();
        const QList<QWidget*> children = pRow->findChildren<QWidget*>();
        for (QWidget* pChild : children) {
            pChild->ensurePolished();
        }
        pItem->setSizeHint(QSize(0, pRow->sizeHint().height()));
        height += pRow->sizeHint().height();
    }
    // As tall as its rows: the page scrolls, not the list
    m_pServerList->setFixedHeight(height);
    m_pServerList->setVisible(!rows.isEmpty());
    m_pStatus->setText(rows.isEmpty() ? tr("No Crate server yet. Add yours to browse your "
                                           "library and mix with Smart Mix.")
                                      : QString());
    m_pStatus->setVisible(rows.isEmpty());
}

QWidget* DlgPrefCrate::rowWidget(const ServerRow& row) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pRow = new QFrame();
    pRow->setObjectName(QStringLiteral("CrateServerRow"));
    auto* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(0, metrics.px("serverPaddingY"), 0, metrics.px("serverPaddingY"));
    pLayout->setSpacing(metrics.px("serverGap"));

    auto* pDot = new QLabel(pRow);
    pDot->setObjectName(QStringLiteral("CrateServerDot"));
    pDot->setProperty("crateState", row.stateName());
    pLayout->addWidget(pDot, 0, Qt::AlignVCenter);

    auto* pText = new QVBoxLayout();
    pText->setSpacing(0);
    pText->addWidget(plainLabel(row.name, "CrateServerName", pRow));
    pText->addSpacing(metrics.px("serverNameGap"));
    pText->addWidget(plainLabel(row.meta.join(QStringLiteral("  ·  ")), "CrateServerMeta", pRow));
    if (!row.features.isEmpty()) {
        pText->addSpacing(metrics.px("featuresTop"));
        auto* pFeatures = new QHBoxLayout();
        pFeatures->setSpacing(metrics.px("featureGap"));
        for (const ServerRow::Feature& feature : row.features) {
            auto* pBadge = plainLabel(feature.name, "CrateFeatureBadge", pRow);
            pBadge->setProperty("available", feature.available);
            pFeatures->addWidget(pBadge);
        }
        pFeatures->addStretch(1);
        pText->addLayout(pFeatures);
    }
    pLayout->addLayout(pText, 1);

    auto* pState = plainLabel(row.stateText(), "CrateStateBadge", pRow);
    pState->setProperty("crateState", row.stateName());
    pLayout->addWidget(pState, 0, Qt::AlignVCenter);
    auto* pRemove = new QPushButton(tr("Sign out and remove"), pRow);
    pRemove->setObjectName(QStringLiteral("crateRemoveServer"));
    setButtonVariant(pRemove, "danger");
    pLayout->addWidget(pRemove, 0, Qt::AlignVCenter);
    const QString id = row.id;
    connect(pRemove, &QPushButton::clicked, this, [this, id] {
        if (m_pServers && !id.isEmpty()) {
            m_pServers->removeServer(id);
        }
    });
    return pRow;
}

} // namespace crate

#include "moc_dlgprefcrate.cpp"
