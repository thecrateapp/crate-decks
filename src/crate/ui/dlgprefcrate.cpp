#include "crate/ui/dlgprefcrate.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include "crate/cache/audiocache.h"
#include "crate/cratepreferences.h"
#include "crate/crateservers.h"
#include "crate/session/cratesession.h"
#include "crate/ui/cachestatusmodel.h"
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
          m_pAnalysis(new AnalysisSection(this)),
          m_pStems(new StemsSection(this)),
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

    // Cache and Auto DJ and Smart Mix, side by side (decks.css .pcols).
    // Smart Mix stays hidden until F6 gives it its data; Crate analysis
    // goes under it. Without servers there is nothing to set.
    auto* pColumns = new QHBoxLayout(m_pColumns);
    pColumns->setContentsMargins(0, 0, 0, 0);
    pColumns->setSpacing(metrics.px("prefsColumnGap"));
    pColumns->addWidget(m_pCache, 115, Qt::AlignTop);
    auto* pRight = new QVBoxLayout();
    pRight->setContentsMargins(0, 0, 0, 0);
    pRight->setSpacing(0);
    pRight->addWidget(m_pSmartMix);
    pRight->addWidget(m_pAnalysis);
    pRight->addWidget(m_pStems);
    pRight->addStretch(1);
    pColumns->addLayout(pRight, 100);
    m_pCache->hide();
    m_pSmartMix->hide();
    m_pAnalysis->hide();
    m_pStems->hide();

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
        bindPreferences();
    }
    slotUpdate();
}

void DlgPrefCrate::bindPreferences() {
    CratePreferences* const pPreferences = m_pServers->preferences();
    CacheStatusModel::Options options;
    options.pCache = m_pServers->existingAudioCache();
    options.pPreferences = pPreferences;
    m_pCacheStatus = new CacheStatusModel(std::move(options), this);
    connect(m_pServers, &CrateServers::audioCacheCreated, this, [this] {
        if (m_pServers) {
            m_pCacheStatus->setSources(
                    m_pServers->existingAudioCache(), m_pServers->preferences());
        }
    });
    connect(m_pCacheStatus, &CacheStatusModel::changed, this, &DlgPrefCrate::showCache);

    m_pCache->setMinimumLimit(CratePreferences::kMinimumCacheLimitBytes);
    connect(m_pCache, &CacheSection::limitChosen, m_pCacheStatus, &CacheStatusModel::setLimitBytes);
    connect(m_pCache->chooseLocation(),
            &QPushButton::clicked,
            this,
            &DlgPrefCrate::chooseCacheLocation);
    connect(m_pCache->clear(), &QPushButton::clicked, this, &DlgPrefCrate::clearCache);
    connect(m_pCache->prefetchAutoDj(),
            &QCheckBox::toggled,
            pPreferences,
            &CratePreferences::setPrefetchAutoDj);
    connect(m_pAnalysis->keyFromCrate(),
            &QCheckBox::toggled,
            pPreferences,
            &CratePreferences::setKeyFromCrate);
    connect(m_pAnalysis->gainFromLufs(),
            &QCheckBox::toggled,
            pPreferences,
            &CratePreferences::setGainFromLufs);
    connect(m_pStems->useGpu(),
            &QCheckBox::toggled,
            pPreferences,
            &CratePreferences::setStemsOnGpu);
    connect(m_pStems->fineTunedModel(),
            &QCheckBox::toggled,
            pPreferences,
            &CratePreferences::setFineTunedStemModel);
    connect(m_pStems->separateWhilePlaying(),
            &QCheckBox::toggled,
            pPreferences,
            &CratePreferences::setSeparateWhilePlaying);
    connect(pPreferences,
            &CratePreferences::cacheDirectoryChanged,
            this,
            &DlgPrefCrate::showCache);
    connect(pPreferences, &CratePreferences::cacheLimitChanged, this, &DlgPrefCrate::showCache);
    m_pCache->show();
    m_pAnalysis->show();
    m_pStems->show();
    showPreferences();
}

void DlgPrefCrate::showPreferences() {
    if (!m_pServers) {
        return;
    }
    const CratePreferences* pPreferences = m_pServers->preferences();
    m_pCache->prefetchAutoDj()->setChecked(pPreferences->prefetchAutoDj());
    m_pAnalysis->keyFromCrate()->setChecked(pPreferences->keyFromCrate());
    m_pAnalysis->gainFromLufs()->setChecked(pPreferences->gainFromLufs());
    m_pStems->useGpu()->setChecked(pPreferences->stemsOnGpu());
    m_pStems->fineTunedModel()->setChecked(
            pPreferences->stemModel() == QLatin1String("htdemucs_ft"));
    m_pStems->separateWhilePlaying()->setChecked(pPreferences->separateWhilePlaying());
    showCache();
}

void DlgPrefCrate::showCache() {
    if (!m_pServers || !m_pCacheStatus) {
        return;
    }
    const CratePreferences* pPreferences = m_pServers->preferences();
    m_pCache->setUsage(m_pCacheStatus->usedBytes(),
            pPreferences->cacheLimitBytes(),
            m_pCacheStatus->readyFiles(),
            m_pCacheStatus->activeDownloads() + m_pCacheStatus->queuedDownloads());
    // The directory in use, or the one chosen for the next start
    const cache::AudioCache* pCache = m_pServers->existingAudioCache();
    const QString chosen = pPreferences->cacheDirectory();
    QString inUse = pCache ? pCache->directory() : chosen;
    if (inUse.isEmpty()) {
        inUse = cache::AudioCache::defaultDirectory();
    }
    const bool atNextStart = !chosen.isEmpty() && QDir::cleanPath(chosen) != QDir::cleanPath(inUse);
    m_pCache->setLocation(QDir::toNativeSeparators(atNextStart ? chosen : inUse), atNextStart);
}

void DlgPrefCrate::chooseCacheLocation() {
    // TEST_GAP: a file dialog is modal; the tests set the preference itself
    const QString directory = QFileDialog::getExistingDirectory(this,
            tr("Cache location"),
            QDir::fromNativeSeparators(m_pCache->location()->text()));
    if (!directory.isEmpty() && m_pServers) {
        m_pServers->preferences()->setCacheDirectory(directory);
    }
}

void DlgPrefCrate::clearCache() {
    // TEST_GAP: the confirmation is modal; AudioCache::clear() is tested
    cache::AudioCache* pCache = m_pServers ? m_pServers->existingAudioCache() : nullptr;
    if (!pCache) {
        return;
    }
    const QMessageBox::StandardButton answer = QMessageBox::question(this,
            tr("Clear cache"),
            tr("Delete the cached tracks from this computer? Offline sets and the "
               "downloads on their way stay."));
    if (answer == QMessageBox::Yes) {
        pCache->clear();
    }
}

void DlgPrefCrate::slotResetToDefaults() {
    if (!m_pServers) {
        return;
    }
    CratePreferences* const pPreferences = m_pServers->preferences();
    pPreferences->setCacheLimitBytes(CratePreferences::kDefaultCacheLimitBytes);
    pPreferences->setCacheDirectory(QString());
    pPreferences->setPrefetchAutoDj(true);
    pPreferences->setKeyFromCrate(false);
    pPreferences->setGainFromLufs(false);
    pPreferences->setStemsOnGpu(true);
    pPreferences->setFineTunedStemModel(false);
    pPreferences->setSeparateWhilePlaying(false);
    showPreferences();
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
    showCache();
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
