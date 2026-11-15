#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "crate/client/crateresult.h"
#include "crate/smartmix/transitionplan.h"

namespace crate {

class Client;
class Reply;

/// The planner context of the edges of one PlanClient::requestPlans(): the
/// `context` of each edge in `POST /api/playback/transition-plans`, where
/// `source` is always "decks" (S5), and the planner to ask. The defaults are
/// AutoDJ's.
struct PlanContext {
    /// `smart-mix-v2` or `smart-mix-v3`; empty asks for the server's default
    /// (`capabilities.smart_mix.planner_version`).
    QString plannerVersion;
    /// A planned transition, as Auto DJ's and the assistant's; false is a
    /// skip the DJ asks for now (Listen's manual next), which the server
    /// plans as a short crossfade.
    bool automatic = true;
    bool offline = false;
    /// The transition length the DJ prefers; the server takes up to 12 s.
    int preferredDurationMs = 8000;
    QString userCueProfile = QStringLiteral("default");
    bool allowBeatmatch = true;
    bool allowTempoAdjustment = true;
    /// Sent only when set, as Listen does.
    bool allowBassHandoff = false;
    bool allowLongBeatmatch = false;
    /// The assistant's longer transitions (S5): long blends up to 96 s
    /// (Diego, 2026-11-15), within the plan's `policyLimits`. Sent only when
    /// set: a server without S5 rejects the field.
    bool assist = false;
    /// Asks `smart-mix-v3` for the candidate menu (STEM-D10). Sent only when
    /// set.
    bool localRefinement = false;

    QJsonObject toJson() const;
};

/// One edge of the queue: the outgoing and incoming tracks and the revisions
/// of their mix profiles (empty for a track without one). The server does
/// not take the revisions; they key the cache, as they key the server's.
struct PlanEdge {
    QString outgoingTrackEntityUid;
    QString incomingTrackEntityUid;
    QString outgoingProfileRevision;
    QString incomingProfileRevision;
};

/// What one PlanClient::requestPlans() got: one result per edge asked, in
/// the same order. A plan that does not parse, or a request that failed
/// after its retry, is an Error for its edges only.
struct PlanResults {
    std::vector<Result<TransitionPlan>> plans;
};

/// One PlanClient::requestPlans(). It emits finished() once and then deletes
/// itself; deleting it earlier only stops the answer, and the plans still
/// reach the cache.
class PlanFetch : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;

  signals:
    void finished(const crate::PlanResults& results);
};

/// Options of crate::PlanClient.
struct PlanClientOptions {
    /// Edges per request; the server takes at most 32 (design D8).
    int maxEdgesPerRequest = 32;
    /// Plans kept in memory; the least recently used go first.
    int cacheSize = 256;
    /// Wait before the single retry of a request that failed for a reason
    /// that may pass (no network, 429 or a 5xx).
    std::chrono::milliseconds retryDelay{250};
    /// Runs `callback` after `delay`. Defaults to a single-shot QTimer;
    /// tests replace it to control time.
    std::function<void(std::chrono::milliseconds delay, const std::function<void()>& callback)>
            schedule;
};

/// Asks one Crate server for Smart Mix transition plans (plan F6.C-PR1,
/// design D8): the planner stays on the server and Decks plays its plans.
///
/// - Edges whose plan is cached are answered from memory. The cache is keyed
///   by both uids, both profile revisions and the context, so a new profile
///   revision never gets an old plan. Asking with a new revision of a track,
///   or invalidateTrack(), drops that track's older plans.
/// - The rest go in `POST /api/playback/transition-plans`, up to
///   PlanClientOptions::maxEdgesPerRequest per request, without repeating an
///   edge.
/// - A request that fails for a reason that may pass is sent once more; the
///   Client already renews the access token on 401.
///
/// Plans come back parsed (TransitionPlan::fromJson()) and matched to their
/// edge; checking them against the tracks (TransitionPlan::validate()) is
/// for whoever plays them. Lives on the main thread; parsing 32 plans is
/// well under a frame.
class PlanClient : public QObject {
    Q_OBJECT
  public:
    using Options = PlanClientOptions;

    PlanClient(Client* pClient, Options options = Options(), QObject* parent = nullptr);
    ~PlanClient() override;

    /// The plans of `edges` in `context`, from the cache or the server. The
    /// fetch finishes later even when every plan is cached.
    PlanFetch* requestPlans(const QList<PlanEdge>& edges, const PlanContext& context);

    /// The cached plan of `edge` in `context`, if any, without asking.
    std::optional<TransitionPlan> cachedPlan(const PlanEdge& edge,
            const PlanContext& context) const;

    /// Drops the cached plans of every edge with this track, e.g. when its
    /// mix profile changed.
    void invalidateTrack(const QString& entityUid);
    void clear();

    int cachedPlans() const {
        return static_cast<int>(m_cache.size());
    }

  private:
    struct Fetch;
    struct Slot;
    struct Batch;
    struct CacheEntry {
        TransitionPlan plan;
        quint64 lastUsed = 0;
    };

    void noteRevision(const QString& uid, const QString& revision);
    void send(const std::shared_ptr<Batch>& pBatch);
    void onReply(const std::shared_ptr<Batch>& pBatch, const JsonResult& result);
    void fillBatch(const Batch& batch, const Error& error);
    void fill(quint64 fetchId, const QList<int>& indexes, const Result<TransitionPlan>& result);
    void store(const QString& key, const PlanEdge& edge, const TransitionPlan& plan);
    void finishBatch(quint64 fetchId);
    void finishFetchIfDone(quint64 fetchId);

    const QPointer<Client> m_pClient;
    Options m_options;

    quint64 m_nextFetchId = 1;
    std::map<quint64, std::unique_ptr<Fetch>> m_fetches;
    QHash<QString, CacheEntry> m_cache;
    quint64 m_useCounter = 0;
    /// The latest profile revision asked for, by lowercase uid.
    QHash<QString, QString> m_revisions;
};

} // namespace crate

Q_DECLARE_METATYPE(crate::PlanResults)
