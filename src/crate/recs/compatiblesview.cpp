#include "crate/recs/compatiblesview.h"

#include <QComboBox>
#include <QDateTime>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QThread>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

#include "crate/client/crateclient.h"
#include "crate/library/crateloadcoordinator.h"
#include "crate/recs/compatiblesmodel.h"
#include "crate/ui/widgets/camelotwheel.h"
#include "moc_compatiblesview.cpp"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

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

CompatiblesView::CompatiblesView(Compatibles* pCompatibles,
        QWidget* pTable,
        widgets::CamelotWheelModel* pWheelModel,
        QWidget* pParent)
        : QWidget(pParent),
          m_pCompatibles(pCompatibles),
          m_pTable(pTable),
          m_pWheelModel(pWheelModel),
          m_pTitle(new QLabel(this)),
          m_pHint(new QLabel(this)),
          m_pFollow(new QComboBox(this)),
          m_pContext(new QComboBox(this)) {
    setObjectName(QStringLiteral("CrateCompatibles"));
    m_pTitle->setObjectName(QStringLiteral("CrateCompatiblesTitle"));
    m_pHint->setObjectName(QStringLiteral("CrateCompatiblesHint"));
    m_pHint->setWordWrap(true);
    m_pHint->setAlignment(Qt::AlignCenter);

    //: What the Compatibles view follows: the track of the deck on air
    m_pFollow->addItem(tr("Master deck"), static_cast<int>(Compatibles::Follow::MasterDeck));
    //: What the Compatibles view follows: the track selected in the library
    m_pFollow->addItem(tr("Selection"), static_cast<int>(Compatibles::Follow::Selection));
    m_pFollow->setToolTip(tr("Find compatible tracks for the deck on air or for the selection"));
    //: How the DJ means to mix the next track: Smart Mix decides
    m_pContext->addItem(tr("Auto"), static_cast<int>(CompatibleContext::Auto));
    //: How the DJ means to mix the next track: a beatmatched blend
    m_pContext->addItem(tr("Beatmatch"), static_cast<int>(CompatibleContext::Beatmatch));
    //: How the DJ means to mix the next track: a smooth fade
    m_pContext->addItem(tr("Smooth"), static_cast<int>(CompatibleContext::Smooth));
    for (QComboBox* pChoice : {m_pFollow, m_pContext}) {
        pChoice->setFocusPolicy(Qt::ClickFocus);
    }

    auto* pBar = new QHBoxLayout();
    pBar->setContentsMargins(0, 0, 0, 0);
    pBar->addWidget(m_pTitle, 1);
    pBar->addWidget(m_pFollow);
    pBar->addWidget(m_pContext);

    auto* pList = new QVBoxLayout();
    pList->setContentsMargins(0, 0, 0, 0);
    pList->addLayout(pBar);
    pList->addWidget(m_pHint);
    m_pTable->setParent(this);
    pList->addWidget(m_pTable, 1);

    auto* pLayout = new QHBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    if (m_pWheelModel) {
        m_pWheel = new widgets::CamelotWheel(
                m_pWheelModel, widgets::InstrumentStyle::installed(), this);
        m_pWheel->setFixedWidth(m_pWheel->sizeHint().width());
        connect(m_pWheel,
                &widgets::CamelotWheel::keyClicked,
                this,
                &CompatiblesView::onKeyClicked);
        pLayout->addWidget(m_pWheel, 0, Qt::AlignTop);
    }
    pLayout->addLayout(pList, 1);

    if (m_pCompatibles) {
        connect(m_pFollow, &QComboBox::activated, this, [this](int index) {
            if (m_pCompatibles) {
                m_pCompatibles->setFollow(
                        static_cast<Compatibles::Follow>(m_pFollow->itemData(index).toInt()));
            }
        });
        connect(m_pContext, &QComboBox::activated, this, [this](int index) {
            if (m_pCompatibles) {
                m_pCompatibles->setContext(
                        static_cast<CompatibleContext>(m_pContext->itemData(index).toInt()));
            }
        });
        connect(m_pCompatibles, &Compatibles::stateChanged, this, &CompatiblesView::update);
        connect(m_pCompatibles->model(), &QAbstractItemModel::modelReset, this, [this] {
            update();
        });
    }
    update();
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

void CompatiblesView::update() {
    if (!m_pCompatibles) {
        return;
    }
    const Compatibles::State state = m_pCompatibles->state();
    const QString hint = hintFor(state);
    m_pHint->setText(hint);
    // While loading, the last results stay under the hint
    m_pHint->setHidden(hint.isEmpty());
    const QString label = m_pCompatibles->followedLabel();
    m_pTitle->setText(label.isEmpty() ? tr("Compatibles")
                                      //: The Compatibles view's title: compatible with a track
                                      : tr("Compatible with %1").arg(label));
    m_pFollow->setCurrentIndex(m_pFollow->findData(static_cast<int>(m_pCompatibles->follow())));
    m_pContext->setCurrentIndex(
            m_pContext->findData(static_cast<int>(m_pCompatibles->context())));

    if (m_pWheelModel) {
        CompatiblesModel* pModel = m_pCompatibles->model();
        QList<widgets::CamelotWheelModel::ChromaticKey> keys;
        for (const QString& camelot : pModel->keysInResults()) {
            for (const auto& node : m_pWheelModel->nodes()) {
                if (node.camelot == camelot) {
                    keys.append(node.key);
                }
            }
        }
        m_pWheelModel->setKeysInResults(keys);
        // The followed track's key, as the catalogue has it
        auto masterKey = mixxx::track::io::key::INVALID;
        for (const auto& node : m_pWheelModel->nodes()) {
            if (node.camelot == pModel->seedCamelot()) {
                masterKey = node.key;
            }
        }
        m_pWheelModel->setMasterKey(masterKey);
        if (pModel->keysInResults().isEmpty() && !pModel->keyFilter().isEmpty()) {
            pModel->setKeyFilter(QString());
        }
    }
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
