#include "crate/smartmix/assistpanel.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>
#include <cmath>

#include "control/controlobject.h"
#include "crate/smartmix/smartmixassist.h"
#include "crate/ui/dialogs/dialogstyle.h"
#include "crate/ui/widgets/levelring.h"
#include "moc_assistpanel.cpp"
#include "track/track.h"

namespace crate {

namespace {

const ConfigKey kAutoDjEnabled(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"));
// How often the times to the entry change while the panel shows
constexpr int kClockMs = 100;
// The ` · ` between the parts of a MonoLabel (MonoLabel.tsx)
const QString kSeparator = QStringLiteral(" · ");
const QString kMinus = QStringLiteral("−");

QLabel* textLabel(const char* objectName, QWidget* pParent) {
    auto* pLabel = new QLabel(pParent);
    pLabel->setObjectName(QLatin1String(objectName));
    pLabel->setTextFormat(Qt::PlainText);
    return pLabel;
}

void setState(QWidget* pWidget, const QString& state) {
    if (pWidget->property("crateState").toString() == state) {
        return;
    }
    pWidget->setProperty("crateState", state);
    pWidget->style()->unpolish(pWidget);
    pWidget->style()->polish(pWidget);
}

/// "A" for [Channel1], "B" for [Channel2]…
QString deckLetter(const QString& group) {
    const QString number = group.mid(8, group.size() - 9);
    bool ok = false;
    const int index = number.toInt(&ok);
    return ok && index > 0 ? QString(QChar(static_cast<char16_t>(u'A' + index - 1))) : group;
}

/// 1:02, or 0:07.7 with `tenths`; negative with a minus sign.
QString clockText(double ms, bool tenths) {
    const bool negative = ms < 0.0;
    const qint64 units = tenths ? std::llround(std::abs(ms) / 100.0)
                                : std::llround(std::abs(ms) / 1000.0);
    const qint64 seconds = tenths ? units / 10 : units;
    QString text = QStringLiteral("%1:%2")
                           .arg(seconds / 60)
                           .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    if (tenths) {
        text += QStringLiteral(".%1").arg(units % 10);
    }
    return negative ? QString(kMinus + text) : text;
}

QString signedNumber(double value, int decimals) {
    const QString text = QString::number(std::abs(value), 'f', decimals);
    return QString(value < 0.0 ? kMinus : QStringLiteral("+")) + text;
}

double deckBpm(const QString& group) {
    const ControlObject* pControl = ControlObject::getControl(
            ConfigKey(group, QStringLiteral("bpm")), ControlFlag::NoWarnIfMissing);
    return pControl ? pControl->get() : 0.0;
}

QString titleOf(const TrackPointer& pTrack) {
    if (!pTrack) {
        return {};
    }
    const QString title = pTrack->getTitle();
    return title.isEmpty() ? pTrack->getFileInfo().fileName() : title;
}

} // namespace

AssistPanel::AssistPanel(QWidget* pParent)
        : QWidget(pParent) {
    setObjectName(QStringLiteral("CrateSmartMixAssist"));
    setAttribute(Qt::WA_StyledBackground);
    const DialogMetrics& metrics = DialogMetrics::installed();

    auto* pBody = new QWidget(this);
    pBody->setObjectName(QStringLiteral("CrateAssistBody"));
    auto* pColumns = new QHBoxLayout(pBody);
    pColumns->setContentsMargins(metrics.px("cacheViewPaddingX"),
            metrics.px("assistPaddingTop"),
            metrics.px("cacheViewPaddingX"),
            0);
    pColumns->setSpacing(metrics.px("assistColumnGap"));
    // The host, and under it the plan's stage (TransitionPlanView, V3.PR1b)
    auto* pPlan = new QVBoxLayout();
    pPlan->setContentsMargins(0, 0, 0, 0);
    pPlan->setSpacing(0);
    pPlan->addWidget(makeHost());
    pPlan->addStretch(1);
    pColumns->addLayout(pPlan, 1);
    pColumns->addWidget(makeInspector());

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);
    pLayout->addWidget(makeHead());
    pLayout->addWidget(pBody, 1);

    m_clock.setInterval(kClockMs);
    connect(&m_clock, &QTimer::timeout, this, &AssistPanel::showTimes);
    showAssist();
}

AssistPanel::~AssistPanel() = default;

QWidget* AssistPanel::makeHead() {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pHead = new QWidget(this);
    pHead->setObjectName(QStringLiteral("CrateAssistHead"));
    pHead->setAttribute(Qt::WA_StyledBackground);
    pHead->setFixedHeight(metrics.px("viewHeadHeight"));
    auto* pRow = new QHBoxLayout(pHead);
    pRow->setContentsMargins(
            metrics.px("cacheViewPaddingX"), 0, metrics.px("cacheViewPaddingX"), 0);
    pRow->setSpacing(metrics.px("viewHeadGap"));

    auto* pIcon = new QLabel(pHead);
    pIcon->setObjectName(QStringLiteral("CrateAssistHeadIcon"));
    const int iconSize = metrics.px("chipHeight") / 2;
    pIcon->setPixmap(QIcon(designPath() + QStringLiteral("icons/magic-wand_active.svg"))
                    .pixmap(iconSize, iconSize));
    pRow->addWidget(pIcon);
    pRow->addWidget(monoLabel(tr("Smart Mix"), QStringLiteral("MonoAssistEyebrow"), pHead));
    m_pDecks = monoLabel(QString(), QStringLiteral("MonoAssistDecks"), pHead);
    pRow->addWidget(m_pDecks);
    pRow->addStretch(1);

    // The mode that drives the decks (decks.css .seg)
    auto* pMode = new QHBoxLayout();
    pMode->setSpacing(metrics.px("chipGap"));
    pMode->addWidget(monoLabel(tr("Mode"), QStringLiteral("MonoAssistModeLabel"), pHead));
    auto* pGroup = new QButtonGroup(this);
    m_pAssistChip = new QPushButton(tr("Assist"), pHead);
    m_pAutoDjChip = new QPushButton(tr("Auto DJ"), pHead);
    for (QPushButton* pChip : {m_pAssistChip, m_pAutoDjChip}) {
        pChip->setObjectName(QStringLiteral("CrateChip"));
        pChip->setCheckable(true);
        pChip->setCursor(Qt::PointingHandCursor);
        pChip->setFocusPolicy(Qt::NoFocus);
        pGroup->addButton(pChip);
        pMode->addWidget(pChip);
    }
    connect(m_pAssistChip, &QPushButton::clicked, this, [this] {
        setAutoDj(false);
    });
    connect(m_pAutoDjChip, &QPushButton::clicked, this, [this] {
        setAutoDj(true);
    });
    pRow->addLayout(pMode);

    // The plan's status (StatusDot.tsx)
    auto* pStatus = new QHBoxLayout();
    pStatus->setSpacing(metrics.px("chipGap"));
    m_pStatusDot = textLabel("CrateAssistStatusDot", pHead);
    m_pStatus = monoLabel(QString(), QStringLiteral("MonoAssistStatus"), pHead);
    pStatus->addWidget(m_pStatusDot, 0, Qt::AlignVCenter);
    pStatus->addWidget(m_pStatus);
    pRow->addLayout(pStatus);
    return pHead;
}

QWidget* AssistPanel::makeHost() {
    const DialogMetrics& metrics = DialogMetrics::installed();
    m_pHost = new QWidget(this);
    m_pHost->setObjectName(QStringLiteral("CrateAssistHost"));
    m_pHost->setAttribute(Qt::WA_StyledBackground);
    auto* pRow = new QHBoxLayout(m_pHost);
    pRow->setContentsMargins(0, 0, 0, metrics.px("assistHostPaddingBottom"));
    pRow->setSpacing(metrics.px("assistHostGap"));

    auto* pIdentity = new QVBoxLayout();
    pIdentity->setSpacing(0);
    m_pHostMeta = monoLabel(QString(), QStringLiteral("MonoAssistHostMeta"), m_pHost);
    pIdentity->addWidget(m_pHostMeta);
    pIdentity->addSpacing(metrics.px("assistHostNameTop"));
    m_pTitle = textLabel("CrateAssistTitle", m_pHost);
    m_pTitle->setWordWrap(true);
    setDisplayAxes(m_pTitle);
    pIdentity->addWidget(m_pTitle);
    pIdentity->addSpacing(metrics.px("assistHostNameBottom"));
    m_pArtists = textLabel("CrateAssistArtists", m_pHost);
    m_pArtists->setWordWrap(true);
    pIdentity->addWidget(m_pArtists);
    pRow->addLayout(pIdentity, 1);

    // Readings, bottom-aligned (stack-map.css .stack-map__host-reading)
    const auto reading = [&](QLabel** ppValue, QLabel** ppUnit) {
        auto* pReading = new QVBoxLayout();
        pReading->setSpacing(metrics.px("assistHostReadingGap"));
        pReading->addStretch(1);
        *ppValue = textLabel("CrateAssistHostReading", m_pHost);
        *ppUnit = textLabel("CrateAssistHostUnit", m_pHost);
        pReading->addWidget(*ppValue);
        pReading->addWidget(*ppUnit);
        pRow->addLayout(pReading);
    };
    reading(&m_pLength, &m_pLengthUnit);
    QLabel* pToEntryUnit = nullptr;
    reading(&m_pToEntry, &pToEntryUnit);
    pToEntryUnit->setText(tr("to entry"));
    return m_pHost;
}

QWidget* AssistPanel::makeInspector() {
    const DialogMetrics& metrics = DialogMetrics::installed();
    // The column with its rule; the plan scrolls in it when the library pane
    // is short, and the actions stay at its foot
    auto* pColumn = new QWidget(this);
    pColumn->setObjectName(QStringLiteral("CrateAssistInspector"));
    pColumn->setAttribute(Qt::WA_StyledBackground);
    pColumn->setFixedWidth(metrics.px("assistInspectorWidth"));
    auto* pColumnLayout = new QVBoxLayout(pColumn);
    pColumnLayout->setContentsMargins(metrics.px("assistInspectorPaddingLeft"), 0, 0, 0);
    pColumnLayout->setSpacing(0);
    auto* pScroll = new QScrollArea(pColumn);
    pScroll->setObjectName(QStringLiteral("CrateAssistScroll"));
    pScroll->setFrameShape(QFrame::NoFrame);
    pScroll->setWidgetResizable(true);
    pScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pColumnLayout->addWidget(pScroll, 1);

    auto* pInspector = new QWidget();
    pInspector->setObjectName(QStringLiteral("CrateAssistPlan"));
    auto* pLayout = new QVBoxLayout(pInspector);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    pLayout->addWidget(
            monoLabel(tr("Suggested plan"), QStringLiteral("MonoAssistPlanEyebrow"), pInspector));
    pLayout->addSpacing(metrics.px("assistModeTop"));
    m_pMode = textLabel("CrateAssistMode", pInspector);
    m_pMode->setWordWrap(true);
    setDisplayAxes(m_pMode);
    pLayout->addWidget(m_pMode);
    pLayout->addSpacing(metrics.px("assistModeBottom"));
    m_pDescription = textLabel("CrateAssistDescription", pInspector);
    m_pDescription->setWordWrap(true);
    pLayout->addWidget(m_pDescription);
    pLayout->addSpacing(metrics.px("assistDescriptionBottom"));

    m_pStateRow = new QWidget(pInspector);
    auto* pState = new QHBoxLayout(m_pStateRow);
    pState->setContentsMargins(0, 0, 0, metrics.px("assistDescriptionBottom"));
    pState->setSpacing(metrics.px("assistStateGap"));
    m_pState = textLabel("CrateAssistState", m_pStateRow);
    m_pStateCaption = textLabel("CrateAssistStateCaption", m_pStateRow);
    m_pStateCaption->setWordWrap(true);
    pState->addWidget(m_pState);
    pState->addWidget(m_pStateCaption, 1);
    pLayout->addWidget(m_pStateRow);

    // Confidence, entry and length (decks.css .reading-row)
    m_pReadings = new QWidget(pInspector);
    m_pReadings->setObjectName(QStringLiteral("CrateAssistReadings"));
    m_pReadings->setAttribute(Qt::WA_StyledBackground);
    auto* pReadings = new QGridLayout(m_pReadings);
    pReadings->setContentsMargins(0,
            metrics.px("assistReadingRowPaddingY"),
            0,
            metrics.px("assistReadingRowPaddingY"));
    pReadings->setHorizontalSpacing(metrics.px("assistReadingRowGap"));
    pReadings->setVerticalSpacing(metrics.px("assistReadingValueTop"));
    const QString readings[] = {tr("Confidence"), tr("Entry in"), tr("Length")};
    for (int column = 0; column < 3; ++column) {
        pReadings->setColumnStretch(column, 1);
        pReadings->addWidget(monoLabel(readings[column],
                                     QStringLiteral("MonoAssistReadingLabel"),
                                     m_pReadings),
                0,
                column);
        QLabel* pValue = textLabel("CrateAssistReadingValue", m_pReadings);
        m_readings.insert(readings[column], pValue);
        if (column == 0) {
            auto* pConfidence = new QHBoxLayout();
            pConfidence->setSpacing(metrics.px("buttonGapSm"));
            pConfidence->addWidget(pValue);
            m_pConfidenceRing = new widgets::LevelRing(
                    widgets::InstrumentStyle::installed(), m_pReadings);
            pConfidence->addWidget(m_pConfidenceRing, 0, Qt::AlignVCenter);
            pConfidence->addStretch(1);
            pReadings->addLayout(pConfidence, 1, column);
        } else {
            pReadings->addWidget(pValue, 1, column);
        }
    }
    pLayout->addWidget(m_pReadings);

    // The plan, row by row (stack-map.css .stack-map__metadata)
    m_pMeta = new QWidget(pInspector);
    m_pMeta->setObjectName(QStringLiteral("CrateAssistMeta"));
    m_pMeta->setAttribute(Qt::WA_StyledBackground);
    auto* pMeta = new QGridLayout(m_pMeta);
    pMeta->setContentsMargins(
            0, metrics.px("assistMetaPaddingY"), 0, metrics.px("assistMetaPaddingY"));
    pMeta->setHorizontalSpacing(metrics.px("assistMetaGap"));
    pMeta->setVerticalSpacing(2 * metrics.px("assistMetaRowPaddingY"));
    pMeta->setColumnMinimumWidth(0, metrics.px("assistMetaKeyWidth"));
    pMeta->setColumnStretch(1, 1);
    const QString keys[] = {
            tr("Mode"), tr("Entry"), tr("Tempo"), tr("Bass swap"), tr("Gain"), tr("Planner")};
    int row = 0;
    for (const QString& key : keys) {
        QLabel* pKey = textLabel("CrateAssistMetaKey", m_pMeta);
        pKey->setText(key.toUpper());
        pKey->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        QLabel* pValue = textLabel("CrateAssistMetaValue", m_pMeta);
        pValue->setWordWrap(true);
        pMeta->addWidget(pKey, row, 0);
        pMeta->addWidget(pValue, row, 1);
        m_metadata.insert(key, pValue);
        m_metadataRows.insert(key, pKey);
        ++row;
    }
    pLayout->addWidget(m_pMeta);
    pLayout->addStretch(1);
    pScroll->setWidget(pInspector);

    // Prepare, Execute and Cancel (decks.css .assist-actions)
    auto* pActions = new QWidget(pColumn);
    pActions->setObjectName(QStringLiteral("CrateAssistActions"));
    pActions->setAttribute(Qt::WA_StyledBackground);
    auto* pButtons = new QHBoxLayout(pActions);
    pButtons->setContentsMargins(0,
            metrics.px("assistActionsPaddingY"),
            0,
            metrics.px("assistActionsPaddingY"));
    pButtons->setSpacing(metrics.px("assistActionsGap"));
    m_pPrepare = new QPushButton(tr("Prepare"), pActions);
    m_pExecute = new QPushButton(tr("Execute"), pActions);
    m_pCancel = new QPushButton(tr("Cancel"), pActions);
    QIcon play;
    const QString icons = designPath() + QStringLiteral("icons/");
    play.addFile(icons + QStringLiteral("play-bold_primary.svg"), QSize(), QIcon::Normal);
    play.addFile(icons + QStringLiteral("play-bold_hover.svg"), QSize(), QIcon::Active);
    play.addFile(icons + QStringLiteral("play-bold_disabled.svg"), QSize(), QIcon::Disabled);
    m_pExecute->setIcon(play);
    setButtonVariant(m_pPrepare, "neutral");
    setButtonVariant(m_pExecute, "primary");
    setButtonVariant(m_pCancel, "ghost");
    pButtons->addWidget(m_pPrepare);
    pButtons->addWidget(m_pExecute, 1);
    pButtons->addWidget(m_pCancel);
    connect(m_pPrepare, &QPushButton::clicked, this, [this] {
        if (m_pAssist) {
            m_pAssist->prepare();
        }
    });
    connect(m_pExecute, &QPushButton::clicked, this, [this] {
        if (m_pAssist) {
            m_pAssist->execute();
        }
    });
    connect(m_pCancel, &QPushButton::clicked, this, [this] {
        if (m_pAssist) {
            m_pAssist->cancel();
        }
    });
    pColumnLayout->addWidget(pActions);

    QLabel* pQuiet = textLabel("CrateAssistQuiet", pColumn);
    pQuiet->setWordWrap(true);
    //: The controls a controller mapping can use; not translated
    pQuiet->setText(tr("Mappable: %1")
                    .arg(QStringLiteral(
                            "[CrateSmartMix],prepare · execute · cancel")));
    pColumnLayout->addWidget(pQuiet);
    pColumnLayout->addSpacing(metrics.px("assistQuietTop"));
    return pColumn;
}

void AssistPanel::setAssist(SmartMixAssist* pAssist) {
    if (m_pAssist == pAssist) {
        return;
    }
    if (m_pAssist) {
        m_pAssist->disconnect(this);
    }
    m_pAssist = pAssist;
    if (m_pAssist) {
        connect(m_pAssist, &SmartMixAssist::changed, this, &AssistPanel::showAssist);
        connect(m_pAssist, &QObject::destroyed, this, &AssistPanel::showAssist);
    }
    showAssist();
}

void AssistPanel::setAutoDj(bool on) {
    ControlObject* pControl = ControlObject::getControl(
            kAutoDjEnabled, ControlFlag::NoWarnIfMissing);
    if (pControl && (pControl->get() > 0.0) != on) {
        // Auto DJ may refuse, with an empty queue
        pControl->set(on ? 1.0 : 0.0);
    }
    showAssist();
}

void AssistPanel::showAssist() {
    using Status = SmartMixAssist::Status;
    SmartMixAssist* const pAssist = m_pAssist;
    const Status status = pAssist ? pAssist->status() : Status::NoServer;
    const ControlObject* pAutoDj =
            ControlObject::getControl(kAutoDjEnabled, ControlFlag::NoWarnIfMissing);
    const bool autoDj = pAutoDj && pAutoDj->get() > 0.0;
    m_pAssistChip->setChecked(!autoDj);
    m_pAutoDjChip->setChecked(autoDj);

    const QString outgoing = pAssist ? pAssist->outgoingGroup() : QString();
    const QString incoming = pAssist ? pAssist->incomingGroup() : QString();
    QStringList decks{tr("Assistant")};
    if (!outgoing.isEmpty() && !incoming.isEmpty()) {
        decks.append(tr("Master deck %1 → deck %2")
                        .arg(deckLetter(outgoing), deckLetter(incoming)));
    }
    m_pDecks->setText(decks.join(kSeparator));

    QString statusText;
    QString statusState = QStringLiteral("off");
    switch (status) {
    case Status::NoServer:
        statusText = tr("No Crate server");
        break;
    case Status::NoTransition:
        statusText = tr("Waiting for two decks");
        break;
    case Status::NotCrate:
        statusText = tr("Not Crate tracks");
        break;
    case Status::Requesting:
        statusText = tr("Asking Crate");
        statusState = QStringLiteral("busy");
        break;
    case Status::NoPlan:
        statusText = tr("No plan");
        statusState = QStringLiteral("warning");
        break;
    case Status::Ready:
        statusText = tr("Plan ready") + kSeparator +
                tr("%1 s").arg(QString::number(
                        static_cast<double>(pAssist->planLatencyMs()) / 1000.0, 'f', 1));
        statusState = QStringLiteral("ready");
        break;
    case Status::Prepared:
        statusText = tr("Prepared");
        statusState = QStringLiteral("ready");
        break;
    case Status::Executing:
        statusText = tr("Waiting for the entry");
        statusState = QStringLiteral("ready");
        break;
    case Status::Running:
        statusText = tr("Mixing");
        statusState = QStringLiteral("ready");
        break;
    case Status::AutoDj:
        statusText = tr("Auto DJ is mixing");
        statusState = QStringLiteral("busy");
        break;
    }
    m_pStatus->setText(statusText);
    setState(m_pStatus, statusState);
    setState(m_pStatusDot, statusState);

    const TrackPointer pOutgoing = pAssist ? pAssist->outgoingTrack() : TrackPointer();
    const TrackPointer pIncoming = pAssist ? pAssist->incomingTrack() : TrackPointer();
    const bool hasEdge = pOutgoing && pIncoming && !incoming.isEmpty();
    m_pHost->setVisible(hasEdge);
    if (hasEdge) {
        QStringList meta;
        for (const auto& [group, pTrack] :
                {std::pair{outgoing, pOutgoing}, std::pair{incoming, pIncoming}}) {
            QStringList deck{deckLetter(group)};
            if (!pTrack->getKeyText().isEmpty()) {
                deck.append(pTrack->getKeyText());
            }
            if (deckBpm(group) > 0.0) {
                deck.append(QString::number(deckBpm(group), 'f', 2));
            }
            meta.append(deck.join(kSeparator));
        }
        m_pHostMeta->setText(meta.join(QStringLiteral("   ")));
        m_pTitle->setText(QStringLiteral("%1 → %2").arg(titleOf(pOutgoing), titleOf(pIncoming)));
        QStringList artists;
        for (const TrackPointer& pTrack : {pOutgoing, pIncoming}) {
            if (!pTrack->getArtist().isEmpty()) {
                artists.append(pTrack->getArtist());
            }
        }
        artists.append(tr("planned by Crate for decks"));
        m_pArtists->setText(artists.join(kSeparator));
    }

    const std::optional<TransitionPlan> plan =
            pAssist ? pAssist->plan() : std::optional<TransitionPlan>();
    const bool showsPlan = plan.has_value();
    m_pReadings->setVisible(showsPlan);
    m_pMeta->setVisible(showsPlan);
    m_pLength->setVisible(showsPlan);
    m_pLengthUnit->setVisible(showsPlan);
    QString mode;
    QString description;
    if (showsPlan) {
        const QString in = deckLetter(incoming);
        const QString out = deckLetter(outgoing);
        const double outgoingMs = SmartMixDirector::startAtMs(*plan,
                ControlObject::get(ConfigKey(outgoing, QStringLiteral("duration"))) * 1000.0);
        const QString length = clockText(static_cast<double>(plan->durationMs), false);
        const QString entry = clockText(static_cast<double>(plan->startIncomingCueMs()), true);
        const QString cue = clockText(outgoingMs, true);
        switch (plan->mode) {
        case TransitionPlan::Mode::Beatmatch:
            mode = tr("Beatmatch");
            description = (plan->curve == TransitionPlan::Curve::DjBlend
                                          ? tr("Long blend over %1.")
                                          : tr("Beatmatch over %1."))
                                  .arg(length) +
                    QLatin1Char(' ') +
                    tr("Deck %1 enters at %2 of its track as deck %3 reaches %4.")
                            .arg(in, entry, out, cue);
            break;
        case TransitionPlan::Mode::Adaptive:
            mode = tr("Crossfade");
            description = tr("Crossfade over %1 without a tempo change.").arg(length) +
                    QLatin1Char(' ') +
                    tr("Deck %1 enters at %2 of its track as deck %3 reaches %4.")
                            .arg(in, entry, out, cue);
            break;
        case TransitionPlan::Mode::Gapless:
            mode = tr("Gapless");
            description = tr(
                    "Deck %1 starts at %2 of its track as deck %3 ends, without "
                    "overlap.")
                                  .arg(in, entry, out);
            break;
        }
        if (plan->hasBassHandoff()) {
            description += QLatin1Char(' ') +
                    tr("The bass swaps %1 in.")
                            .arg(clockText(plan->handoffProgress *
                                            static_cast<double>(plan->durationMs),
                                    false));
        }

        // The rows
        QStringList modeParts{toString(plan->mode)};
        if (plan->curve == TransitionPlan::Curve::DjBlend) {
            modeParts.append(tr("long blend"));
        }
        if (plan->hasBassHandoff()) {
            modeParts.append(tr("bass handoff"));
        }
        if (plan->fallbackReason) {
            modeParts.append(tr("fell back: %1")
                            .arg(toString(*plan->fallbackReason).replace('_', ' ')));
        }
        m_metadata.value(tr("Mode"))->setText(modeParts.join(kSeparator));
        m_metadata.value(tr("Entry"))
                ->setText(QStringLiteral("%1 %2 → %3 %4").arg(out, cue, in, entry));
        QString tempo = tr("unchanged");
        if (plan->isBeatmatch()) {
            const QString percent =
                    signedNumber((plan->incomingTempoRatio - 1.0) * 100.0, 2) +
                    QStringLiteral(" %");
            const double bpm = pIncoming ? pIncoming->getBpm() : 0.0;
            tempo = bpm > 0.0
                    ? QStringLiteral("%1 %2 → %3")
                                    .arg(in,
                                            QString::number(bpm, 'f', 2),
                                            QString::number(
                                                    bpm * plan->incomingTempoRatio,
                                                    'f',
                                                    2)) +
                            kSeparator + percent
                    : QStringLiteral("%1 ×%2").arg(in,
                              QString::number(
                                      plan->incomingTempoRatio, 'f', 4)) +
                            kSeparator + percent;
        }
        m_metadata.value(tr("Tempo"))->setText(tempo);
        m_metadata.value(tr("Bass swap"))
                ->setText(plan->hasBassHandoff()
                                ? tr("at %1 %").arg(std::lround(plan->handoffProgress * 100.0)) +
                                        kSeparator +
                                        tr("%1 in").arg(clockText(plan->handoffProgress *
                                                        static_cast<double>(plan->durationMs),
                                                false))
                                : tr("none"));
        m_metadata.value(tr("Gain"))
                ->setText(
                        QStringLiteral("%1 %2 dB")
                                .arg(out,
                                        signedNumber(plan->outgoingGainDb, 1)) +
                        kSeparator +
                        QStringLiteral("%1 %2 dB")
                                .arg(in,
                                        signedNumber(plan->incomingGainDb, 1)));
        QStringList planner{SmartMixAssist::kPlannerVersion, tr("assist")};
        if (plan->structureUsed.value_or(false)) {
            planner.append(tr("phrases"));
        }
        if (plan->stemsUsed) {
            planner.append(tr("stems"));
        }
        m_metadata.value(tr("Planner"))->setText(planner.join(kSeparator));
        m_readings.value(tr("Confidence"))->setText(QString::number(plan->confidence, 'f', 2));
        m_pConfidenceRing->setValue(plan->confidence);
        m_readings.value(tr("Length"))->setText(length);
        const double outgoingBpm = deckBpm(outgoing);
        if (outgoingBpm > 0.0) {
            const double bars = static_cast<double>(plan->durationMs) * outgoingBpm / 240000.0;
            m_pLength->setText(QString::number(std::lround(bars)));
            m_pLengthUnit->setText(tr("bars"));
        } else {
            m_pLength->setText(length);
            m_pLengthUnit->setText(tr("long"));
        }
    } else {
        switch (status) {
        case Status::NoServer:
            mode = tr("No plan");
            description = tr(
                    "Smart Mix plans come from a Crate server. Sign in to one in "
                    "Preferences, Crate.");
            break;
        case Status::NoTransition:
            mode = tr("No plan");
            description = tr(
                    "Play a deck and load the next track on another deck: the "
                    "assistant plans the transition from the deck on air.");
            break;
        case Status::NotCrate:
            mode = tr("No plan");
            description = tr(
                    "Both decks need tracks of the same Crate server, which plans "
                    "the transition from their analysis.");
            break;
        case Status::Requesting:
            mode = tr("Planning");
            description = tr("Asking Crate for the transition.");
            break;
        case Status::NoPlan:
            mode = tr("No plan");
            description = tr("Crate has no plan that fits these tracks: %1")
                                  .arg(pAssist ? pAssist->problem() : QString());
            break;
        case Status::AutoDj:
            mode = tr("Auto DJ");
            description = tr(
                    "Auto DJ is on and mixes the decks. Turn it off, or pick "
                    "Assist, to mix with the assistant.");
            break;
        case Status::Ready:
        case Status::Prepared:
        case Status::Executing:
        case Status::Running:
            break;
        }
    }
    m_pMode->setText(mode);
    setParagraph(m_pDescription, description, "fontSizeCaption");

    // The state of the transition
    QString state;
    QString caption;
    QString badge = QStringLiteral("ready");
    const QString in = deckLetter(incoming);
    switch (status) {
    case Status::Ready:
        state = tr("Ready");
        caption = tr("deck %1 loaded · stopped").arg(in);
        break;
    case Status::Prepared:
        state = tr("Prepared");
        caption = plan && plan->isBeatmatch() ? tr("deck %1 cued · tempo matched").arg(in)
                                              : tr("deck %1 cued").arg(in);
        break;
    case Status::Executing:
        state = tr("Armed");
        caption = tr("deck %1 starts at the entry").arg(in);
        break;
    case Status::Running:
        state = tr("Mixing");
        caption = tr("a control you move is yours");
        break;
    case Status::NoPlan:
        state = tr("No plan");
        badge = QStringLiteral("warning");
        break;
    case Status::NoServer:
    case Status::NoTransition:
    case Status::NotCrate:
    case Status::Requesting:
    case Status::AutoDj:
        break;
    }
    m_pStateRow->setVisible(!state.isEmpty());
    m_pState->setText(state);
    m_pStateCaption->setText(caption);
    setState(m_pState, badge);

    m_pPrepare->setEnabled(pAssist && pAssist->canPrepare());
    m_pExecute->setEnabled(pAssist && pAssist->canExecute());
    m_pCancel->setEnabled(pAssist && pAssist->canCancel());
    showTimes();
    if (showsPlan && isVisible()) {
        m_clock.start();
    } else {
        m_clock.stop();
    }
}

void AssistPanel::showTimes() {
    const auto entry = m_pAssist ? m_pAssist->msToEntry() : std::nullopt;
    const QString text = entry ? clockText(*entry, true) : QString();
    m_pToEntry->setText(text);
    if (QLabel* pReading = m_readings.value(tr("Entry in"))) {
        pReading->setText(text);
    }
    if (m_pAssist) {
        // The entry passing changes what the buttons can do
        m_pPrepare->setEnabled(m_pAssist->canPrepare());
        m_pExecute->setEnabled(m_pAssist->canExecute());
    }
}

void AssistPanel::onShow() {
    if (m_pAssist) {
        m_pAssist->scheduleRefresh();
    }
    showAssist();
    m_clock.start();
}

bool AssistPanel::hasFocus() const {
    return QWidget::hasFocus();
}

QString AssistPanel::statusText() const {
    return m_pStatus->text();
}

QString AssistPanel::titleText() const {
    return m_pHost->isVisibleTo(this) ? m_pTitle->text() : QString();
}

QString AssistPanel::modeText() const {
    return m_pMode->text();
}

QString AssistPanel::stateText() const {
    return m_pStateRow->isVisibleTo(this) ? m_pState->text() : QString();
}

QString AssistPanel::metadata(const QString& key) const {
    const QLabel* pValue = m_metadata.value(key);
    return pValue && m_pMeta->isVisibleTo(this) ? pValue->text() : QString();
}

QString AssistPanel::readingText(const QString& key) const {
    const QLabel* pValue = m_readings.value(key);
    return pValue && m_pReadings->isVisibleTo(this) ? pValue->text() : QString();
}

} // namespace crate
