#include "crate/smartmix/planclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <utility>

#include "crate/client/crateclient.h"
#include "crate/client/cratejson.h"
#include "moc_planclient.cpp"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::PlanClient");

const QString kPlansPath = QStringLiteral("/api/playback/transition-plans");
constexpr int kServerMaxEdges = 32;

/// A canonical lowercase uid, or empty if `text` is not one.
QString uidOf(const QString& text) {
    const QUuid uid = QUuid::fromString(text.trimmed());
    return uid.isNull() ? QString() : uid.toString(QUuid::WithoutBraces);
}

/// The failures that may pass: the server or the network, not the request.
bool isRetryable(const Error& error) {
    if (error.kind == Error::Kind::Network) {
        return true;
    }
    if (error.kind != Error::Kind::Http) {
        return false;
    }
    return error.httpStatus == 429 || error.httpStatus >= 500;
}

QString cacheKey(const PlanEdge& edge, const PlanContext& context) {
    return QStringList{
            uidOf(edge.outgoingTrackEntityUid),
            edge.outgoingProfileRevision,
            uidOf(edge.incomingTrackEntityUid),
            edge.incomingProfileRevision,
            context.plannerVersion,
            // QJsonObject keeps its keys sorted, so the text is canonical.
            QString::fromUtf8(QJsonDocument(context.toJson()).toJson(QJsonDocument::Compact)),
    }
            .join(QLatin1Char('\n'));
}

} // namespace

QJsonObject PlanContext::toJson() const {
    QJsonObject context{
            {QStringLiteral("source"), QStringLiteral("decks")},
            {QStringLiteral("automatic"), automatic},
            {QStringLiteral("offline"), offline},
            {QStringLiteral("preferredDurationMs"), preferredDurationMs},
            {QStringLiteral("userCueProfile"), userCueProfile},
            {QStringLiteral("allowBeatmatch"), allowBeatmatch},
            {QStringLiteral("allowTempoAdjustment"), allowTempoAdjustment},
    };
    if (allowBassHandoff) {
        context.insert(QStringLiteral("allowBassHandoff"), true);
    }
    if (allowLongBeatmatch) {
        context.insert(QStringLiteral("allowLongBeatmatch"), true);
    }
    if (assist) {
        context.insert(QStringLiteral("assist"), true);
    }
    if (localRefinement) {
        context.insert(QStringLiteral("localRefinement"), true);
    }
    return context;
}

/// One requestPlans(): a result per edge asked, filled as they come.
struct PlanClient::Fetch {
    QPointer<PlanFetch> pFetch;
    std::vector<std::optional<Result<TransitionPlan>>> results;
    /// Batches that have not answered.
    int pending = 0;
};

/// One edge sent: the edges asked that share it.
struct PlanClient::Slot {
    QString key;
    PlanEdge edge;
    QList<int> indexes;
};

/// One request of up to maxEdgesPerRequest edges.
struct PlanClient::Batch {
    quint64 fetchId = 0;
    PlanContext context;
    QList<Slot> sent;
    int attempt = 0;
};

PlanClient::PlanClient(Client* pClient, Options options, QObject* parent)
        : QObject(parent),
          m_pClient(pClient),
          m_options(std::move(options)) {
    if (!m_options.schedule) {
        m_options.schedule = [this](std::chrono::milliseconds delay,
                                     const std::function<void()>& callback) {
            QTimer::singleShot(delay, this, callback);
        };
    }
}

PlanClient::~PlanClient() = default;

PlanFetch* PlanClient::requestPlans(const QList<PlanEdge>& edges, const PlanContext& context) {
    auto* pFetch = new PlanFetch(this);
    const quint64 fetchId = m_nextFetchId++;
    auto pState = std::make_unique<Fetch>();
    pState->pFetch = pFetch;
    pState->results.resize(edges.size());

    for (const PlanEdge& edge : edges) {
        noteRevision(uidOf(edge.outgoingTrackEntityUid), edge.outgoingProfileRevision);
        noteRevision(uidOf(edge.incomingTrackEntityUid), edge.incomingProfileRevision);
    }
    QList<Slot> misses;
    QHash<QString, int> missByKey;
    for (int index = 0; index < edges.size(); ++index) {
        const PlanEdge& edge = edges.at(index);
        if (uidOf(edge.outgoingTrackEntityUid).isEmpty() ||
                uidOf(edge.incomingTrackEntityUid).isEmpty()) {
            pState->results[index] = Error::decode(
                    QStringLiteral("The edge has no track uid"));
            continue;
        }
        const QString key = cacheKey(edge, context);
        const auto cached = m_cache.find(key);
        if (cached != m_cache.end()) {
            cached->lastUsed = ++m_useCounter;
            pState->results[index] = cached->plan;
            continue;
        }
        const auto known = missByKey.constFind(key);
        if (known != missByKey.cend()) {
            misses[known.value()].indexes.append(index);
            continue;
        }
        missByKey.insert(key, static_cast<int>(misses.size()));
        misses.append(Slot{key, edge, {index}});
    }

    const int size = std::clamp(m_options.maxEdgesPerRequest, 1, kServerMaxEdges);
    QList<std::shared_ptr<Batch>> batches;
    for (int first = 0; first < misses.size(); first += size) {
        auto pBatch = std::make_shared<Batch>();
        pBatch->fetchId = fetchId;
        pBatch->context = context;
        pBatch->sent = misses.mid(first, size);
        batches.append(pBatch);
    }
    pState->pending = static_cast<int>(batches.size());
    m_fetches.emplace(fetchId, std::move(pState));

    if (batches.isEmpty()) {
        // Answered from the cache: later, so that the caller can connect.
        QMetaObject::invokeMethod(
                this,
                [this, fetchId] {
                    finishFetchIfDone(fetchId);
                },
                Qt::QueuedConnection);
    }
    for (const std::shared_ptr<Batch>& pBatch : std::as_const(batches)) {
        send(pBatch);
    }
    return pFetch;
}

std::optional<TransitionPlan> PlanClient::cachedPlan(
        const PlanEdge& edge, const PlanContext& context) const {
    const auto cached = m_cache.constFind(cacheKey(edge, context));
    if (cached == m_cache.cend()) {
        return std::nullopt;
    }
    return cached->plan;
}

void PlanClient::invalidateTrack(const QString& entityUid) {
    const QString uid = uidOf(entityUid);
    if (uid.isEmpty()) {
        return;
    }
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it->plan.outgoingTrackEntityUid == uid || it->plan.incomingTrackEntityUid == uid) {
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void PlanClient::clear() {
    m_cache.clear();
    m_revisions.clear();
}

void PlanClient::noteRevision(const QString& uid, const QString& revision) {
    if (uid.isEmpty()) {
        return;
    }
    const auto known = m_revisions.find(uid);
    if (known == m_revisions.end()) {
        m_revisions.insert(uid, revision);
        return;
    }
    if (known.value() != revision) {
        // A new profile: the plans of the old one will not be asked for again.
        *known = revision;
        invalidateTrack(uid);
    }
}

void PlanClient::send(const std::shared_ptr<Batch>& pBatch) {
    ++pBatch->attempt;
    if (!m_pClient) {
        fillBatch(*pBatch, Error::network(QStringLiteral("The Crate server is gone")));
        finishBatch(pBatch->fetchId);
        return;
    }
    const QJsonObject context = pBatch->context.toJson();
    QJsonArray edges;
    for (const Slot& slot : std::as_const(pBatch->sent)) {
        edges.append(QJsonObject{
                {QStringLiteral("outgoingTrackEntityUid"),
                        uidOf(slot.edge.outgoingTrackEntityUid)},
                {QStringLiteral("incomingTrackEntityUid"),
                        uidOf(slot.edge.incomingTrackEntityUid)},
                {QStringLiteral("context"), context},
        });
    }
    QJsonObject body{{QStringLiteral("edges"), edges}};
    if (!pBatch->context.plannerVersion.isEmpty()) {
        body.insert(QStringLiteral("plannerVersion"), pBatch->context.plannerVersion);
    }
    Reply* pReply = m_pClient->post(kPlansPath, QJsonDocument(body));
    connect(pReply, &Reply::finished, this, [this, pBatch](const JsonResult& result) {
        onReply(pBatch, result);
    });
}

void PlanClient::onReply(const std::shared_ptr<Batch>& pBatch, const JsonResult& result) {
    if (!result.ok()) {
        const Error& error = result.error();
        if (pBatch->attempt == 1 && isRetryable(error)) {
            kLogger.info() << "Asking again for" << pBatch->sent.size()
                           << "transition plans after:" << error.message;
            m_options.schedule(m_options.retryDelay, [guard = QPointer(this), pBatch] {
                if (guard) {
                    guard->send(pBatch);
                }
            });
            return;
        }
        kLogger.warning() << "No transition plans for" << pBatch->sent.size()
                          << "edges:" << error.message;
        fillBatch(*pBatch, error);
        finishBatch(pBatch->fetchId);
        return;
    }

    const Result<QJsonObject> object = json::requireObject(result.value());
    if (!object.ok()) {
        fillBatch(*pBatch, object.error());
        finishBatch(pBatch->fetchId);
        return;
    }
    json::ObjectReader reader(object.value());
    const QString plannerVersion = reader.requiredString(u"plannerVersion");
    const QJsonArray plans = reader.requiredArray(u"plans");
    std::optional<Error> answerError;
    if (!reader.ok()) {
        answerError = reader.error();
    } else if (!pBatch->context.plannerVersion.isEmpty() &&
            plannerVersion != pBatch->context.plannerVersion) {
        answerError = Error::decode(QStringLiteral("The server planned with %1, not %2")
                        .arg(plannerVersion, pBatch->context.plannerVersion));
    } else if (plans.size() != pBatch->sent.size()) {
        answerError = Error::decode(QStringLiteral("The server answered %1 plans for %2 edges")
                        .arg(plans.size())
                        .arg(pBatch->sent.size()));
    }
    if (answerError) {
        kLogger.warning() << "Unusable transition plans:" << answerError->message;
        fillBatch(*pBatch, *answerError);
        finishBatch(pBatch->fetchId);
        return;
    }

    // The server answers the edges in the order they were sent.
    for (int position = 0; position < pBatch->sent.size(); ++position) {
        const Slot& slot = pBatch->sent.at(position);
        const QJsonValue value = plans.at(position);
        Result<TransitionPlan> plan = value.isObject()
                ? TransitionPlan::fromJson(value.toObject())
                : Result<TransitionPlan>(
                          Error::decode(QStringLiteral("plan is not an object")));
        if (plan.ok() &&
                (plan.value().outgoingTrackEntityUid !=
                                uidOf(slot.edge.outgoingTrackEntityUid) ||
                        plan.value().incomingTrackEntityUid !=
                                uidOf(slot.edge.incomingTrackEntityUid))) {
            plan = Error::decode(QStringLiteral("The plan is for another edge"));
        }
        if (plan.ok()) {
            store(slot.key, slot.edge, plan.value());
        } else {
            kLogger.warning() << "Unusable transition plan:" << plan.error().message;
        }
        fill(pBatch->fetchId, slot.indexes, plan);
    }
    finishBatch(pBatch->fetchId);
}

void PlanClient::fillBatch(const Batch& batch, const Error& error) {
    for (const Slot& slot : batch.sent) {
        fill(batch.fetchId, slot.indexes, error);
    }
}

void PlanClient::fill(quint64 fetchId,
        const QList<int>& indexes,
        const Result<TransitionPlan>& result) {
    const auto it = m_fetches.find(fetchId);
    if (it == m_fetches.end()) {
        return;
    }
    for (const int index : indexes) {
        it->second->results[index] = result;
    }
}

void PlanClient::store(const QString& key, const PlanEdge& edge, const TransitionPlan& plan) {
    // A plan of a profile that changed while it was asked for is never
    // looked up again.
    if (m_revisions.value(plan.outgoingTrackEntityUid) != edge.outgoingProfileRevision ||
            m_revisions.value(plan.incomingTrackEntityUid) != edge.incomingProfileRevision) {
        return;
    }
    m_cache.insert(key, CacheEntry{plan, ++m_useCounter});
    const int limit = std::max(1, m_options.cacheSize);
    while (m_cache.size() > limit) {
        const auto oldest = std::min_element(m_cache.cbegin(),
                m_cache.cend(),
                [](const CacheEntry& left, const CacheEntry& right) {
                    return left.lastUsed < right.lastUsed;
                });
        m_cache.erase(oldest);
    }
}

void PlanClient::finishBatch(quint64 fetchId) {
    const auto it = m_fetches.find(fetchId);
    if (it == m_fetches.end()) {
        return;
    }
    --it->second->pending;
    finishFetchIfDone(fetchId);
}

void PlanClient::finishFetchIfDone(quint64 fetchId) {
    const auto it = m_fetches.find(fetchId);
    if (it == m_fetches.end() || it->second->pending > 0) {
        return;
    }
    std::unique_ptr<Fetch> pState = std::move(it->second);
    m_fetches.erase(it);
    PlanResults results;
    results.plans.reserve(pState->results.size());
    for (auto& result : pState->results) {
        results.plans.push_back(result.value_or(
                Error::decode(QStringLiteral("The edge got no plan"))));
    }
    if (pState->pFetch) {
        emit pState->pFetch->finished(results);
        pState->pFetch->deleteLater();
    }
}

} // namespace crate
