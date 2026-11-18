#include "crate/ui/cachestatusview.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
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
          m_pScroll(new QScrollArea(this)) {
    setObjectName(QStringLiteral("CrateCacheView"));
    const DialogMetrics& metrics = DialogMetrics::installed();
    // Everything scrolls together: nothing clips in a short pane
    m_pScroll->setObjectName(QStringLiteral("CrateCacheViewScroll"));
    m_pScroll->setFrameShape(QFrame::NoFrame);
    m_pScroll->setWidgetResizable(true);
    m_pScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* pContent = new QWidget(m_pScroll);
    pContent->setObjectName(QStringLiteral("CrateCacheViewContent"));
    m_pGauge = new LevelGauge(pContent);
    m_pUsage = textLabel("CrateCacheViewUsage", pContent);
    m_pOf = textLabel("CrateCacheViewOf", pContent);
    m_pCaption = textLabel("CrateCacheViewCaption", pContent);
    m_pDownloads = makeTable("CrateCacheViewDownloads");
    m_pDownloads->setParent(pContent);
    m_pNoDownloads = textLabel("CrateCacheViewEmpty", pContent);
    m_pOfflineSets = makeTable("CrateCacheViewOfflineSets");
    m_pOfflineSets->setParent(pContent);
    m_pNoOfflineSets = textLabel("CrateCacheViewEmpty", pContent);

    // The use, as the Crate page's cache section shows it (decks.css .usage)
    m_pHeader = new QWidget(pContent);
    m_pHeader->setObjectName(QStringLiteral("CrateCacheViewHeader"));
    auto* pUsage = new QHBoxLayout(m_pHeader);
    pUsage->setContentsMargins(0, 0, 0, metrics.px("usageBottom"));
    pUsage->setSpacing(metrics.px("usageGap"));
    pUsage->addWidget(m_pGauge);
    auto* pText = new QVBoxLayout();
    pText->setSpacing(0);
    pText->addStretch(1);
    pText->addWidget(monoLabel(tr("In use"), QStringLiteral("MonoCacheViewInUse"), m_pHeader));
    pText->addSpacing(metrics.px("usageNumberTop"));
    auto* pNumber = new QHBoxLayout();
    pNumber->setSpacing(metrics.px("buttonGapSm"));
    m_pUsage->setParent(m_pHeader);
    m_pOf->setParent(m_pHeader);
    m_pCaption->setParent(m_pHeader);
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
    m_pDownloads->horizontalHeader()->setSectionResizeMode(
            CacheDownloadsModel::Title, QHeaderView::Stretch);
    m_pOfflineSets->horizontalHeader()->setSectionResizeMode(
            CacheOfflineSetsModel::Name, QHeaderView::Stretch);

    auto* pLayout = new QVBoxLayout(pContent);
    m_pLayout = pLayout;
    pLayout->setContentsMargins(metrics.px("cacheViewPaddingX"),
            metrics.px("prefsSectionPaddingTop"),
            metrics.px("cacheViewPaddingX"),
            metrics.px("prefsSectionPaddingBottom"));
    pLayout->setSpacing(0);
    pLayout->addWidget(m_pHeader);
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
        pLayout->addWidget(
                monoLabel(section.title, QStringLiteral("MonoCacheViewSection"), pContent));
        pLayout->addSpacing(metrics.px("prefsSectionLabelGap"));
        pLayout->addWidget(section.pTable);
        pLayout->addWidget(section.pEmpty);
    }
    // The tables are as tall as their rows: what is left stays under them
    pLayout->addStretch(1);
    m_pScroll->setWidget(pContent);

    auto* pOuter = new QVBoxLayout(this);
    pOuter->setContentsMargins(0, 0, 0, 0);
    pOuter->setSpacing(0);
    pOuter->addWidget(m_pScroll);

    applyStyle();
    connect(pModel, &CacheStatusModel::changed, this, &CacheStatusView::showModel);
    showModel();
}

CacheStatusView::~CacheStatusView() = default;

QSize CacheStatusView::minimumSizeHint() const {
    // It scrolls: no height of its own may make the window grow
    return QSize(0, 0);
}

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
    // As tall as its rows: the view scrolls, not the table
    pTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pTable->verticalHeader()->hide();
    pTable->horizontalHeader()->setHighlightSections(false);
    pTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    pTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    return pTable;
}

void CacheStatusView::applyStyle() {
    const DelegateStyle& style = DelegateStyle::current();
    if (m_styled || !style.isValid()) {
        return;
    }
    m_styled = true;
    for (QTableView* pTable : {m_pDownloads, m_pOfflineSets}) {
        pTable->verticalHeader()->setDefaultSectionSize(style.rowHeight);
        pTable->verticalHeader()->setMinimumSectionSize(style.rowHeight);
        pTable->horizontalHeader()->setFixedHeight(style.headerHeight);
        new RowRules(pTable, style);
    }
    const auto replace = [](QTableView* pTable, int column, QAbstractItemDelegate* pDelegate) {
        QAbstractItemDelegate* pOld = pTable->itemDelegateForColumn(column);
        pTable->setItemDelegateForColumn(column, pDelegate);
        delete pOld;
    };
    replace(m_pDownloads,
            CacheDownloadsModel::Title,
            new TrackTextDelegate(style, TrackTextDelegate::Kind::Title, m_pDownloads));
    replace(m_pDownloads,
            CacheDownloadsModel::Artist,
            new TrackTextDelegate(style, TrackTextDelegate::Kind::Name, m_pDownloads));
    replace(m_pDownloads,
            CacheDownloadsModel::Purpose,
            new TrackTextDelegate(style, TrackTextDelegate::Kind::Meta, m_pDownloads));
    replace(m_pDownloads,
            CacheDownloadsModel::Progress,
            new delegates::DownloadProgressDelegate(style, m_pDownloads));
    replace(m_pOfflineSets,
            CacheOfflineSetsModel::Name,
            new TrackTextDelegate(style, TrackTextDelegate::Kind::Title, m_pOfflineSets));
    replace(m_pOfflineSets,
            CacheOfflineSetsModel::Tracks,
            new TrackTextDelegate(style, TrackTextDelegate::Kind::Meta, m_pOfflineSets));
    replace(m_pOfflineSets,
            CacheOfflineSetsModel::Size,
            new TrackTextDelegate(style, TrackTextDelegate::Kind::Meta, m_pOfflineSets));
    replace(m_pOfflineSets,
            CacheOfflineSetsModel::State,
            new delegates::CacheStateDelegate(style, m_pOfflineSets));
    for (QTableView* pTable : {m_pDownloads, m_pOfflineSets}) {
        pTable->horizontalHeader()->resizeSections(QHeaderView::ResizeToContents);
    }
    fitTables();
}

void CacheStatusView::fitTables() {
    for (QTableView* pTable : {m_pDownloads, m_pOfflineSets}) {
        int height = pTable->horizontalHeader()->height();
        for (int row = 0; row < pTable->model()->rowCount(); ++row) {
            height += pTable->rowHeight(row);
        }
        pTable->setFixedHeight(height);
    }
}

void CacheStatusView::setCompact(bool compact) {
    if (compact == m_compact) {
        return;
    }
    m_compact = compact;
    const DialogMetrics& metrics = DialogMetrics::installed();
    // The caption goes to the use's tooltip, and the paddings shrink
    m_pCaption->setVisible(!compact);
    const int top = compact ? metrics.px("usageBottom") : metrics.px("prefsSectionPaddingTop");
    m_pLayout->setContentsMargins(metrics.px("cacheViewPaddingX"),
            top,
            metrics.px("cacheViewPaddingX"),
            metrics.px("prefsSectionPaddingBottom"));
}

void CacheStatusView::showEvent(QShowEvent* pEvent) {
    // The skin sets its style after the library binds its views
    applyStyle();
    QWidget::showEvent(pEvent);
}

void CacheStatusView::resizeEvent(QResizeEvent* pEvent) {
    QWidget::resizeEvent(pEvent);
    // Short of the header and a section's first rows: the compact header
    const DelegateStyle& style = DelegateStyle::current();
    const int rows = style.isValid() ? style.headerHeight + 2 * style.rowHeight : 0;
    setCompact(height() < 2 * m_pHeader->sizeHint().height() + rows);
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
    if (m_pModel->inUseBytes() > 0) {
        //: The cached tracks loaded in a deck or queued in Auto DJ, which
        //: lowering the limit or clearing the cache does not delete
        caption += tr(" · %1 in use").arg(formatCacheBytes(m_pModel->inUseBytes()));
    }
    m_pCaption->setText(caption);

    const bool anyDownload = m_pModel->downloads()->rowCount() > 0;
    m_pDownloads->setVisible(anyDownload);
    m_pNoDownloads->setVisible(!anyDownload);
    const bool anySet = m_pModel->offlineSets()->rowCount() > 0;
    m_pOfflineSets->setVisible(anySet);
    m_pNoOfflineSets->setVisible(!anySet);
    m_pUsage->setToolTip(caption);
    fitTables();
}

void CacheStatusView::onShow() {
    m_pModel->refresh();
}

bool CacheStatusView::hasFocus() const {
    return QWidget::hasFocus();
}

} // namespace crate
