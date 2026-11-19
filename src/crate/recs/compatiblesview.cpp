#include "crate/recs/compatiblesview.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QDateTime>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QThread>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <utility>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "crate/client/crateclient.h"
#include "crate/library/crateloadcoordinator.h"
#include "crate/recs/compatiblesmodel.h"
#include "crate/recs/harmony.h"
#include "crate/ui/delegates/delegateroles.h"
#include "crate/ui/delegates/delegatestyle.h"
#include "crate/ui/dialogs/dialogstyle.h"
#include "crate/ui/widgets/camelotlegend.h"
#include "crate/ui/widgets/camelotwheel.h"
#include "crate/ui/widgets/levelring.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_compatiblesview.cpp"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"
#include "widget/wtracktableview.h"

namespace crate::recs {

namespace {

const mixxx::Logger kLogger("crate::recs::Compatibles");

/// The server's largest page of compatible tracks.
constexpr int kServerMaxLimit = 100;

bool sameTrack(const std::optional<cache::TrackKey>& left,
        const std::optional<cache::TrackKey>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    return !left ||
            (left->serverId == right->serverId &&
                    left->entityUid.compare(right->entityUid, Qt::CaseInsensitive) == 0);
}

QString labelOf(const TrackPointer& pTrack) {
    if (!pTrack) {
        return QString();
    }
    const QString artist = pTrack->getArtist();
    const QString title = pTrack->getTitle();
    if (artist.isEmpty() || title.isEmpty()) {
        return title.isEmpty() ? artist : title;
    }
    return QStringLiteral("%1 - %2").arg(artist, title);
}

} // namespace

Compatibles::Compatibles(CompatiblesModel* pModel, Options options, QObject* parent)
        : QObject(parent),
          m_pModel(pModel),
          m_options(std::move(options)) {
    m_pool.setMaxThreadCount(1);
    m_debounce.setSingleShot(true);
    m_debounce.setTimerType(Qt::PreciseTimer);
    m_debounce.setInterval(m_options.debounce);
    connect(&m_debounce, &QTimer::timeout, this, &Compatibles::send);
}

Compatibles::~Compatibles() {
    cancel();
    m_pool.waitForDone();
}

void Compatibles::setFollow(Follow follow) {
    if (follow == m_follow) {
        return;
    }
    m_follow = follow;
    apply();
}

void Compatibles::setContext(CompatibleContext context) {
    if (context == m_context) {
        return;
    }
    m_context = context;
    if (m_followed) {
        // The same track, other results
        cancel();
        setState(State::Loading);
        m_debounce.start();
    }
}

Compatibles::Followed Compatibles::followedOf(const TrackPointer& pTrack) const {
    Followed followed;
    if (!pTrack) {
        return followed;
    }
    if (m_options.trackOfFile) {
        followed.track = m_options.trackOfFile(pTrack->getLocation());
    }
    followed.label = labelOf(pTrack);
    const double bpm = pTrack->getBpm();
    if (bpm > 0) {
        followed.bpm = bpm;
    }
    return followed;
}

void Compatibles::setMasterTrack(const TrackPointer& pTrack) {
    m_master = followedOf(pTrack);
    if (m_follow == Follow::MasterDeck) {
        apply();
    }
}

void Compatibles::setMasterDeck(const QString& group) {
    if (group == m_masterDeck) {
        return;
    }
    m_masterDeck = group;
    emit masterDeckChanged(group);
}

void Compatibles::setSelectedTrack(const TrackPointer& pTrack) {
    m_selected = followedOf(pTrack);
    if (m_follow == Follow::Selection) {
        apply();
    }
}

void Compatibles::setSelectedCrateTrack(const std::optional<cache::TrackKey>& track) {
    m_selected = Followed{track, QString(), std::nullopt};
    if (m_follow == Follow::Selection) {
        apply();
    }
}

void Compatibles::apply() {
    setFollowed(m_follow == Follow::MasterDeck ? m_master : m_selected);
}

void Compatibles::setFollowed(const Followed& followed) {
    if (sameTrack(followed.track, m_followed)) {
        if (!followed.label.isEmpty()) {
            m_followedLabel = followed.label;
        }
        if (followed.bpm) {
            m_followedBpm = followed.bpm;
        }
        return;
    }
    // A new track: the request in flight is stale
    cancel();
    m_followed = followed.track;
    m_followedLabel = followed.label;
    m_followedBpm = followed.bpm;
    if (!m_followed) {
        m_pModel->clear();
        setState(State::NoCrateTrack);
        return;
    }
    setState(State::Loading);
    m_debounce.start();
}

void Compatibles::cancel() {
    ++m_generation;
    m_debounce.stop();
    if (Reply* pReply = m_pReply.data()) {
        m_pReply = nullptr;
        pReply->disconnect(this);
        pReply->cancel();
    }
}

void Compatibles::send() {
    if (!m_followed) {
        return;
    }
    const cache::TrackKey seed = *m_followed;
    Client* pClient = m_options.clientFor ? m_options.clientFor(seed.serverId) : nullptr;
    if (!pClient) {
        m_pModel->clear();
        setState(State::Failed);
        return;
    }
    RequestOptions options;
    options.query.addQueryItem(QStringLiteral("context"), contextName(m_context));
    options.query.addQueryItem(QStringLiteral("limit"),
            QString::number(std::clamp(m_options.limit, 1, kServerMaxLimit)));
    options.rawBody = true;
    Reply* pReply = pClient->get(compatiblePath(seed.entityUid), options);
    m_pReply = pReply;
    const quint64 generation = ++m_generation;
    connect(pReply,
            &Reply::finished,
            this,
            [this, pReply, generation, seed](const JsonResult& result) {
                if (m_pReply == pReply) {
                    m_pReply = nullptr;
                }
                if (generation != m_generation) {
                    return;
                }
                if (!result.ok()) {
                    if (result.error().kind == Error::Kind::Cancelled) {
                        return;
                    }
                    m_pModel->clear();
                    if (result.error().kind == Error::Kind::Http &&
                            result.error().httpStatus == 404) {
                        // No track, or no usable Smart Mix profile of it
                        setState(State::NoProfile);
                        return;
                    }
                    kLogger.warning() << "Cannot get the compatible tracks:"
                                      << result.error().message;
                    setState(State::Failed);
                    return;
                }
                onReply(pReply, generation, seed);
            });
}

void Compatibles::onReply(Reply* pReply, quint64 generation, const cache::TrackKey& seed) {
    auto* pWatcher = new QFutureWatcher<Result<StoredCompatibles>>(this);
    connect(pWatcher,
            &QFutureWatcherBase::finished,
            this,
            [this, pWatcher, generation, seed] {
                pWatcher->deleteLater();
                const Result<StoredCompatibles> stored = pWatcher->result();
                if (generation != m_generation) {
                    return;
                }
                if (!stored.ok()) {
                    kLogger.warning() << "Cannot show the compatible tracks:"
                                      << stored.error().message;
                    m_pModel->clear();
                    setState(State::Failed);
                    return;
                }
                m_pModel->setResults(seed.serverId, seed.entityUid, stored.value());
                setState(State::Ready);
                if (m_options.prewarm && m_options.prewarmEnabled &&
                        m_options.prewarmEnabled()) {
                    QList<library::CrateTrackRef> best;
                    const auto& rows = stored.value().rows;
                    for (int i = 0; i < rows.size() && i < m_options.prewarmCount; ++i) {
                        library::CrateTrackRef track;
                        track.serverId = rows[i].serverId;
                        track.entityUid = rows[i].entityUid;
                        track.hints.format = rows[i].format;
                        track.hints.byteLength = rows[i].byteLength;
                        track.hints.sourceRevision = rows[i].sourceRevision;
                        best.append(track);
                    }
                    if (!best.isEmpty()) {
                        m_options.prewarm(best);
                    }
                }
            });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_options.pDbConnectionPool,
                    body = pReply->body(),
                    seed,
                    seedBpm = m_followedBpm,
                    attempts = std::max(1, m_options.writeAttempts),
                    retryDelay = m_options.writeRetryDelay]() -> Result<StoredCompatibles> {
                const Result<CompatibleTracks> compatibles = parseCompatibleTracks(body);
                if (!compatibles.ok()) {
                    return compatibles.error();
                }
                const mixxx::DbConnectionPooler pooler(pPool);
                const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
                const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                for (int attempt = 1;; ++attempt) {
                    library::WriteStatus status = library::WriteStatus::Failed;
                    Result<StoredCompatibles> stored = storeCompatibles(database,
                            seed.serverId,
                            seed.entityUid,
                            compatibles.value(),
                            now,
                            seedBpm,
                            &status);
                    if (status != library::WriteStatus::Busy || attempt >= attempts) {
                        return stored;
                    }
                    QThread::msleep(static_cast<unsigned long>(retryDelay.count() * attempt));
                }
            }));
}

void Compatibles::setState(State state) {
    if (state == m_state) {
        return;
    }
    m_state = state;
    emit stateChanged(state);
}

namespace {

// The ` · ` between the parts of a MonoLabel (MonoLabel.tsx)
const QString kSeparator = QStringLiteral(" · ");
const ConfigKey kMaximizedLibrary(
        QStringLiteral("[Skin]"), QStringLiteral("show_maximized_library"));
const ConfigKey kDeckCount(QStringLiteral("[App]"), QStringLiteral("num_decks"));
// How often the age of the results and the target deck are read again
constexpr int kClockMs = 1000;
// The best results the band shows the cache of (Compatibles' prewarm)
constexpr int kPrewarmRings = 5;

QLabel* textLabel(const char* objectName, QWidget* pParent) {
    auto* pLabel = new QLabel(pParent);
    pLabel->setObjectName(QLatin1String(objectName));
    pLabel->setTextFormat(Qt::PlainText);
    return pLabel;
}

void setTone(QWidget* pWidget, const QString& tone) {
    if (pWidget->property("crateTone").toString() == tone) {
        return;
    }
    pWidget->setProperty("crateTone", tone);
    pWidget->style()->unpolish(pWidget);
    pWidget->style()->polish(pWidget);
}

/// "A" for [Channel1], "B" for [Channel2]…
QString deckLetter(const QString& group) {
    const QString number = group.mid(8, group.size() - 9);
    bool ok = false;
    const int index = number.toInt(&ok);
    return ok && index > 0 ? QString(QChar(static_cast<char16_t>(u'A' + index - 1))) : QString();
}

double controlValue(const ConfigKey& key, double fallback) {
    const ControlObject* pControl = ControlObject::getControl(key, ControlFlag::NoWarnIfMissing);
    return pControl ? pControl->get() : fallback;
}

int deckCount() {
    return std::max(1, static_cast<int>(controlValue(kDeckCount, 2)));
}

QString bpmText(const std::optional<double>& bpm) {
    return bpm ? QStringLiteral("%1 BPM").arg(QString::number(*bpm, 'f', 2)) : QString();
}

/// genre-wheel.css .genre-taxonomy-summary__meter: a 1 px lane in border and
/// its share in accentHi
class ShareMeter : public QWidget {
  public:
    explicit ShareMeter(QWidget* pParent)
            : QWidget(pParent) {
        setObjectName(QStringLiteral("CrateCompatiblesMeter"));
        const auto& style = widgets::InstrumentStyle::installed();
        setFixedHeight(std::max(1, static_cast<int>(std::lround(style.strokeHairline))));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setValue(double value) {
        const double clamped = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
        if (clamped != m_value) {
            m_value = clamped;
            update();
        }
    }
    double value() const {
        return m_value;
    }

  protected:
    void paintEvent(QPaintEvent* /*pEvent*/) override {
        const auto& style = widgets::InstrumentStyle::installed();
        QPainter painter(this);
        painter.fillRect(rect(), style.border);
        const int lit = static_cast<int>(std::lround(width() * m_value));
        if (lit > 0) {
            painter.fillRect(QRect(0, 0, lit, height()), style.accentHi);
        }
    }

  private:
    double m_value = 0;
};

} // namespace

CompatiblesView::CompatiblesView(Compatibles* pCompatibles,
        QWidget* pTable,
        widgets::CamelotWheelModel* pWheelModel,
        QWidget* pParent)
        : QWidget(pParent),
          m_pCompatibles(pCompatibles),
          m_pTable(pTable),
          m_pWheelModel(pWheelModel) {
    setObjectName(QStringLiteral("CrateCompatibles"));
    setAttribute(Qt::WA_StyledBackground);
    // The library's search row is not this view's: its head has what the
    // view needs (librarystyle.cpp hides the row while it shows)
    setProperty("crateOwnHead", true);

    m_pHead = makeHead();
    m_pBand = makeBand();
    m_pStrip = makeStrip();
    m_pHint = textLabel("CrateCompatiblesHint", this);
    m_pHint->setWordWrap(true);
    m_pHint->setAlignment(Qt::AlignCenter);
    m_pTable->setParent(this);
    m_pTable->show();

    m_clock.setInterval(kClockMs);
    connect(&m_clock, &QTimer::timeout, this, [this] {
        updateCount();
        updateSelected();
    });

    if (m_pCompatibles) {
        connect(m_pCompatibles, &Compatibles::stateChanged, this, &CompatiblesView::update);
        connect(m_pCompatibles, &Compatibles::masterDeckChanged, this, &CompatiblesView::update);
        CompatiblesModel* pModel = m_pCompatibles->model();
        connect(pModel, &QAbstractItemModel::modelReset, this, &CompatiblesView::update);
        connect(pModel, &CompatiblesModel::loaded, this, &CompatiblesView::update);
        // The Cache column repaints as downloads move
        connect(pModel,
                &QAbstractItemModel::dataChanged,
                this,
                [this](const QModelIndex& topLeft, const QModelIndex& bottomRight) {
                    if (topLeft.column() <= CompatiblesModel::kCacheColumn &&
                            bottomRight.column() >= CompatiblesModel::kCacheColumn) {
                        updateCache();
                        updateSelected();
                    }
                });
    }
    update();
}

CompatiblesView::~CompatiblesView() = default;

QWidget* CompatiblesView::makeHead() {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pHead = new QWidget(this);
    pHead->setObjectName(QStringLiteral("CrateCompatiblesHead"));
    pHead->setAttribute(Qt::WA_StyledBackground);
    auto* pRow = new QHBoxLayout(pHead);
    pRow->setContentsMargins(
            metrics.px("cacheViewPaddingX"), 0, metrics.px("cacheViewPaddingX"), 0);
    pRow->setSpacing(metrics.px("viewHeadGap"));

    auto* pIcon = new QLabel(pHead);
    pIcon->setObjectName(QStringLiteral("CrateCompatiblesHeadIcon"));
    const int iconSize = metrics.px("chipHeight") / 2;
    pIcon->setPixmap(QIcon(designPath() + QStringLiteral("icons/stars_active.svg"))
                    .pixmap(iconSize, iconSize));
    pRow->addWidget(pIcon);
    pRow->addWidget(monoLabel(tr("Compatibles"), QStringLiteral("MonoCompatiblesEyebrow"), pHead));
    m_pHeadMeta = monoLabel(QString(), QStringLiteral("MonoCompatiblesMeta"), pHead);
    // It gives way to the chips when the head is short of room
    m_pHeadMeta->setMinimumWidth(1);
    pRow->addWidget(m_pHeadMeta, 1);

    const auto chipRow = [&](QLabel* pLabel,
                                 QList<QPushButton*>* pChips,
                                 const QList<std::pair<QString, int>>& items) {
        auto* pSegment = new QHBoxLayout();
        pSegment->setSpacing(metrics.px("chipGap"));
        pSegment->addWidget(pLabel);
        auto* pGroup = new QButtonGroup(pHead);
        for (const auto& [text, value] : items) {
            auto* pChip = new QPushButton(text, pHead);
            pChip->setObjectName(QStringLiteral("CrateChip"));
            pChip->setCheckable(true);
            pChip->setCursor(Qt::PointingHandCursor);
            pChip->setFocusPolicy(Qt::NoFocus);
            pChip->setProperty("crateValue", value);
            pGroup->addButton(pChip);
            pSegment->addWidget(pChip);
            pChips->append(pChip);
        }
        pRow->addLayout(pSegment);
    };
    //: The Compatibles view's choice of what to follow
    m_pFollowLabel = monoLabel(tr("Follow"), QStringLiteral("MonoCompatiblesFollow"), pHead);
    chipRow(m_pFollowLabel,
            &m_followChips,
            {
                    //: What the Compatibles view follows: the track of the deck on air
                    {tr("Master deck"), static_cast<int>(Compatibles::Follow::MasterDeck)},
                    //: What the Compatibles view follows: the track selected in the library
                    {tr("Selected track"), static_cast<int>(Compatibles::Follow::Selection)},
            });
    QIcon vinyl;
    const QString icons = designPath() + QStringLiteral("icons/");
    vinyl.addFile(icons + QStringLiteral("vinyl-record_idle.svg"),
            QSize(),
            QIcon::Normal,
            QIcon::Off);
    vinyl.addFile(icons + QStringLiteral("vinyl-record_onaccent.svg"),
            QSize(),
            QIcon::Normal,
            QIcon::On);
    m_followChips.first()->setIcon(vinyl);
    m_followChips.first()->setToolTip(
            tr("Find compatible tracks for the deck on air or for the selection"));
    //: How the DJ means to mix the next track, in the Compatibles view
    m_pContextLabel = monoLabel(tr("Context"), QStringLiteral("MonoCompatiblesContext"), pHead);
    chipRow(m_pContextLabel,
            &m_contextChips,
            {
                    //: How the DJ means to mix the next track: Smart Mix decides
                    {tr("Auto"), static_cast<int>(CompatibleContext::Auto)},
                    //: How the DJ means to mix the next track: a beatmatched blend
                    {tr("Beatmatch"), static_cast<int>(CompatibleContext::Beatmatch)},
                    //: How the DJ means to mix the next track: a smooth fade
                    {tr("Smooth"), static_cast<int>(CompatibleContext::Smooth)},
            });
    m_pCount = monoLabel(QString(), QStringLiteral("MonoCompatiblesCount"), pHead);
    pRow->addWidget(m_pCount);

    // The library's maximise button, which the hidden search row had
    if (ControlObject::exists(kMaximizedLibrary)) {
        m_pMaximized = new ControlProxy(kMaximizedLibrary, this);
        m_pMaximize = new QPushButton(pHead);
        m_pMaximize->setObjectName(QStringLiteral("CrateCompatiblesMaximize"));
        m_pMaximize->setCheckable(true);
        m_pMaximize->setFocusPolicy(Qt::NoFocus);
        m_pMaximize->setCursor(Qt::PointingHandCursor);
        m_pMaximize->setToolTip(tr("Maximize the library"));
        QIcon maximize;
        maximize.addFile(icons + QStringLiteral("maximize_idle.svg"),
                QSize(),
                QIcon::Normal,
                QIcon::Off);
        maximize.addFile(icons + QStringLiteral("maximize_hover.svg"),
                QSize(),
                QIcon::Active,
                QIcon::Off);
        maximize.addFile(icons + QStringLiteral("maximize_active.svg"),
                QSize(),
                QIcon::Normal,
                QIcon::On);
        m_pMaximize->setIcon(maximize);
        m_pMaximize->setIconSize(QSize(iconSize + iconSize / 4, iconSize + iconSize / 4));
        m_pMaximize->setChecked(m_pMaximized->get() > 0);
        connect(m_pMaximize, &QPushButton::toggled, this, [this](bool checked) {
            m_pMaximized->set(checked ? 1.0 : 0.0);
        });
        m_pMaximized->connectValueChanged(this, [this](double value) {
            const QSignalBlocker blocker(m_pMaximize);
            m_pMaximize->setChecked(value > 0);
        });
        pRow->addWidget(m_pMaximize);
    }

    for (QPushButton* pChip : std::as_const(m_followChips)) {
        connect(pChip, &QPushButton::clicked, this, [this, pChip] {
            if (m_pCompatibles) {
                m_pCompatibles->setFollow(
                        static_cast<Compatibles::Follow>(pChip->property("crateValue").toInt()));
            }
        });
    }
    for (QPushButton* pChip : std::as_const(m_contextChips)) {
        connect(pChip, &QPushButton::clicked, this, [this, pChip] {
            if (m_pCompatibles) {
                m_pCompatibles->setContext(
                        static_cast<CompatibleContext>(pChip->property("crateValue").toInt()));
            }
        });
    }
    return pHead;
}

QWidget* CompatiblesView::makeBand() {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pBand = new QWidget(this);
    pBand->setObjectName(QStringLiteral("CrateCompatiblesBand"));
    pBand->setAttribute(Qt::WA_StyledBackground);
    auto* pRow = new QHBoxLayout(pBand);
    pRow->setContentsMargins(metrics.px("cacheViewPaddingX"),
            metrics.px("compatBandPaddingTop"),
            metrics.px("cacheViewPaddingX"),
            metrics.px("compatBandPaddingBottom"));
    pRow->setSpacing(metrics.px("compatBandGap"));
    if (m_pWheelModel) {
        m_pWheel = new widgets::CamelotWheel(
                m_pWheelModel, widgets::InstrumentStyle::installed(), pBand);
        connect(m_pWheel,
                &widgets::CamelotWheel::keyClicked,
                this,
                &CompatiblesView::onKeyClicked);
        pRow->addWidget(m_pWheel, 0, Qt::AlignTop);
    }
    m_pSummary = makeSummary(pBand);
    pRow->addWidget(m_pSummary);
    m_pSide = makeSide(pBand);
    pRow->addWidget(m_pSide, 1);
    return pBand;
}

QWidget* CompatiblesView::makeSummary(QWidget* pParent) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pSummary = new QWidget(pParent);
    pSummary->setObjectName(QStringLiteral("CrateCompatiblesSummary"));
    pSummary->setAttribute(Qt::WA_StyledBackground);
    auto* pLayout = new QVBoxLayout(pSummary);
    pLayout->setContentsMargins(metrics.px("compatColumnPaddingLeft"), 0, 0, 0);
    pLayout->setSpacing(0);

    auto* pHeader = new QHBoxLayout();
    pHeader->setSpacing(metrics.px("chipGap"));
    //: The Compatibles view's summary of the followed track's key
    pHeader->addWidget(monoLabel(tr("Master key"),
            QStringLiteral("MonoCompatiblesMasterKey"),
            pSummary));
    pHeader->addStretch(1);
    m_pDeckBadge = textLabel("CrateBadge", pSummary);
    setTone(m_pDeckBadge, QStringLiteral("accent"));
    pHeader->addWidget(m_pDeckBadge);
    pLayout->addLayout(pHeader);
    pLayout->addSpacing(metrics.px("compatTitleTop"));
    m_pSummaryTitle = textLabel("CrateCompatiblesTitle", pSummary);
    setDisplayAxes(m_pSummaryTitle);
    pLayout->addWidget(m_pSummaryTitle);
    pLayout->addSpacing(metrics.px("compatTitleBottom"));
    m_pSummaryTrack = textLabel("CrateCompatiblesTrack", pSummary);
    pLayout->addWidget(m_pSummaryTrack);
    pLayout->addSpacing(metrics.px("compatMetricsTop"));

    // Results, Harmonic and Cached
    auto* pMetrics = new QWidget(pSummary);
    pMetrics->setObjectName(QStringLiteral("CrateCompatiblesMetrics"));
    pMetrics->setAttribute(Qt::WA_StyledBackground);
    auto* pGrid = new QGridLayout(pMetrics);
    pGrid->setContentsMargins(
            0, metrics.px("compatMetricsPaddingY"), 0, metrics.px("compatMetricsPaddingY"));
    pGrid->setHorizontalSpacing(metrics.px("helperGap"));
    pGrid->setVerticalSpacing(metrics.px("compatMetricGap"));
    const QString names[] = {
            //: How many tracks the Compatibles view found
            tr("Results"),
            //: How many of them mix in key
            tr("Harmonic"),
            //: How many of them are in the cache
            tr("Cached"),
    };
    for (int column = 0; column < 3; ++column) {
        pGrid->addWidget(monoLabel(names[column],
                                 QStringLiteral("MonoCompatiblesMetric"),
                                 pMetrics),
                0,
                column);
        QLabel* pFigure = textLabel("CrateCompatiblesFigure", pMetrics);
        pGrid->addWidget(pFigure, 1, column);
        pGrid->setColumnStretch(column, 1);
        m_metrics.append(pFigure);
    }
    pLayout->addWidget(pMetrics);
    pLayout->addSpacing(metrics.px("compatSharesTop"));

    // A meter per relation
    for (int share = 0; share < 5; ++share) {
        if (share > 0) {
            pLayout->addSpacing(metrics.px("compatSharesGap"));
        }
        auto* pRow = new QHBoxLayout();
        pRow->setSpacing(metrics.px("chipGap"));
        QLabel* pName = monoLabel(QString(), QStringLiteral("MonoCompatiblesShare"), pSummary);
        QLabel* pCount = textLabel("CrateCompatiblesShare", pSummary);
        pRow->addWidget(pName, 0, Qt::AlignBottom);
        pRow->addStretch(1);
        pRow->addWidget(pCount, 0, Qt::AlignBottom);
        pLayout->addLayout(pRow);
        pLayout->addSpacing(metrics.px("compatMeterTop"));
        auto* pMeter = new ShareMeter(pSummary);
        pLayout->addWidget(pMeter);
        m_shareLabels.append(pName);
        m_shareCounts.append(pCount);
        m_shareMeters.append(pMeter);
    }
    pLayout->addStretch(1);
    return pSummary;
}

QWidget* CompatiblesView::makeSide(QWidget* pParent) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pSide = new QWidget(pParent);
    pSide->setObjectName(QStringLiteral("CrateCompatiblesSide"));
    pSide->setAttribute(Qt::WA_StyledBackground);
    auto* pLayout = new QVBoxLayout(pSide);
    pLayout->setContentsMargins(metrics.px("compatColumnPaddingLeft"), 0, 0, 0);
    pLayout->setSpacing(metrics.px("compatSideGap"));
    pLayout->addWidget(new widgets::CamelotLegend(widgets::InstrumentStyle::installed(), pSide));

    // The cache of the best five, which Compatibles prewarms
    auto* pPrewarm = new QVBoxLayout();
    pPrewarm->setSpacing(metrics.px("compatPrewarmGap"));
    pPrewarm->addWidget(monoLabel(
            //: The cache of the best five compatible tracks, which Decks downloads ahead
            tr("Prewarm cache · top 5"),
            QStringLiteral("MonoCompatiblesPrewarm"),
            pSide));
    auto* pRings = new QHBoxLayout();
    pRings->setSpacing(metrics.px("compatPrewarmGap"));
    for (int ring = 0; ring < kPrewarmRings; ++ring) {
        auto* pRing = new widgets::LevelRing(widgets::InstrumentStyle::installed(), pSide);
        pRings->addWidget(pRing);
        m_prewarmRings.append(pRing);
    }
    m_pPrewarmLabel = textLabel("CrateCompatiblesPrewarm", pSide);
    pRings->addWidget(m_pPrewarmLabel);
    pRings->addStretch(1);
    pPrewarm->addLayout(pRings);
    pLayout->addLayout(pPrewarm);

    // The selected track, with what to do with it
    m_pSelected = new QWidget(pSide);
    m_pSelected->setObjectName(QStringLiteral("CrateCompatiblesSelected"));
    m_pSelected->setAttribute(Qt::WA_StyledBackground);
    auto* pSelected = new QVBoxLayout(m_pSelected);
    pSelected->setContentsMargins(0, metrics.px("compatSelectedPaddingTop"), 0, 0);
    pSelected->setSpacing(metrics.px("compatSelectedGap"));
    m_pSelectedEyebrow = monoLabel(
            QString(), QStringLiteral("MonoCompatiblesSelected"), m_pSelected);
    pSelected->addWidget(m_pSelectedEyebrow);
    auto* pTitle = new QHBoxLayout();
    pTitle->setSpacing(0);
    m_pSelectedTitle = textLabel("CrateCompatiblesSelectedTitle", m_pSelected);
    m_pSelectedArtist = textLabel("CrateCompatiblesSelectedArtist", m_pSelected);
    pTitle->addWidget(m_pSelectedTitle);
    pTitle->addWidget(m_pSelectedArtist);
    pTitle->addStretch(1);
    pSelected->addLayout(pTitle);
    auto* pBadges = new QHBoxLayout();
    pBadges->setSpacing(metrics.px("chipGap"));
    for (int badge = 0; badge < 4; ++badge) {
        QLabel* pBadge = textLabel("CrateBadge", m_pSelected);
        pBadges->addWidget(pBadge);
        m_badges.append(pBadge);
    }
    pBadges->addStretch(1);
    pSelected->addLayout(pBadges);
    pSelected->addSpacing(metrics.px("compatButtonsTop"));
    auto* pButtons = new QHBoxLayout();
    pButtons->setSpacing(metrics.px("chipGap"));
    const QString icons = designPath() + QStringLiteral("icons/");
    m_pLoad = new QPushButton(m_pSelected);
    QIcon vinyl;
    vinyl.addFile(icons + QStringLiteral("vinyl-record_active.svg"), QSize(), QIcon::Normal);
    vinyl.addFile(icons + QStringLiteral("vinyl-record_disabled.svg"), QSize(), QIcon::Disabled);
    m_pLoad->setIcon(vinyl);
    setButtonVariant(m_pLoad, "primary");
    //: Opens the Smart Mix assistant for the master deck and the selected track
    m_pPlan = new QPushButton(tr("Plan transition"), m_pSelected);
    QIcon stars;
    stars.addFile(icons + QStringLiteral("stars_idle.svg"), QSize(), QIcon::Normal);
    stars.addFile(icons + QStringLiteral("stars_hover.svg"), QSize(), QIcon::Active);
    stars.addFile(icons + QStringLiteral("stars_disabled.svg"), QSize(), QIcon::Disabled);
    m_pPlan->setIcon(stars);
    setButtonVariant(m_pPlan, "ghost");
    m_pPlan->setToolTip(
            tr("Load the track in the other deck and plan the transition to it with Smart Mix"));
    for (QPushButton* pButton : {m_pLoad, m_pPlan}) {
        pButton->setFocusPolicy(Qt::NoFocus);
        pButtons->addWidget(pButton);
    }
    pButtons->addStretch(1);
    pSelected->addLayout(pButtons);
    pLayout->addWidget(m_pSelected);
    pLayout->addStretch(1);

    connect(m_pLoad, &QPushButton::clicked, this, [this] {
        loadSelected(targetGroup());
    });
    connect(m_pPlan, &QPushButton::clicked, this, [this] {
        const QString group = targetGroup();
        if (group.isEmpty()) {
            return;
        }
        loadSelected(group);
        emit planTransitionRequested(group);
    });
    return pSide;
}

QWidget* CompatiblesView::makeStrip() {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pStrip = new QWidget(this);
    pStrip->setObjectName(QStringLiteral("CrateCompatiblesStrip"));
    pStrip->setAttribute(Qt::WA_StyledBackground);
    auto* pRow = new QHBoxLayout(pStrip);
    pRow->setContentsMargins(metrics.px("cacheViewPaddingX"),
            metrics.px("compatStripPaddingY"),
            metrics.px("cacheViewPaddingX"),
            metrics.px("compatStripPaddingY"));
    pRow->setSpacing(metrics.px("viewHeadGap"));
    m_pStripKey = monoLabel(QString(), QStringLiteral("MonoCompatiblesStripKey"), pStrip);
    pRow->addWidget(m_pStripKey);
    m_pStripCounts = monoLabel(QString(), QStringLiteral("MonoCompatiblesStripCounts"), pStrip);
    m_pStripCounts->setMinimumWidth(0);
    m_pStripCounts->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pRow->addWidget(m_pStripCounts, 1);
    //: Shows the Camelot wheel over a short list
    m_pWheelToggle = new QPushButton(tr("Wheel"), pStrip);
    m_pWheelToggle->setObjectName(QStringLiteral("CrateChip"));
    m_pWheelToggle->setCheckable(true);
    m_pWheelToggle->setCursor(Qt::PointingHandCursor);
    m_pWheelToggle->setFocusPolicy(Qt::NoFocus);
    m_pWheelToggle->setToolTip(tr("Show the Camelot wheel and the summary over the list"));
    m_pWheelToggle->setVisible(m_pWheelModel != nullptr);
    connect(m_pWheelToggle, &QPushButton::toggled, this, &CompatiblesView::layoutParts);
    pRow->addWidget(m_pWheelToggle);
    return pStrip;
}

QPushButton* CompatiblesView::followChip(Compatibles::Follow follow) const {
    for (QPushButton* pChip : m_followChips) {
        if (pChip->property("crateValue").toInt() == static_cast<int>(follow)) {
            return pChip;
        }
    }
    return nullptr;
}

QPushButton* CompatiblesView::contextChip(CompatibleContext context) const {
    for (QPushButton* pChip : m_contextChips) {
        if (pChip->property("crateValue").toInt() == static_cast<int>(context)) {
            return pChip;
        }
    }
    return nullptr;
}

QString CompatiblesView::hintFor(Compatibles::State state) {
    switch (state) {
    case Compatibles::State::NoCrateTrack:
        return tr("Load a Crate track to see compatibles");
    case Compatibles::State::Loading:
        return tr("Finding compatible tracks...");
    case Compatibles::State::NoProfile:
        return tr("Crate has not analyzed this track for Smart Mix yet");
    case Compatibles::State::Failed:
        return tr("Cannot reach the Crate server");
    case Compatibles::State::Ready:
        break;
    }
    return QString();
}

QString CompatiblesView::ageText(qint64 ms) {
    const qint64 age = std::max<qint64>(0, ms);
    if (age < 10'000) {
        //: How long ago the compatible tracks came, in seconds with a decimal
        return tr("%1 s ago").arg(QString::number(static_cast<double>(age) / 1000.0, 'f', 1));
    }
    if (age < 60'000) {
        return tr("%1 s ago").arg(age / 1000);
    }
    if (age < 3'600'000) {
        //: How long ago the compatible tracks came, in minutes
        return tr("%1 min ago").arg(age / 60'000);
    }
    //: How long ago the compatible tracks came, in hours
    return tr("%1 h ago").arg(age / 3'600'000);
}

int CompatiblesView::targetDeck(
        int masterIndex, int deckCount, const std::function<bool(int)>& plays) {
    QList<int> order;
    if (masterIndex >= 0 && (masterIndex ^ 1) < deckCount) {
        // Its partner first: deck 1 with 2, 3 with 4
        order.append(masterIndex ^ 1);
    }
    for (int index = 0; index < deckCount; ++index) {
        if (index != masterIndex && !order.contains(index)) {
            order.append(index);
        }
    }
    for (const int index : std::as_const(order)) {
        if (!plays || !plays(index)) {
            return index;
        }
    }
    return -1;
}

QString CompatiblesView::targetGroup() const {
    const int decks = deckCount();
    int master = -1;
    if (m_pCompatibles) {
        for (int index = 0; index < decks; ++index) {
            if (PlayerManager::groupForDeck(index) == m_pCompatibles->masterDeck()) {
                master = index;
            }
        }
    }
    const int target = targetDeck(master, decks, [](int index) {
        return controlValue(ConfigKey(PlayerManager::groupForDeck(index), QStringLiteral("play")),
                       0) > 0;
    });
    return target < 0 ? QString() : PlayerManager::groupForDeck(target);
}

CompatiblesView::BandLayout CompatiblesView::bandLayout(
        int available, int paddingY, int wheelMinimum, int wheelMaximum, int content) {
    BandLayout layout;
    const int needed = std::max(wheelMinimum, content) + paddingY;
    if (wheelMaximum < wheelMinimum || available < needed) {
        layout.compact = true;
        layout.wheel = wheelMinimum;
        layout.height = needed;
        return layout;
    }
    // The wheel grows with the pane up to the mockup's size; what is left
    // goes to the list
    layout.wheel = std::clamp(available - paddingY, wheelMinimum, wheelMaximum);
    layout.height = std::max(layout.wheel, content) + paddingY;
    return layout;
}

QSize CompatiblesView::sizeHint() const {
    // The library's pane is MinimumExpanding: it cannot be narrower than the
    // size hint of its widest view, so this one asks for nothing
    return QSize(0, 0);
}

QSize CompatiblesView::minimumSizeHint() const {
    // Nothing in the view may make the window grow: it lays its parts out
    // in whatever the pane gives it, the list scrolling
    return QSize(0, 0);
}

void CompatiblesView::resizeEvent(QResizeEvent* pEvent) {
    QWidget::resizeEvent(pEvent);
    layoutParts();
}

void CompatiblesView::showEvent(QShowEvent* pEvent) {
    QWidget::showEvent(pEvent);
    m_clock.start();
    updateCount();
    updateSelected();
}

void CompatiblesView::hideEvent(QHideEvent* pEvent) {
    QWidget::hideEvent(pEvent);
    m_clock.stop();
}

void CompatiblesView::layoutParts() {
    const DialogMetrics& metrics = DialogMetrics::installed();
    const delegates::DelegateStyle& style = delegates::DelegateStyle::current();
    const int width = this->width();
    const int height = this->height();

    // The head: what does not fit goes, the least needed first, and then
    // what the view follows gives way
    const int headHeight = metrics.px("viewHeadHeight");
    const QList<QWidget*> optional = {m_pFollowLabel, m_pContextLabel, m_pCount};
    for (QWidget* pPart : optional) {
        pPart->setVisible(true);
    }
    for (QWidget* pPart : optional) {
        m_pHead->layout()->invalidate();
        if (m_pHead->layout()->sizeHint().width() <= width) {
            break;
        }
        pPart->setVisible(false);
    }
    m_pHead->setGeometry(0, 0, width, headHeight);
    int top = headHeight;

    // The band, or the strip where it does not fit
    const int rowHeight = style.isValid() ? style.rowHeight : m_pHead->fontMetrics().height() * 2;
    const int headerHeight = style.isValid() ? style.headerHeight : rowHeight;
    const int hintHeight = m_pHint->isHidden()
            ? 0
            : m_pHint->heightForWidth(width) + 2 * metrics.px("compatHintPaddingY");
    const int list = headerHeight + metrics.px("compatListRows") * rowHeight;
    const int available = height - top - hintHeight - list;
    const int paddingY = metrics.px("compatBandPaddingTop") + metrics.px("compatBandPaddingBottom");
    const int gutter = metrics.px("cacheViewPaddingX");
    const int gap = metrics.px("compatBandGap");
    const int wheelMinimum = m_pWheel ? metrics.px("compatWheelMinimum") : 0;
    const int sideMinimum = metrics.px("compatSideMinWidth");
    const int summaryMinimum = metrics.px("compatSummaryMinWidth");
    const int summaryMaximum = metrics.px("compatSummaryMaxWidth");
    const int columns = m_pWheel ? 2 : 1;
    const int wheelPreferred = m_pWheel ? m_pWheel->sizeHint().width() : 0;
    const int summaryWidth = std::clamp(
            width - 2 * gutter - columns * gap - wheelPreferred - sideMinimum,
            summaryMinimum,
            summaryMaximum);
    const int wheelMaximum = m_pWheel
            ? std::min(wheelPreferred,
                      width - 2 * gutter - columns * gap - summaryWidth - sideMinimum)
            : 0;
    m_pSummary->setFixedWidth(summaryWidth);
    // The columns' height at the wheel's likely size
    const int wheelGuess = std::clamp(available - paddingY,
            wheelMinimum,
            std::max(wheelMinimum, wheelMaximum));
    const int sideWidth = std::max(sideMinimum,
            width - 2 * gutter - columns * gap - summaryWidth - (m_pWheel ? wheelGuess : 0));
    const int content = std::max(m_pSummary->sizeHint().height(),
            m_pSide->hasHeightForWidth() ? m_pSide->heightForWidth(sideWidth)
                                         : m_pSide->sizeHint().height());
    const BandLayout band = bandLayout(available,
            paddingY,
            wheelMinimum,
            m_pWheel ? wheelMaximum : 0,
            content);
    m_compact = band.compact;
    const bool showBand = !m_compact || (m_pWheelToggle->isChecked() && m_pWheel);
    m_pStrip->setVisible(m_compact);
    if (m_compact) {
        const int stripHeight = m_pStrip->sizeHint().height();
        m_pStrip->setGeometry(0, top, width, stripHeight);
        top += stripHeight;
    }
    m_pBand->setVisible(showBand);
    if (showBand) {
        if (m_pWheel) {
            m_pWheel->setFixedSize(band.wheel, band.wheel);
        }
        m_sideWidth = width - 2 * gutter - columns * gap - summaryWidth -
                (m_pWheel ? band.wheel : 0) - metrics.px("compatColumnPaddingLeft");
        fitBadges();
        m_pBand->setGeometry(0, top, width, band.height);
        top += band.height;
    }
    if (!m_pHint->isHidden()) {
        m_pHint->setGeometry(0, top, width, hintHeight);
        top += hintHeight;
    }
    m_pTable->setGeometry(0, top, width, std::max(0, height - top));
}

void CompatiblesView::update() {
    if (!m_pCompatibles) {
        return;
    }
    const Compatibles::State state = m_pCompatibles->state();
    const QString hint = hintFor(state);
    m_pHint->setText(hint);
    // While loading, the last results stay under the hint
    m_pHint->setHidden(hint.isEmpty());
    for (QPushButton* pChip : std::as_const(m_followChips)) {
        pChip->setChecked(pChip->property("crateValue").toInt() ==
                static_cast<int>(m_pCompatibles->follow()));
    }
    for (QPushButton* pChip : std::as_const(m_contextChips)) {
        pChip->setChecked(pChip->property("crateValue").toInt() ==
                static_cast<int>(m_pCompatibles->context()));
    }

    CompatiblesModel* pModel = m_pCompatibles->model();
    const auto masterKey = widgets::CamelotWheelModel::keyOfCamelot(pModel->seedCamelot());
    if (m_pWheelModel) {
        QList<widgets::CamelotWheelModel::ChromaticKey> keys;
        for (const QString& camelot : pModel->keysInResults()) {
            const auto key = widgets::CamelotWheelModel::keyOfCamelot(camelot);
            if (key != mixxx::track::io::key::INVALID) {
                keys.append(key);
            }
        }
        m_pWheelModel->setKeysInResults(keys);
        // The followed track's key, as the catalogue has it
        m_pWheelModel->setMasterKey(masterKey);
        if (pModel->keysInResults().isEmpty() && !pModel->keyFilter().isEmpty()) {
            pModel->setKeyFilter(QString());
        }
    }
    if (m_pWheel) {
        m_pWheel->setSubtitle(widgets::CamelotWheelModel::nameOf(masterKey));
    }
    watchSelection();
    updateHead();
    updateSummary();
    updateCache();
    updateSelected();
    updateCount();
    layoutParts();
}

void CompatiblesView::updateHead() {
    CompatiblesModel* pModel = m_pCompatibles->model();
    const auto& seed = pModel->seed();
    QStringList parts;
    const QString letter = deckLetter(m_pCompatibles->masterDeck());
    if (m_pCompatibles->follow() == Compatibles::Follow::Selection) {
        //: What the Compatibles view follows: the selected track
        parts.append(tr("Selected"));
    } else if (!letter.isEmpty()) {
        //: The deck the Compatibles view follows: "Deck A"
        parts.append(tr("Deck %1").arg(letter));
    }
    if (!seed.title.isEmpty()) {
        parts.append(seed.title);
    } else if (!m_pCompatibles->followedLabel().isEmpty()) {
        parts.append(m_pCompatibles->followedLabel());
    }
    if (!pModel->seedCamelot().isEmpty()) {
        parts.append(pModel->seedCamelot());
    }
    if (seed.bpm) {
        parts.append(bpmText(seed.bpm));
    }
    m_pHeadMeta->setText(parts.join(kSeparator));
    m_pHeadMeta->setToolTip(parts.join(kSeparator));
    m_pDeckBadge->setText(m_pCompatibles->follow() == Compatibles::Follow::Selection
                    ? tr("Selected")
                    : letter.isEmpty() ? tr("Master deck")
                                       : tr("Deck %1").arg(letter));
}

void CompatiblesView::updateSummary() {
    CompatiblesModel* pModel = m_pCompatibles->model();
    const CompatiblesSummary summary = pModel->summary();
    const QString camelot = pModel->seedCamelot();
    const QString name = widgets::CamelotWheelModel::nameOf(
            widgets::CamelotWheelModel::keyOfCamelot(camelot));
    QStringList title;
    if (!camelot.isEmpty()) {
        title.append(camelot);
    }
    if (!name.isEmpty()) {
        title.append(name);
    }
    m_pSummaryTitle->setText(title.isEmpty() ? QStringLiteral("—") : title.join(kSeparator));
    const auto& seed = pModel->seed();
    QStringList track;
    for (const QString& part : {seed.title, seed.artist, bpmText(seed.bpm)}) {
        if (!part.isEmpty()) {
            track.append(part);
        }
    }
    m_pSummaryTrack->setText(track.join(kSeparator));
    m_metrics[0]->setText(QString::number(summary.results));
    m_metrics[1]->setText(QString::number(summary.harmonic));
    m_metrics[2]->setText(QString::number(summary.cached));

    const QString step = camelot.isEmpty()
            ? QString()
            : camelotStep(camelot, -1) + QLatin1Char(' ') + camelotStep(camelot, 1);
    const struct {
        QString name;
        QString keys;
        int count;
    } shares[] = {
            {harmonyLabel(Harmony::Same), camelot, summary.same},
            {QStringLiteral("±1"), step, summary.step},
            {harmonyLabel(Harmony::Relative), camelotStep(camelot, 0, true), summary.relative},
            {harmonyLabel(Harmony::EnergyBoost), camelotStep(camelot, 7), summary.energy},
            //: The compatible tracks in keys that do not mix with the followed one
            {tr("Other keys"), QString(), summary.other},
    };
    QStringList counts;
    for (int index = 0; index < 5; ++index) {
        const auto& share = shares[index];
        m_shareLabels[index]->setText(
                share.keys.isEmpty() ? share.name : share.name + kSeparator + share.keys);
        m_shareCounts[index]->setText(QString::number(share.count));
        // The mockup's meters are at twice the share: half the results
        // fill one
        static_cast<ShareMeter*>(m_shareMeters[index])
                ->setValue(summary.results > 0 ? 2.0 * share.count / summary.results : 0.0);
        counts.append(share.name + QLatin1Char(' ') + QString::number(share.count));
    }
    m_pStripKey->setText(title.join(kSeparator));
    m_pStripCounts->setText(counts.join(kSeparator));
}

void CompatiblesView::updateCache() {
    CompatiblesModel* pModel = m_pCompatibles->model();
    const auto cells = pModel->bestCache(kPrewarmRings);
    int ready = 0;
    for (int index = 0; index < m_prewarmRings.size(); ++index) {
        double value = 0;
        if (index < cells.size()) {
            switch (cells[index].state) {
            case delegates::CacheState::Ready:
                value = 1;
                ++ready;
                break;
            case delegates::CacheState::Downloading:
                value = cells[index].progress;
                break;
            case delegates::CacheState::Failed:
            case delegates::CacheState::None:
                break;
            }
        }
        m_prewarmRings[index]->setValue(value);
        m_prewarmRings[index]->setVisible(index < cells.size() || cells.isEmpty());
    }
    //: How many of the best five compatible tracks are cached: "3 / 5 ready"
    m_pPrewarmLabel->setText(tr("%1 / %2 ready")
                    .arg(ready)
                    .arg(std::max<qsizetype>(cells.size(), kPrewarmRings)));
    m_metrics[2]->setText(QString::number(pModel->summary().cached));
}

void CompatiblesView::watchSelection() {
    auto* pView = qobject_cast<QAbstractItemView*>(m_pTable);
    QItemSelectionModel* pSelection = pView ? pView->selectionModel() : nullptr;
    if (pSelection == m_pSelection) {
        return;
    }
    m_pSelection = pSelection;
    if (pSelection) {
        connect(pSelection,
                &QItemSelectionModel::selectionChanged,
                this,
                &CompatiblesView::updateSelected);
    }
}

int CompatiblesView::selectedRow() const {
    if (!m_pSelection) {
        return -1;
    }
    const QModelIndexList rows = m_pSelection->selectedRows();
    return rows.isEmpty() ? -1 : rows.first().row();
}

void CompatiblesView::updateSelected() {
    if (!m_pCompatibles) {
        return;
    }
    CompatiblesModel* pModel = m_pCompatibles->model();
    const int row = selectedRow();
    const QString group = targetGroup();
    const QString letter = deckLetter(group);
    //: Loads the selected compatible track into deck N
    m_pLoad->setText(letter.isEmpty() ? tr("Load in a deck") : tr("Load in deck %1").arg(letter));
    if (row < 0 || row >= pModel->rowCount()) {
        //: The selected compatible track: none yet
        m_pSelectedEyebrow->setText(tr("Selected"));
        m_pSelectedTitle->setText(tr("Select a track to load it or plan the transition"));
        m_pSelectedArtist->clear();
        for (QLabel* pBadge : std::as_const(m_badges)) {
            pBadge->setProperty("crateWanted", false);
            pBadge->hide();
        }
        m_pLoad->setEnabled(false);
        m_pPlan->setEnabled(false);
        return;
    }
    const auto cell = [pModel, row](int column, int role = Qt::DisplayRole) {
        return pModel->data(pModel->index(row, column), role);
    };
    //: The selected compatible track and how many there are: "Selected · 1 of 50"
    m_pSelectedEyebrow->setText(tr("Selected · %1 of %2").arg(row + 1).arg(pModel->rowCount()));
    m_pSelectedTitle->setText(cell(CompatiblesModel::kTitleColumn).toString());
    const QString artist = cell(CompatiblesModel::kArtistColumn).toString();
    m_pSelectedArtist->setText(artist.isEmpty() ? QString() : kSeparator + artist);

    // Harmony, Δ BPM, Δ energy and the cache, as the mockup's badges
    const QString key = cell(CompatiblesModel::kKeyColumn).toString();
    const QString harmony = cell(CompatiblesModel::kHarmonicColumn).toString();
    const bool harmonic =
            cell(CompatiblesModel::kHarmonicColumn, delegates::HarmonyMatchRole).toBool();
    QStringList harmonyParts;
    if (harmonic) {
        harmonyParts.append(harmony);
    }
    if (!key.isEmpty()) {
        harmonyParts.append(key);
    }
    m_badges[0]->setText(harmonyParts.join(kSeparator));
    setTone(m_badges[0], harmonic ? QStringLiteral("accent") : QString());
    const QString bpmDelta = cell(CompatiblesModel::kBpmDeltaColumn).toString();
    //: The tempo of the selected track minus the followed one's
    m_badges[1]->setText(tr("Δ BPM %1").arg(bpmDelta));
    setTone(m_badges[1],
            cell(CompatiblesModel::kBpmDeltaColumn, delegates::WarningRole).toBool()
                    ? QStringLiteral("warning")
                    : QString());
    const QString energyDelta = cell(CompatiblesModel::kEnergyDeltaColumn).toString();
    //: The energy of the selected track minus the followed one's
    m_badges[2]->setText(tr("Δ energy %1").arg(energyDelta));
    const auto cache = static_cast<delegates::CacheState>(
            cell(CompatiblesModel::kCacheColumn, delegates::CacheStateRole).toInt());
    QString cacheText;
    QString cacheTone;
    switch (cache) {
    case delegates::CacheState::Ready:
        //: The selected compatible track is in the cache
        cacheText = tr("Cached");
        break;
    case delegates::CacheState::Downloading:
        //: The selected compatible track is downloading: "Downloading 62 %"
        cacheText = tr("Downloading %1 %")
                            .arg(std::lround(100 *
                                    cell(CompatiblesModel::kCacheColumn,
                                            delegates::CacheProgressRole)
                                            .toDouble()));
        break;
    case delegates::CacheState::Failed:
        cacheText = tr("Download failed");
        cacheTone = QStringLiteral("warning");
        break;
    case delegates::CacheState::None:
        //: The selected compatible track is not in the cache yet
        cacheText = tr("Not cached");
        cacheTone = QStringLiteral("off");
        break;
    }
    m_badges[3]->setText(cacheText);
    setTone(m_badges[3], cacheTone);
    for (QLabel* pBadge : std::as_const(m_badges)) {
        pBadge->setProperty("crateWanted", !pBadge->text().isEmpty());
    }
    m_badges[1]->setProperty("crateWanted", !bpmDelta.isEmpty());
    m_badges[2]->setProperty("crateWanted", !energyDelta.isEmpty());
    fitBadges();
    m_pLoad->setEnabled(!group.isEmpty());
    m_pPlan->setEnabled(!group.isEmpty());
}

void CompatiblesView::fitBadges() {
    // The badges keep their order and drop from the end where the side
    // column is short of room for them
    const int gap = DialogMetrics::installed().px("chipGap");
    int used = 0;
    bool full = false;
    for (QLabel* pBadge : std::as_const(m_badges)) {
        if (!pBadge->property("crateWanted").toBool()) {
            pBadge->hide();
            continue;
        }
        const int needed = (used > 0 ? gap : 0) + pBadge->sizeHint().width();
        if (full || (m_sideWidth > 0 && used + needed > m_sideWidth)) {
            full = true;
            pBadge->hide();
            continue;
        }
        used += needed;
        pBadge->show();
    }
}

void CompatiblesView::updateCount() {
    if (!m_pCompatibles) {
        return;
    }
    CompatiblesModel* pModel = m_pCompatibles->model();
    const int results = static_cast<int>(pModel->results().size());
    QStringList parts;
    parts.append(results == 1
                    //: How many compatible tracks the view shows: one
                    ? tr("1 track")
                    //: How many compatible tracks the view shows: "50 tracks"
                    : tr("%1 tracks").arg(results));
    if (pModel->resultsTimeMs() > 0) {
        parts.append(ageText(QDateTime::currentMSecsSinceEpoch() - pModel->resultsTimeMs()));
    }
    m_pCount->setText(parts.join(kSeparator));
}

void CompatiblesView::loadSelected(const QString& group) {
    const int row = selectedRow();
    if (group.isEmpty() || row < 0 || !m_pCompatibles) {
        return;
    }
    CompatiblesModel* pModel = m_pCompatibles->model();
    const QModelIndex index = pModel->index(row, 0);
    // Nothing to do if the deck has the track already
    const TrackPointer pLoaded = PlayerInfo::instance().getTrackInfo(group);
    if (pLoaded && pLoaded->getId().isValid() && pLoaded->getId() == pModel->getTrackId(index)) {
        return;
    }
    if (auto* pTrackTable = qobject_cast<WTrackTableView*>(m_pTable)) {
        pTrackTable->loadSelectedTrackToGroup(group,
#ifdef __STEM__
                mixxx::StemChannelSelection(),
#endif
                false);
        return;
    }
    // Without upstream's table (tests): the model's own load
    pModel->requestTrackLoad(index, group, false);
}

void CompatiblesView::onKeyClicked(mixxx::track::io::key::ChromaticKey key) {
    if (!m_pCompatibles) {
        return;
    }
    CompatiblesModel* pModel = m_pCompatibles->model();
    const QString camelot = widgets::CamelotWheelModel::camelotOf(key);
    // A second click on the same key shows every key again
    pModel->setKeyFilter(pModel->keyFilter() == camelot ? QString() : camelot);
}

void CompatiblesView::onShow() {
}

bool CompatiblesView::hasFocus() const {
    return m_pTable->hasFocus();
}

void CompatiblesView::setFocus() {
    m_pTable->setFocus();
}

void CompatiblesView::saveCurrentViewState() {
    if (auto* pView = dynamic_cast<LibraryView*>(m_pTable)) {
        pView->saveCurrentViewState();
    }
}

bool CompatiblesView::restoreCurrentViewState() {
    auto* pView = dynamic_cast<LibraryView*>(m_pTable);
    return pView && pView->restoreCurrentViewState();
}

} // namespace crate::recs
