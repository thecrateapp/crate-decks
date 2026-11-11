#include "crate/ui/crateprefsections.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "crate/ui/delegates/delegatestyle.h"
#include "crate/ui/dialogs/dialogstyle.h"
#include "crate/ui/dialogs/levelgauge.h"
#include "moc_crateprefsections.cpp"

namespace crate {

namespace {

constexpr double kBytesPerGigabyte = 1000.0 * 1000.0 * 1000.0;

QLabel* textLabel(const QString& text, const char* objectName, QWidget* pParent) {
    auto* pLabel = new QLabel(text, pParent);
    pLabel->setObjectName(QLatin1String(objectName));
    pLabel->setTextFormat(Qt::PlainText);
    return pLabel;
}

/// Widgets left to right that wrap onto a new line when the width runs out,
/// as the admin's SegmentedControl (flex-wrap).
class FlowLayout : public QLayout {
  public:
    FlowLayout(int spacing, QWidget* pParent)
            : QLayout(pParent),
              m_spacing(spacing) {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() override {
        while (QLayoutItem* pItem = takeAt(0)) {
            delete pItem;
        }
    }

    void addItem(QLayoutItem* pItem) override {
        m_items.append(pItem);
    }
    int count() const override {
        return static_cast<int>(m_items.size());
    }
    QLayoutItem* itemAt(int index) const override {
        return m_items.value(index);
    }
    QLayoutItem* takeAt(int index) override {
        return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override {
        return {};
    }
    bool hasHeightForWidth() const override {
        return true;
    }
    int heightForWidth(int width) const override {
        return arrange(QRect(0, 0, width, 0), false);
    }
    QSize minimumSize() const override {
        QSize size;
        for (const QLayoutItem* pItem : m_items) {
            size = size.expandedTo(pItem->minimumSize());
        }
        return size;
    }
    QSize sizeHint() const override {
        return minimumSize();
    }
    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        arrange(rect, true);
    }

  private:
    int arrange(const QRect& rect, bool place) const {
        int x = rect.x();
        int y = rect.y();
        int lineHeight = 0;
        for (QLayoutItem* pItem : m_items) {
            const QSize size = pItem->sizeHint();
            if (x > rect.x() && x + size.width() > rect.right() + 1) {
                x = rect.x();
                y += lineHeight + m_spacing;
                lineHeight = 0;
            }
            if (place) {
                pItem->setGeometry(QRect(QPoint(x, y), size));
            }
            x += size.width() + m_spacing;
            lineHeight = std::max(lineHeight, size.height());
        }
        return y + lineHeight - rect.y();
    }

    QList<QLayoutItem*> m_items;
    const int m_spacing;
};

/// A row with a label of `labelWidth` on the left (decks.css .prow).
QHBoxLayout* addLabelledRow(QVBoxLayout* pRows, const QString& label, QWidget* pParent) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pRow = new QHBoxLayout();
    pRow->setSpacing(metrics.px("prefsRowGap"));
    auto* pLabel = textLabel(label, "CratePrefsRowLabel", pParent);
    pLabel->setFixedWidth(metrics.px("prefsRowLabelWidthColumn"));
    pRow->addWidget(pLabel);
    auto* pHolder = new QWidget(pParent);
    pHolder->setMinimumHeight(metrics.px("heightControlLg"));
    pHolder->setLayout(pRow);
    pRow->setContentsMargins(0, 0, 0, 0);
    pRows->addWidget(pHolder);
    return pRow;
}

} // namespace

PrefsSection::PrefsSection(const QString& title, QWidget* pParent)
        : QFrame(pParent),
          m_pRows(new QVBoxLayout()) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    setObjectName(QStringLiteral("CratePrefsSection"));
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0,
            metrics.px("prefsSectionPaddingTop"),
            0,
            metrics.px("prefsSectionPaddingBottom"));
    pLayout->setSpacing(metrics.px("prefsSectionLabelGap"));
    pLayout->addWidget(monoLabel(title, QStringLiteral("MonoPrefsSection"), this, true));
    m_pRows->setContentsMargins(0, 0, 0, 0);
    m_pRows->setSpacing(0);
    pLayout->addLayout(m_pRows);
}

QCheckBox* PrefsSection::addToggle(const QString& title, const QString& hint) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pRow = new QFrame(this);
    pRow->setObjectName(QStringLiteral("CratePrefsToggle"));
    auto* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(metrics.px("prefsRowGap"));
    auto* pText = new QVBoxLayout();
    pText->setContentsMargins(0, 0, 0, 0);
    pText->setSpacing(metrics.px("prefsHintGap"));
    pText->addStretch(1);
    pText->addWidget(textLabel(title, "CratePrefsRowLabel", pRow));
    pText->addWidget(textLabel(hint, "CratePrefsHint", pRow));
    pText->addStretch(1);
    pLayout->addLayout(pText, 1);
    auto* pSwitch = new QCheckBox(pRow);
    pSwitch->setObjectName(QStringLiteral("CratePrefsSwitch"));
    pSwitch->setAccessibleName(title);
    pLayout->addWidget(pSwitch, 0, Qt::AlignVCenter);
    m_pRows->addWidget(pRow);
    return pSwitch;
}

CacheSection::CacheSection(QWidget* pParent)
        : PrefsSection(tr("Cache"), pParent),
          m_pGauge(new LevelGauge(this)),
          m_pUsage(textLabel(QString(), "CrateCacheUsage", this)),
          m_pOf(textLabel(QString(), "CrateCacheOf", this)),
          m_pCaption(textLabel(QString(), "CrateCacheCaption", this)),
          m_pLimit(new QSlider(Qt::Horizontal, this)),
          m_pLimitValue(textLabel(QString(), "CrateCacheLimitValue", this)),
          m_pLocation(new QLineEdit(this)),
          m_pLocationHint(textLabel(QString(), "CratePrefsHint", this)) {
    const DialogMetrics& metrics = DialogMetrics::installed();

    // Level ring, use and the clear button (decks.css .usage)
    auto* pUsage = new QHBoxLayout();
    pUsage->setContentsMargins(0, 0, 0, metrics.px("usageBottom"));
    pUsage->setSpacing(metrics.px("usageGap"));
    pUsage->addWidget(m_pGauge);
    auto* pText = new QVBoxLayout();
    pText->setSpacing(0);
    pText->addWidget(monoLabel(tr("In use"), QStringLiteral("MonoCacheInUse"), this));
    pText->addSpacing(metrics.px("usageNumberTop"));
    auto* pNumber = new QHBoxLayout();
    pNumber->setSpacing(metrics.px("buttonGapSm"));
    pNumber->addWidget(m_pUsage, 0, Qt::AlignBaseline);
    pNumber->addWidget(m_pOf, 0, Qt::AlignBaseline);
    pNumber->addStretch(1);
    pText->addLayout(pNumber);
    pText->addWidget(m_pCaption);
    pUsage->addLayout(pText);
    pUsage->addStretch(1);
    m_pClear = new QPushButton(tr("Clear cache"), this);
    m_pClear->setObjectName(QStringLiteral("crateClearCache"));
    setButtonVariant(m_pClear, "danger");
    pUsage->addWidget(m_pClear, 0, Qt::AlignVCenter);
    rows()->addLayout(pUsage);

    QHBoxLayout* pLimit = addLabelledRow(rows(), tr("Size limit"), this);
    m_pLimit->setObjectName(QStringLiteral("crateCacheLimit"));
    pLimit->addWidget(m_pLimit, 1);
    m_pLimitValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_pLimitValue->setMinimumWidth(metrics.px("cacheLimitWidth"));
    // Gigabytes, as the mockups show the limit
    m_pLimit->setRange(0, metrics.px("cacheLimitMaximum"));
    // Chosen when the slider is let go: every step on the way would evict
    m_pLimit->setTracking(false);
    pLimit->addWidget(m_pLimitValue);
    connect(m_pLimit, &QSlider::sliderMoved, this, [this](int gigabytes) {
        m_pLimitValue->setText(cacheLimitText(static_cast<qint64>(gigabytes * kBytesPerGigabyte)));
    });
    connect(m_pLimit, &QSlider::valueChanged, this, [this](int gigabytes) {
        emit limitChosen(static_cast<qint64>(gigabytes * kBytesPerGigabyte));
    });

    QHBoxLayout* pLocation = addLabelledRow(rows(), tr("Location"), this);
    m_pLocation->setObjectName(QStringLiteral("crateCacheLocation"));
    m_pLocation->setReadOnly(true);
    pLocation->addWidget(m_pLocation, 1);
    m_pChoose = new QPushButton(tr("Choose…"), this);
    m_pChoose->setObjectName(QStringLiteral("crateChooseCache"));
    setButtonVariant(m_pChoose, "ghost");
    pLocation->addWidget(m_pChoose);
    m_pLocationHint->setText(tr("takes effect the next time Crate Decks starts"));
    m_pLocationHint->setContentsMargins(
            metrics.px("prefsRowLabelWidthColumn") + metrics.px("prefsRowGap"), 0, 0, 0);
    m_pLocationHint->hide();
    rows()->addWidget(m_pLocationHint);

    m_pPrefetch = addToggle(tr("Prefetch Auto DJ"),
            tr("keeps the next 3 tracks of the Auto DJ queue downloaded"));
    m_pPrewarm = addToggle(
            tr("Prewarm compatibles"), tr("downloads the top 5 of the Compatibles view"));
    m_pProgressive = addToggle(tr("Progressive playback"),
            tr("FLAC, AAC, ALAC, Opus · plays before the download ends"));
    // Plans F5 and F4b
    m_pPrewarm->parentWidget()->hide();
    m_pProgressive->parentWidget()->hide();
}

void CacheSection::setUsage(qint64 usedBytes, qint64 limitBytes, int tracks, int downloading) {
    m_pGauge->setValue(limitBytes > 0 ? static_cast<double>(usedBytes) / limitBytes : 0);
    m_pUsage->setText(cacheUsageNumber(usedBytes));
    m_pOf->setText(tr("of %1").arg(cacheLimitText(limitBytes)));
    m_pCaption->setText(tr("%1 tracks · %2 downloading")
                    .arg(delegates::groupThousands(QString::number(tracks)),
                            delegates::groupThousands(QString::number(downloading))));
    if (!m_pLimit->isSliderDown()) {
        // Showing the limit does not choose it
        const QSignalBlocker blocker(m_pLimit);
        m_pLimit->setValue(static_cast<int>(std::lround(limitBytes / kBytesPerGigabyte)));
        m_pLimitValue->setText(cacheLimitText(limitBytes));
    }
}

void CacheSection::setLocation(const QString& path, bool atNextStart) {
    m_pLocation->setText(path);
    m_pLocationHint->setVisible(atNextStart);
}

void CacheSection::setMinimumLimit(qint64 bytes) {
    const QSignalBlocker blocker(m_pLimit);
    m_pLimit->setMinimum(static_cast<int>(std::ceil(bytes / kBytesPerGigabyte)));
}

AnalysisSection::AnalysisSection(QWidget* pParent)
        : PrefsSection(tr("Crate analysis"), pParent),
          m_pKey(addToggle(tr("Key from Crate"),
                  tr("instead of the key detection of Crate Decks"))),
          m_pGain(addToggle(tr("Gain from loudness"),
                  tr("ReplayGain from Crate's LUFS for tracks without one"))) {
}

SmartMixSection::SmartMixSection(QWidget* pParent)
        : PrefsSection(tr("Auto DJ and Smart Mix"), pParent),
          m_pSmartMix(nullptr) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    rows()->addSpacing(metrics.px("segmentLabelTop") - metrics.px("prefsSectionLabelGap"));
    rows()->addWidget(monoLabel(tr("Auto DJ transition"), QStringLiteral("MonoPrefsLabel"), this));
    rows()->addSpacing(metrics.px("segmentLabelBottom"));

    // The admin's SegmentedControl: one active chip, filled in accentHi
    auto* pChipsBox = new QWidget(this);
    auto* pChips = new FlowLayout(metrics.px("chipGap"), pChipsBox);
    auto* pGroup = new QButtonGroup(this);
    const QString icons = designPath() + QStringLiteral("icons/");
    for (const QString& text :
            {tr("Crate Smart Mix"), tr("Full intro + outro"), tr("Fade at outro")}) {
        auto* pChip = new QPushButton(text, pChipsBox);
        pChip->setObjectName(QStringLiteral("CrateChip"));
        pChip->setCheckable(true);
        pChip->setAutoDefault(false);
        pGroup->addButton(pChip);
        pChips->addWidget(pChip);
        if (!m_pSmartMix) {
            m_pSmartMix = pChip;
            QIcon icon;
            icon.addFile(icons + QStringLiteral("stars_idle.svg"),
                    QSize(),
                    QIcon::Normal,
                    QIcon::Off);
            icon.addFile(icons + QStringLiteral("stars_onaccent.svg"),
                    QSize(),
                    QIcon::Normal,
                    QIcon::On);
            pChip->setIcon(icon);
            pChip->setChecked(true);
        }
    }
    rows()->addWidget(pChipsBox);
    rows()->addSpacing(metrics.px("segmentBottom"));

    addToggle(tr("Assistant follows the master deck"), tr("plans master deck → the other deck"))
            ->setChecked(true);
    addToggle(tr("Bass swap"), tr("on servers that announce bass handoff"))->setChecked(true);
    addToggle(tr("Long beatmatch"), tr("blends up to 64 bars"))->setChecked(true);
    addToggle(tr("AI assisted mixing"), tr("needs a server with Smart Mix 3"));
}

QString cacheUsageNumber(qint64 usedBytes) {
    return QString::number(usedBytes / kBytesPerGigabyte, 'f', 1);
}

QString cacheLimitText(qint64 limitBytes) {
    return QStringLiteral("%1 GB").arg(QString::number(limitBytes / kBytesPerGigabyte, 'f', 0));
}

} // namespace crate
