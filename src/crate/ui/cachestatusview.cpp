#include "crate/ui/cachestatusview.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QTableView>
#include <QVBoxLayout>

#include "crate/ui/cachestatusmodel.h"
#include "crate/ui/crateprefsections.h"
#include "crate/ui/delegates/celldelegate.h"
#include "crate/ui/delegates/delegatestyle.h"
#include "crate/ui/delegates/statedelegates.h"
#include "crate/ui/dialogs/dialogstyle.h"
#include "crate/ui/dialogs/levelgauge.h"
#include "moc_cachestatusview.cpp"

namespace crate {

namespace {

using delegates::DelegateStyle;
using delegates::TrackTextDelegate;

QLabel* textLabel(const char* objectName, QWidget* pParent) {
    auto* pLabel = new QLabel(pParent);
    pLabel->setObjectName(QLatin1String(objectName));
    pLabel->setTextFormat(Qt::PlainText);
    return pLabel;
}

/// The library's 1 px rule under each row of a table (decks.css .tracks
/// td), which the track tables get from crate/ui/skin/librarystyle.cpp.
class RowRules : public QObject {
  public:
    RowRules(QTableView* pTable, const DelegateStyle& style)
            : QObject(pTable),
              m_pTable(pTable),
              m_style(style) {
        pTable->viewport()->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pWatched == m_pTable->viewport() && pEvent->type() == QEvent::Paint &&
                m_style.isValid() && m_pTable->model()) {
            QPainter painter(m_pTable->viewport());
            const int width = m_pTable->viewport()->width();
            const int rows = m_pTable->model()->rowCount();
            for (int row = 0; row < rows; ++row) {
                const int bottom = m_pTable->rowViewportPosition(row) + m_pTable->rowHeight(row);
                painter.fillRect(QRect(0, bottom - m_style.rule, width, m_style.rule),
                        m_style.borderSubtle);
            }
        }
        return false;
    }

  private:
    QTableView* const m_pTable;
    const DelegateStyle m_style;
};

} // namespace

CacheStatusView::CacheStatusView(CacheStatusModel* pModel, QWidget* pParent)
        : QWidget(pParent),
          m_pModel(pModel),
          m_pGauge(new LevelGauge(this)),
          m_pUsage(textLabel("CrateCacheViewUsage", this)),
          m_pOf(textLabel("CrateCacheViewOf", this)),
          m_pCaption(textLabel("CrateCacheViewCaption", this)),
          m_pDownloads(makeTable("CrateCacheViewDownloads")),
          m_pNoDownloads(textLabel("CrateCacheViewEmpty", this)),
          m_pOfflineSets(makeTable("CrateCacheViewOfflineSets")),
          m_pNoOfflineSets(textLabel("CrateCacheViewEmpty", this)) {
    setObjectName(QStringLiteral("CrateCacheView"));
    const DialogMetrics& metrics = DialogMetrics::installed();
    const DelegateStyle& style = DelegateStyle::current();

    // The use, as the Crate page's cache section shows it (decks.css .usage)
    auto* pUsage = new QHBoxLayout();
    pUsage->setContentsMargins(0, 0, 0, metrics.px("usageBottom"));
    pUsage->setSpacing(metrics.px("usageGap"));
    pUsage->addWidget(m_pGauge);
    auto* pText = new QVBoxLayout();
    pText->setSpacing(0);
    pText->addStretch(1);
    pText->addWidget(monoLabel(tr("In use"), QStringLiteral("MonoCacheViewInUse"), this));
    pText->addSpacing(metrics.px("usageNumberTop"));
    auto* pNumber = new QHBoxLayout();
    pNumber->setSpacing(metrics.px("buttonGapSm"));
    pNumber->addWidget(m_pUsage, 0, Qt::AlignBaseline);
    pNumber->addWidget(m_pOf, 0, Qt::AlignBaseline);
    pNumber->addStretch(1);
    pText->addLayout(pNumber);
    pText->addWidget(m_pCaption);
    pText->addStretch(1);
    pUsage->addLayout(pText, 1);

    m_pNoDownloads->setText(tr("Nothing is downloading."));
    m_pNoOfflineSets->setText(
            tr("No offline sets. Right-click a playlist or crate of a Crate server and "
               "choose \"Download for offline use\"."));
    m_pNoOfflineSets->setWordWrap(true);
    m_pDownloads->setModel(pModel->downloads());
    m_pOfflineSets->setModel(pModel->offlineSets());
    if (style.isValid()) {
        m_pDownloads->setItemDelegateForColumn(CacheDownloadsModel::Title,
                new TrackTextDelegate(style, TrackTextDelegate::Kind::Title, m_pDownloads));
        m_pDownloads->setItemDelegateForColumn(CacheDownloadsModel::Artist,
                new TrackTextDelegate(style, TrackTextDelegate::Kind::Name, m_pDownloads));
        m_pDownloads->setItemDelegateForColumn(CacheDownloadsModel::Purpose,
                new TrackTextDelegate(style, TrackTextDelegate::Kind::Meta, m_pDownloads));
        m_pDownloads->setItemDelegateForColumn(CacheDownloadsModel::Progress,
                new TrackTextDelegate(style, TrackTextDelegate::Kind::Meta, m_pDownloads));
        m_pOfflineSets->setItemDelegateForColumn(CacheOfflineSetsModel::Name,
                new TrackTextDelegate(style, TrackTextDelegate::Kind::Title, m_pOfflineSets));
        m_pOfflineSets->setItemDelegateForColumn(CacheOfflineSetsModel::Tracks,
                new TrackTextDelegate(style, TrackTextDelegate::Kind::Meta, m_pOfflineSets));
        m_pOfflineSets->setItemDelegateForColumn(CacheOfflineSetsModel::Size,
                new TrackTextDelegate(style, TrackTextDelegate::Kind::Meta, m_pOfflineSets));
        m_pOfflineSets->setItemDelegateForColumn(CacheOfflineSetsModel::State,
                new delegates::CacheStateDelegate(style, m_pOfflineSets));
    }
    m_pDownloads->horizontalHeader()->setSectionResizeMode(
            CacheDownloadsModel::Title, QHeaderView::Stretch);
    m_pOfflineSets->horizontalHeader()->setSectionResizeMode(
            CacheOfflineSetsModel::Name, QHeaderView::Stretch);

    auto* pLayout = new QVBoxLayout(this);
    m_pLayout = pLayout;
    pLayout->setContentsMargins(metrics.px("cacheViewPaddingX"),
            metrics.px("prefsSectionPaddingTop"),
            metrics.px("cacheViewPaddingX"),
            metrics.px("prefsSectionPaddingBottom"));
    pLayout->setSpacing(0);
    pLayout->addLayout(pUsage);
    const struct {
        QString title;
        QTableView* pTable;
        QLabel* pEmpty;
    } sections[] = {
            {tr("Downloads"), m_pDownloads, m_pNoDownloads},
            {tr("Offline sets"), m_pOfflineSets, m_pNoOfflineSets},
    };
    for (const auto& section : sections) {
        pLayout->addSpacing(metrics.px("prefsSectionPaddingTop"));
        pLayout->addWidget(monoLabel(section.title, QStringLiteral("MonoCacheViewSection"), this));
        pLayout->addSpacing(metrics.px("prefsSectionLabelGap"));
        pLayout->addWidget(section.pTable, 1);
        pLayout->addWidget(section.pEmpty);
    }
    // Takes the height that no table does
    pLayout->addStretch(1);

    connect(pModel, &CacheStatusModel::changed, this, &CacheStatusView::showModel);
    showModel();
}

CacheStatusView::~CacheStatusView() = default;

QTableView* CacheStatusView::makeTable(const char* objectName) {
    auto* pTable = new QTableView(this);
    pTable->setObjectName(QLatin1String(objectName));
    pTable->setSelectionMode(QAbstractItemView::NoSelection);
    pTable->setFocusPolicy(Qt::NoFocus);
    pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pTable->setShowGrid(false);
    pTable->setWordWrap(false);
    pTable->setFrameShape(QFrame::NoFrame);
    pTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pTable->verticalHeader()->hide();
    pTable->horizontalHeader()->setHighlightSections(false);
    pTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    pTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    const DelegateStyle& style = DelegateStyle::current();
    if (style.isValid()) {
        pTable->verticalHeader()->setDefaultSectionSize(style.rowHeight);
        pTable->horizontalHeader()->setFixedHeight(style.headerHeight);
        new RowRules(pTable, style);
    }
    return pTable;
}

void CacheStatusView::showModel() {
    const qint64 used = m_pModel->usedBytes();
    const qint64 limit = m_pModel->limitBytes();
    m_pGauge->setValue(limit > 0 ? static_cast<double>(used) / static_cast<double>(limit) : 0.0);
    m_pUsage->setText(cacheUsageNumber(used));
    m_pOf->setText(tr("of %1").arg(cacheLimitText(limit)));
    const int downloading = m_pModel->activeDownloads() + m_pModel->queuedDownloads();
    QString caption = tr("%1 tracks · %2 downloading")
                              .arg(delegates::groupThousands(
                                           QString::number(m_pModel->readyFiles())),
                                      delegates::groupThousands(QString::number(downloading)));
    if (m_pModel->pinnedBytes() > 0) {
        caption += tr(" · %1 kept offline").arg(formatCacheBytes(m_pModel->pinnedBytes()));
    }
    m_pCaption->setText(caption);

    const bool anyDownload = m_pModel->downloads()->rowCount() > 0;
    m_pDownloads->setVisible(anyDownload);
    m_pNoDownloads->setVisible(!anyDownload);
    const bool anySet = m_pModel->offlineSets()->rowCount() > 0;
    m_pOfflineSets->setVisible(anySet);
    m_pNoOfflineSets->setVisible(!anySet);
    m_pLayout->setStretch(m_pLayout->count() - 1, anyDownload || anySet ? 0 : 1);
}

void CacheStatusView::onShow() {
    m_pModel->refresh();
}

bool CacheStatusView::hasFocus() const {
    return QWidget::hasFocus();
}

} // namespace crate
