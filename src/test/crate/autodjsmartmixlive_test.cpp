// F6.C-PR3 and F6.C-PR4, live: the plans Auto DJ's Crate Smart Mix and the
// assist mode ask a Crate dev stack for, with their own planner contexts, the plan client and the
// server's capabilities as AddServerFlow keeps them. It measures how long the
// plans take against the director's 3 s deadline and checks that every plan
// fits its tracks (TransitionPlan::validate()).
//
// Skipped unless CRATE_DECKS_LIVE_URL is set, so CI never runs it:
//   CRATE_DECKS_LIVE_URL         a dev stack (loopback or private address
//                                only), e.g. http://127.0.0.1:8585
//   CRATE_DECKS_LIVE_EMAIL,
//   CRATE_DECKS_LIVE_PASSWORD    an account of that dev stack; never
//                                production
//   CRATE_DECKS_LIVE_PLAYLIST    optional: the playlist whose consecutive
//                                tracks make the edges (default
//                                "G3 · Auto DJ (industrial EBM)")
// It reports [ MEASURE  ] lines.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QUrlQuery>
#include <iostream>
#include <memory>
#include <optional>

#include "crate/client/crateclient.h"
#include "crate/crateaddserverflow.h"
#include "crate/crateservers.h"
#include "crate/session/cratesecretstore.h"
#include "crate/session/cratesession.h"
#include "crate/smartmix/autodjsmartmixadapter.h"
#include "crate/smartmix/planclient.h"
#include "crate/smartmix/smartmixassist.h"
#include "mixer/playerinfo.h"
#include "test/mixxxdbtest.h"

namespace {

using crate::PlanEdge;
using crate::PlanFetch;
using crate::PlanResults;
using crate::TransitionPlan;

const QString kDefaultPlaylist = QStringLiteral("G3 · Auto DJ (industrial EBM)");
constexpr int kNetworkWaitMs = 60000;
// The edges: what Auto DJ asks ahead (two), at a handover (one), and more
constexpr int kTracks = 12;

void measure(const std::string& line) {
    std::cout << "[ MEASURE  ] " << line << std::endl;
}

bool isDevStack(const QUrl& url) {
    const QString host = url.host();
    if (host == QLatin1String("localhost")) {
        return true;
    }
    const QHostAddress address(host);
    if (address.isNull()) {
        return false;
    }
    if (address.isLoopback() || address.isLinkLocal()) {
        return true;
    }
    // QHostAddress::isPrivateUse() needs Qt 6.6; the CI builds with Qt 6.4
    for (const char* subnet : {"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "fc00::/7"}) {
        if (address.isInSubnet(QHostAddress::parseSubnet(QString::fromLatin1(subnet)))) {
            return true;
        }
    }
    return false;
}

class AutoDJSmartMixLive : public MixxxDbTest {
  public:
    AutoDJSmartMixLive()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        const QString url = qEnvironmentVariable("CRATE_DECKS_LIVE_URL");
        if (url.isEmpty()) {
            GTEST_SKIP() << "Live check against a Crate dev stack (F6.C-PR3): set "
                            "CRATE_DECKS_LIVE_URL, CRATE_DECKS_LIVE_EMAIL and "
                            "CRATE_DECKS_LIVE_PASSWORD";
        }
        ASSERT_TRUE(isDevStack(QUrl(url))) << "Only a dev stack: " << url.toStdString();
        const QString email = qEnvironmentVariable("CRATE_DECKS_LIVE_EMAIL");
        const QString password = qEnvironmentVariable("CRATE_DECKS_LIVE_PASSWORD");
        ASSERT_FALSE(email.isEmpty() || password.isEmpty())
                << "Set CRATE_DECKS_LIVE_EMAIL and CRATE_DECKS_LIVE_PASSWORD";
        crate::CrateServers::Dependencies dependencies;
        dependencies.pConfig = config();
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.pSecretStore = &m_secrets;
        m_pServers = std::make_unique<crate::CrateServers>(dependencies);
        m_pServers->start();

        using Step = crate::AddServerFlow::Step;
        crate::AddServerFlow flow(m_pServers.get());
        flow.setAddress(url);
        if (flow.step() == Step::ConfirmLocalNetwork) {
            flow.confirmLocalNetwork();
        }
        ASSERT_TRUE(QTest::qWaitFor(
                [&flow] {
                    return flow.step() == Step::SignIn || flow.step() == Step::EnterAddress;
                },
                kNetworkWaitMs));
        ASSERT_EQ(Step::SignIn, flow.step()) << flow.error().toStdString();
        flow.loginWithPassword(email, password);
        ASSERT_TRUE(QTest::qWaitFor(
                [&flow] {
                    return flow.step() != Step::SigningIn;
                },
                kNetworkWaitMs));
        ASSERT_EQ(Step::Done, flow.step()) << flow.error().toStdString();
        ASSERT_EQ(1, m_pServers->sessions().size());
        m_serverId = m_pServers->sessions().constFirst()->profile().id;
        m_pClient = m_pServers->client(m_serverId);
        ASSERT_NE(nullptr, m_pClient);
    }

    void TearDown() override {
        m_pServers.reset();
    }

  protected:
    std::optional<QJsonDocument> get(const QString& path, const QUrlQuery& query = QUrlQuery()) {
        crate::RequestOptions options;
        options.query = query;
        crate::Reply* pReply = m_pClient->get(path, options);
        // Shared with the slot, which may outlive this call on a timeout
        auto pAnswer = std::make_shared<std::optional<crate::JsonResult>>();
        QObject::connect(pReply,
                &crate::Reply::finished,
                [pAnswer](const crate::JsonResult& result) {
                    *pAnswer = result;
                });
        EXPECT_TRUE(QTest::qWaitFor(
                [pAnswer] {
                    return pAnswer->has_value();
                },
                kNetworkWaitMs));
        if (!pAnswer->has_value() || !(*pAnswer)->ok()) {
            return std::nullopt;
        }
        return (*pAnswer)->value();
    }

    /// The uids of the playlist's tracks with a mix profile, in order.
    QStringList playlistTracks(const QString& name) {
        const auto tree = get(QStringLiteral("/api/decks/containers"));
        if (!tree) {
            return {};
        }
        const QJsonArray containers = tree->object().value(QStringLiteral("containers")).toArray();
        for (const QJsonValue& container : containers) {
            const QJsonObject object = container.toObject();
            if (object.value(QStringLiteral("kind")).toString() != QLatin1String("playlist") ||
                    object.value(QStringLiteral("name")).toString() != name) {
                continue;
            }
            const auto tracks = get(QStringLiteral("/api/decks/containers/playlist/%1/tracks")
                                            .arg(object.value(QStringLiteral("id"))
                                                            .toVariant()
                                                            .toString()),
                    QUrlQuery(QStringLiteral("limit=100")));
            QStringList uids;
            const QJsonArray rows = tracks
                    ? tracks->object().value(QStringLiteral("tracks")).toArray()
                    : QJsonArray();
            for (const QJsonValue& track : rows) {
                const QString quality =
                        track.toObject().value(QStringLiteral("profile_quality")).toString();
                if (quality == QLatin1String("full") || quality == QLatin1String("partial")) {
                    uids.append(track.toObject().value(QStringLiteral("entity_uid")).toString());
                }
            }
            return uids;
        }
        return {};
    }

    qint64 durationMs(const QString& uid) {
        const auto profile = get(QStringLiteral("/api/tracks/by-entity/%1/mix-profile").arg(uid));
        return profile ? profile->object().value(QStringLiteral("durationMs")).toInteger() : 0;
    }

    /// Asks for `edges` and waits; the results and how long they took.
    std::pair<PlanResults, double> plans(crate::PlanClient* pPlans,
            const QList<PlanEdge>& edges,
            const crate::PlanContext& context) {
        QElapsedTimer timer;
        timer.start();
        QSignalSpy finished(pPlans->requestPlans(edges, context), &PlanFetch::finished);
        EXPECT_TRUE(finished.wait(kNetworkWaitMs));
        const double ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        return {finished.isEmpty() ? PlanResults() : finished.first().first().value<PlanResults>(),
                ms};
    }

    QNetworkAccessManager m_network;
    crate::SessionOnlySecretStore m_secrets;
    std::unique_ptr<crate::CrateServers> m_pServers;
    QString m_serverId;
    crate::Client* m_pClient = nullptr;
};

TEST_F(AutoDJSmartMixLive, PlansAutoDJsEdgesWithinTheDeadline) {
    const QByteArray capabilities = m_pServers->lastCapabilities(m_serverId);
    EXPECT_TRUE(crate::smartMixOffered({capabilities}));

    // The adapter's own context: Auto DJ's 10 s, the default EQ on
    crate::AutoDJSmartMixAdapter::Options options;
    options.pConfig = config();
    options.install = false;
    options.capabilitiesOf = [capabilities](const QString&) -> std::optional<crate::Capabilities> {
        const auto parsed = crate::Capabilities::fromJson(QJsonDocument::fromJson(capabilities));
        if (!parsed.ok()) {
            return std::nullopt;
        }
        return parsed.value();
    };
    options.defaultEqOn = [](const QStringList&) {
        return true;
    };
    crate::AutoDJSmartMixAdapter adapter(std::move(options));
    const crate::PlanContext context = adapter.contextFor(m_serverId);
    EXPECT_EQ(QStringLiteral("smart-mix-v3"), context.plannerVersion);
    EXPECT_TRUE(context.localRefinement);
    EXPECT_EQ(10000, context.preferredDurationMs);
    EXPECT_TRUE(context.allowBeatmatch);
    EXPECT_TRUE(context.allowBassHandoff);
    EXPECT_TRUE(context.allowLongBeatmatch);

    const QString playlist = qEnvironmentVariable("CRATE_DECKS_LIVE_PLAYLIST", kDefaultPlaylist);
    QStringList uids = playlistTracks(playlist);
    ASSERT_GE(uids.size(), 4) << "Too few profiled tracks in " << playlist.toStdString();
    uids = uids.mid(0, kTracks);
    QHash<QString, qint64> durations;
    for (const QString& uid : std::as_const(uids)) {
        durations.insert(uid.toLower(), durationMs(uid));
    }
    QList<PlanEdge> edges;
    for (int i = 0; i + 1 < uids.size(); ++i) {
        edges.append(PlanEdge{uids[i], uids[i + 1], QString(), QString()});
    }

    crate::PlanClient planClient(m_pClient);
    // What Auto DJ asks ahead, what a handover asks, and the rest
    const auto [ahead, aheadMs] = plans(&planClient, edges.mid(0, 2), context);
    const auto [handover, handoverMs] = plans(&planClient, edges.mid(2, 1), context);
    const auto [rest, restMs] = plans(&planClient, edges.mid(3), context);
    measure("plans ahead (2 edges) " + std::to_string(aheadMs) + " ms");
    measure("plan at a handover (1 edge) " + std::to_string(handoverMs) + " ms, deadline " +
            std::to_string(crate::SmartMixDirector::kPlanDeadlineMs) + " ms");
    measure("plans of " + std::to_string(edges.size() - 3) + " more edges " +
            std::to_string(restMs) + " ms");
    EXPECT_LT(handoverMs, crate::SmartMixDirector::kPlanDeadlineMs);

    QHash<QString, int> modes;
    int fallbacks = 0;
    int withLimits = 0;
    int withStems = 0;
    int bassHandoffs = 0;
    int checked = 0;
    // F6b.PR1: Decks' check of the limits against the server's, on every
    // candidate of every menu (the server serves only valid ones)
    int candidates = 0;
    int agreeing = 0;
    for (const PlanResults* pResults : {&ahead, &handover, &rest}) {
        for (const crate::Result<TransitionPlan>& result : pResults->plans) {
            ASSERT_TRUE(result.ok()) << result.error().message.toStdString();
            const TransitionPlan& plan = result.value();
            const auto problem = plan.validate(durations.value(plan.outgoingTrackEntityUid),
                    durations.value(plan.incomingTrackEntityUid));
            EXPECT_FALSE(problem.has_value()) << problem.value_or(QString()).toStdString();
            if (context.localRefinement) {
                EXPECT_FALSE(plan.candidates.empty());
            }
            for (const crate::TransitionPlanCandidate& candidate : plan.candidates) {
                ++candidates;
                const QStringList codes = candidate.plan.mixes() && candidate.plan.policyLimits
                        ? candidate.plan.limitViolations(*candidate.plan.policyLimits)
                        : QStringList();
                agreeing += codes.isEmpty() ? 1 : 0;
                EXPECT_TRUE(codes.isEmpty()) << candidate.index << ": "
                                             << codes.join(QStringLiteral(", ")).toStdString();
            }
            modes[crate::toString(plan.mode)] += 1;
            fallbacks += plan.fallbackReason ? 1 : 0;
            withLimits += plan.policyLimits ? 1 : 0;
            withStems += plan.stemsUsed ? 1 : 0;
            bassHandoffs += plan.hasBassHandoff() ? 1 : 0;
            ++checked;
        }
    }
    EXPECT_EQ(edges.size(), checked);
    // S5 option B: every decks plan carries its policy limits
    EXPECT_EQ(checked, withLimits);
    for (auto it = modes.cbegin(); it != modes.cend(); ++it) {
        measure("mode " + it.key().toStdString() + ": " + std::to_string(it.value()));
    }
    measure("fallback reasons " + std::to_string(fallbacks) + ", bass handoffs " +
            std::to_string(bassHandoffs) + ", stems used " + std::to_string(withStems) +
            " of " + std::to_string(checked));
    measure(context.plannerVersion.toStdString() + ": Decks' limit check agrees with the " +
            "server on " + std::to_string(agreeing) + " of " + std::to_string(candidates) +
            " candidates");

    // The handover finds what was asked ahead in the cache
    EXPECT_TRUE(planClient.cachedPlan(edges[0], context).has_value());
    EXPECT_TRUE(planClient.cachedPlan(edges[1], context).has_value());
}

// F6.C-PR4: the assistant's context (smart-mix-v3, assist=true, the server's
// own length) gets plans that fit their tracks, with the assist mode's
// limits: long blends up to 96 s.
TEST_F(AutoDJSmartMixLive, PlansAssistEdgesWithTheAssistLimits) {
    const QByteArray capabilities = m_pServers->lastCapabilities(m_serverId);
    PlayerInfo::create();
    const auto destroyPlayerInfo = qScopeGuard([] { PlayerInfo::destroy(); });
    crate::SmartMixAssist::Options options;
    options.mappable = false;
    options.capabilitiesOf = [capabilities](const QString&) -> std::optional<crate::Capabilities> {
        const auto parsed = crate::Capabilities::fromJson(QJsonDocument::fromJson(capabilities));
        if (!parsed.ok()) {
            return std::nullopt;
        }
        return parsed.value();
    };
    options.defaultEqOn = [](const QStringList&) {
        return true;
    };
    crate::SmartMixAssist assist(std::move(options));
    const crate::PlanContext context = assist.contextFor(
            m_serverId, {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")});
    EXPECT_TRUE(context.assist);
    EXPECT_TRUE(context.automatic);
    EXPECT_EQ(QStringLiteral("smart-mix-v3"), context.plannerVersion);

    const QString playlist = qEnvironmentVariable("CRATE_DECKS_LIVE_PLAYLIST", kDefaultPlaylist);
    QStringList uids = playlistTracks(playlist);
    ASSERT_GE(uids.size(), 4) << "Too few profiled tracks in " << playlist.toStdString();
    uids = uids.mid(0, kTracks);
    QHash<QString, qint64> durations;
    for (const QString& uid : std::as_const(uids)) {
        durations.insert(uid.toLower(), durationMs(uid));
    }
    QList<PlanEdge> edges;
    for (int i = 0; i + 1 < uids.size(); ++i) {
        edges.append(PlanEdge{uids[i], uids[i + 1], QString(), QString()});
    }
    crate::PlanClient planClient(m_pClient);
    // The assistant asks one edge when a deck loads
    const auto [first, firstMs] = plans(&planClient, edges.mid(0, 1), context);
    const auto [rest, restMs] = plans(&planClient, edges.mid(1), context);
    measure("assist plan (1 edge) " + std::to_string(firstMs) + " ms");

    QHash<QString, int> modes;
    qint64 longest = 0;
    int checked = 0;
    int longBlends = 0;
    for (const PlanResults* pResults : {&first, &rest}) {
        for (const crate::Result<TransitionPlan>& result : pResults->plans) {
            ASSERT_TRUE(result.ok()) << result.error().message.toStdString();
            const TransitionPlan& plan = result.value();
            const auto problem = plan.validate(durations.value(plan.outgoingTrackEntityUid),
                    durations.value(plan.incomingTrackEntityUid));
            EXPECT_FALSE(problem.has_value()) << problem.value_or(QString()).toStdString();
            ASSERT_TRUE(plan.policyLimits.has_value());
            // The assist mode's ceiling for long blends (S5, 96 s)
            EXPECT_EQ(96000, plan.policyLimits->maxLongBeatmatchMs);
            modes[crate::toString(plan.mode)] += 1;
            longest = std::max(longest, plan.durationMs);
            longBlends += plan.curve == TransitionPlan::Curve::DjBlend ? 1 : 0;
            ++checked;
        }
    }
    EXPECT_EQ(edges.size(), checked);
    for (auto it = modes.cbegin(); it != modes.cend(); ++it) {
        measure("assist mode " + it.key().toStdString() + ": " + std::to_string(it.value()));
    }
    measure("assist long blends " + std::to_string(longBlends) + " of " +
            std::to_string(checked) + ", longest " + std::to_string(longest) + " ms");
}

} // namespace
