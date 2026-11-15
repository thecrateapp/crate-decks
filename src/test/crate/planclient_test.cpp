#include "crate/smartmix/planclient.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <memory>

#include "test/crate/fakecratenetwork.h"
#include "test/mixxxtest.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::PlanClient;
using crate::PlanContext;
using crate::PlanEdge;
using crate::PlanFetch;
using crate::PlanResults;
using crate::TransitionPlan;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;

const QString kPlansPath = QStringLiteral("/api/playback/transition-plans");

/// A track uid that is a valid UUID.
QString uid(int number) {
    return QStringLiteral("00000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0'));
}

PlanEdge edge(int from,
        int to,
        const QString& fromRevision = QStringLiteral("r1"),
        const QString& toRevision = QStringLiteral("r1")) {
    return PlanEdge{uid(from), uid(to), fromRevision, toRevision};
}

class CratePlanClientTest : public MixxxTest {
  protected:
    void SetUp() override {
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(options));
        m_pClient->setTokenSource(&m_tokens);
        PlanClient::Options planOptions;
        planOptions.schedule = m_scheduler.function();
        m_pPlans = std::make_unique<PlanClient>(m_pClient.get(), std::move(planOptions));
    }

    void TearDown() override {
        m_pPlans.reset();
        m_pClient.reset();
    }

    QByteArray fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/transition-plans/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return file.readAll();
    }

    bool waitForRequests(int count) {
        return QTest::qWaitFor(
                [&] {
                    return m_network.sent.size() >= count;
                },
                5000);
    }

    static QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
        return QJsonDocument::fromJson(sent.body).object();
    }

    /// The server's answer to `sent`: the real beatmatch plan (from the dev
    /// stack, real-beatmatch.json) for each edge, in order.
    QByteArray answerTo(const FakeNetworkAccessManager::Sent& sent) const {
        const QJsonObject body = bodyOf(sent);
        const QJsonObject realPlan = QJsonDocument::fromJson(
                fixture(QStringLiteral("real-beatmatch.json")))
                                             .object();
        QJsonArray plans;
        const QJsonArray edges = body.value(QStringLiteral("edges")).toArray();
        for (const QJsonValue& edge : edges) {
            QJsonObject plan = realPlan;
            for (const QString& key : {QStringLiteral("outgoingTrackEntityUid"),
                         QStringLiteral("incomingTrackEntityUid")}) {
                plan.insert(key, edge.toObject().value(key));
            }
            plans.append(plan);
        }
        return QJsonDocument(QJsonObject{
                                     {QStringLiteral("plannerVersion"),
                                             body.value(QStringLiteral("plannerVersion"))
                                                     .toString(QStringLiteral("smart-mix-v2"))},
                                     {QStringLiteral("plans"), plans},
                             })
                .toJson();
    }

    /// Answers every open request with a plan per edge.
    void answerAll() {
        for (const auto& sent : std::as_const(m_network.sent)) {
            if (sent.pReply && !sent.pReply->isFinished()) {
                sent.pReply->respond(200, answerTo(sent));
            }
        }
    }

    static PlanResults resultOf(QSignalSpy& spy) {
        if (spy.isEmpty() && !spy.wait(5000)) {
            ADD_FAILURE() << "The fetch did not finish";
            return PlanResults();
        }
        return spy.first().first().value<PlanResults>();
    }

    /// Requests `edges` and answers them all; the results.
    PlanResults fetch(const QList<PlanEdge>& edges, const PlanContext& context = PlanContext()) {
        QSignalSpy finished(m_pPlans->requestPlans(edges, context), &PlanFetch::finished);
        QTest::qWait(0);
        answerAll();
        return resultOf(finished);
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    FakeScheduler m_scheduler;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<PlanClient> m_pPlans;
};

TEST_F(CratePlanClientTest, BatchesEdges) {
    // 40 edges of a queue, the first one asked twice.
    QList<PlanEdge> edges;
    for (int index = 0; index < 40; ++index) {
        edges.append(edge(index, index + 1));
    }
    edges.append(edge(0, 1));
    PlanContext context;
    context.plannerVersion = QStringLiteral("smart-mix-v3");
    QSignalSpy finished(m_pPlans->requestPlans(edges, context), &PlanFetch::finished);

    ASSERT_TRUE(waitForRequests(2));
    QTest::qWait(0);
    ASSERT_EQ(2, m_network.sent.size());
    QList<QString> sentOutgoing;
    for (const auto& sent : std::as_const(m_network.sent)) {
        EXPECT_EQ(QNetworkAccessManager::PostOperation, sent.operation);
        EXPECT_EQ(kPlansPath, sent.request.url().path());
        const QJsonObject body = bodyOf(sent);
        EXPECT_EQ(QStringLiteral("smart-mix-v3"),
                body.value(QStringLiteral("plannerVersion")).toString());
        const QJsonArray sentEdges = body.value(QStringLiteral("edges")).toArray();
        for (const QJsonValue& value : sentEdges) {
            const QJsonObject sentEdge = value.toObject();
            sentOutgoing.append(
                    sentEdge.value(QStringLiteral("outgoingTrackEntityUid")).toString());
            const QJsonObject sentContext = sentEdge.value(QStringLiteral("context")).toObject();
            EXPECT_EQ(QStringLiteral("decks"),
                    sentContext.value(QStringLiteral("source")).toString());
            EXPECT_TRUE(sentContext.value(QStringLiteral("automatic")).toBool());
            EXPECT_EQ(8000, sentContext.value(QStringLiteral("preferredDurationMs")).toInt());
            // Only what a server without S5 accepts.
            EXPECT_FALSE(sentContext.contains(QStringLiteral("assist")));
        }
    }
    EXPECT_EQ(32, bodyOf(m_network.sent.at(0)).value(QStringLiteral("edges")).toArray().size());
    EXPECT_EQ(8, bodyOf(m_network.sent.at(1)).value(QStringLiteral("edges")).toArray().size());
    // Every edge once, in queue order.
    ASSERT_EQ(40, sentOutgoing.size());
    for (int index = 0; index < 40; ++index) {
        EXPECT_EQ(uid(index), sentOutgoing.at(index));
    }

    answerAll();
    const PlanResults results = resultOf(finished);
    ASSERT_EQ(41, static_cast<int>(results.plans.size()));
    for (int index = 0; index < 40; ++index) {
        ASSERT_TRUE(results.plans.at(index).ok()) << index;
        EXPECT_EQ(uid(index), results.plans.at(index).value().outgoingTrackEntityUid);
        EXPECT_EQ(uid(index + 1), results.plans.at(index).value().incomingTrackEntityUid);
        EXPECT_EQ(TransitionPlan::Mode::Beatmatch, results.plans.at(index).value().mode);
    }
    ASSERT_TRUE(results.plans.at(40).ok());
    EXPECT_EQ(uid(0), results.plans.at(40).value().outgoingTrackEntityUid);
    EXPECT_EQ(40, m_pPlans->cachedPlans());
}

TEST_F(CratePlanClientTest, CachesByRevision) {
    const PlanResults first = fetch({edge(1, 2)});
    ASSERT_EQ(1, m_network.sent.size());
    ASSERT_TRUE(first.plans.at(0).ok());

    // The same edge, revisions and context: from the cache, still later.
    QSignalSpy cached(m_pPlans->requestPlans({edge(1, 2)}, PlanContext()), &PlanFetch::finished);
    EXPECT_TRUE(cached.isEmpty());
    const PlanResults again = resultOf(cached);
    ASSERT_TRUE(again.plans.at(0).ok());
    EXPECT_EQ(1, m_network.sent.size());
    EXPECT_TRUE(m_pPlans->cachedPlan(edge(1, 2), PlanContext()).has_value());

    // Another context is another plan.
    PlanContext longer;
    longer.preferredDurationMs = 12000;
    EXPECT_FALSE(m_pPlans->cachedPlan(edge(1, 2), longer).has_value());
    fetch({edge(1, 2)}, longer);
    EXPECT_EQ(2, m_network.sent.size());

    // Another profile revision of the incoming track is another plan.
    fetch({edge(1, 2, QStringLiteral("r1"), QStringLiteral("r2"))});
    EXPECT_EQ(3, m_network.sent.size());
    EXPECT_TRUE(m_pPlans
                    ->cachedPlan(edge(1, 2, QStringLiteral("r1"), QStringLiteral("r2")),
                            PlanContext())
                    .has_value());
}

TEST_F(CratePlanClientTest, InvalidatesOnProfileChange) {
    fetch({edge(1, 2), edge(2, 3), edge(4, 5)});
    ASSERT_EQ(3, m_pPlans->cachedPlans());

    // Track 2 has a new profile: both its plans go, track 4's stay.
    fetch({edge(2, 6, QStringLiteral("r2"))});
    EXPECT_FALSE(m_pPlans->cachedPlan(edge(1, 2), PlanContext()).has_value());
    EXPECT_FALSE(m_pPlans->cachedPlan(edge(2, 3), PlanContext()).has_value());
    EXPECT_TRUE(m_pPlans->cachedPlan(edge(4, 5), PlanContext()).has_value());
    EXPECT_EQ(2, m_pPlans->cachedPlans());

    // As when the library sync says track 4 changed.
    m_pPlans->invalidateTrack(uid(4).toUpper());
    EXPECT_FALSE(m_pPlans->cachedPlan(edge(4, 5), PlanContext()).has_value());
    EXPECT_EQ(1, m_pPlans->cachedPlans());

    // A plan of the old profile that arrives after the change is not kept.
    QSignalSpy late(m_pPlans->requestPlans({edge(7, 8)}, PlanContext()), &PlanFetch::finished);
    ASSERT_TRUE(waitForRequests(3));
    // Track 8 changes while the plan is on its way; both answers come now.
    fetch({edge(8, 9, QStringLiteral("r2"))});
    EXPECT_EQ(4, m_network.sent.size());
    const PlanResults results = resultOf(late);
    ASSERT_TRUE(results.plans.at(0).ok());
    EXPECT_FALSE(m_pPlans->cachedPlan(edge(7, 8), PlanContext()).has_value());
}

TEST_F(CratePlanClientTest, RetriesOnce) {
    QSignalSpy retried(m_pPlans->requestPlans({edge(1, 2)}, PlanContext()), &PlanFetch::finished);
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(503);
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(std::chrono::milliseconds(250), m_scheduler.tasks.first().delay);
    m_scheduler.runAll();
    ASSERT_TRUE(waitForRequests(2));
    m_network.last().pReply->respond(200, answerTo(m_network.last()));
    const PlanResults recovered = resultOf(retried);
    EXPECT_TRUE(recovered.plans.at(0).ok());

    // A second failure is the answer.
    QSignalSpy failed(m_pPlans->requestPlans({edge(3, 4)}, PlanContext()), &PlanFetch::finished);
    ASSERT_TRUE(waitForRequests(3));
    m_network.last().pReply->failNetwork();
    m_scheduler.runAll();
    ASSERT_TRUE(waitForRequests(4));
    m_network.last().pReply->failNetwork();
    const PlanResults results = resultOf(failed);
    ASSERT_FALSE(results.plans.at(0).ok());
    EXPECT_EQ(crate::Error::Kind::Network, results.plans.at(0).error().kind);
    EXPECT_TRUE(m_scheduler.tasks.isEmpty());

    // A request the server rejects is not sent again.
    QSignalSpy rejected(m_pPlans->requestPlans({edge(5, 6)}, PlanContext()),
            &PlanFetch::finished);
    ASSERT_TRUE(waitForRequests(5));
    m_network.last().pReply->respond(422);
    const PlanResults invalid = resultOf(rejected);
    ASSERT_FALSE(invalid.plans.at(0).ok());
    EXPECT_EQ(422, invalid.plans.at(0).error().httpStatus);
    EXPECT_TRUE(m_scheduler.tasks.isEmpty());
    EXPECT_EQ(5, m_network.sent.size());
}

TEST_F(CratePlanClientTest, ParsesTheRealBatchAnswer) {
    // The first two edges of real-batch-response.json, which
    // export_real_transition_plans.py asked for in this client's default
    // context: a beatmatch and a fallback. The other two edges of that batch
    // had other contexts.
    QJsonObject answer = QJsonDocument::fromJson(
            fixture(QStringLiteral("real-batch-response.json")))
                                 .object();
    QJsonArray plans = answer.value(QStringLiteral("plans")).toArray();
    ASSERT_EQ(4, plans.size());
    plans = QJsonArray{plans.at(0), plans.at(1)};
    answer.insert(QStringLiteral("plans"), plans);
    QList<PlanEdge> edges;
    for (const QJsonValue& plan : std::as_const(plans)) {
        edges.append(PlanEdge{
                plan.toObject().value(QStringLiteral("outgoingTrackEntityUid")).toString(),
                plan.toObject().value(QStringLiteral("incomingTrackEntityUid")).toString(),
                QStringLiteral("r1"),
                QStringLiteral("r1")});
    }
    PlanContext context;
    context.plannerVersion = QStringLiteral("smart-mix-v3");
    context.allowBassHandoff = true;
    QSignalSpy finished(m_pPlans->requestPlans(edges, context), &PlanFetch::finished);
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(200, QJsonDocument(answer).toJson());

    const PlanResults results = resultOf(finished);
    ASSERT_EQ(2, static_cast<int>(results.plans.size()));
    ASSERT_TRUE(results.plans.at(0).ok()) << results.plans.at(0).error().message.toStdString();
    ASSERT_TRUE(results.plans.at(1).ok()) << results.plans.at(1).error().message.toStdString();
    EXPECT_EQ(TransitionPlan::Mode::Beatmatch, results.plans.at(0).value().mode);
    EXPECT_EQ(TransitionPlan::Mode::Adaptive, results.plans.at(1).value().mode);
    EXPECT_EQ(TransitionPlan::FallbackReason::IncompatibleTempo,
            results.plans.at(1).value().fallbackReason);
    EXPECT_EQ(2, m_pPlans->cachedPlans());
}

TEST_F(CratePlanClientTest, RejectsAnswersThatDoNotMatch) {
    QSignalSpy finished(m_pPlans->requestPlans({edge(1, 2), edge(3, 4)}, PlanContext()),
            &PlanFetch::finished);
    ASSERT_TRUE(waitForRequests(1));
    QJsonObject answer = QJsonDocument::fromJson(answerTo(m_network.last())).object();
    QJsonArray plans = answer.value(QStringLiteral("plans")).toArray();
    QJsonObject swapped = plans.at(1).toObject();
    swapped.insert(QStringLiteral("incomingTrackEntityUid"), uid(9));
    plans.replace(1, swapped);
    answer.insert(QStringLiteral("plans"), plans);
    m_network.last().pReply->respond(200, QJsonDocument(answer).toJson());

    const PlanResults results = resultOf(finished);
    EXPECT_TRUE(results.plans.at(0).ok());
    ASSERT_FALSE(results.plans.at(1).ok());
    EXPECT_EQ(QStringLiteral("The plan is for another edge"),
            results.plans.at(1).error().message);
    EXPECT_EQ(1, m_pPlans->cachedPlans());
}

TEST_F(CratePlanClientTest, SendsAssistOnlyWhenAsked) {
    PlanContext assist;
    assist.automatic = false;
    assist.assist = true;
    assist.allowBassHandoff = true;
    m_pPlans->requestPlans({edge(1, 2)}, assist);
    ASSERT_TRUE(waitForRequests(1));
    const QJsonObject body = bodyOf(m_network.last());
    // Without a planner, the server picks its default.
    EXPECT_FALSE(body.contains(QStringLiteral("plannerVersion")));
    const QJsonObject context = body.value(QStringLiteral("edges"))
                                        .toArray()
                                        .first()
                                        .toObject()
                                        .value(QStringLiteral("context"))
                                        .toObject();
    EXPECT_TRUE(context.value(QStringLiteral("assist")).toBool());
    EXPECT_FALSE(context.value(QStringLiteral("automatic")).toBool());
    EXPECT_TRUE(context.value(QStringLiteral("allowBassHandoff")).toBool());
    EXPECT_FALSE(context.contains(QStringLiteral("allowLongBeatmatch")));
}

} // namespace
