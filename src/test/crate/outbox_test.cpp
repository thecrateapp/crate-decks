#include "crate/writeback/outbox.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTest>
#include <QTimeZone>
#include <memory>

#include "crate/client/crateclient.h"
#include "crate/db/crateserverschema.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"

namespace {

using namespace std::chrono_literals;
using crate::Client;
using crate::ClientOptions;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;
using crate::writeback::Outbox;
namespace outboxkind = crate::writeback::outboxkind;

const QString kServer = QStringLiteral("srv-1");
const QString kOtherServer = QStringLiteral("srv-2");
const QString kUid = QStringLiteral("0b6f2c1e-5d6a-4c1b-9a51-2f0c8a3c9e10");

QJsonObject likePayload(const QString& uid) {
    return QJsonObject{{QStringLiteral("track_entity_uid"), uid}};
}

QJsonObject playEvent(const QString& clientEventId) {
    return QJsonObject{{QStringLiteral("client_event_id"), clientEventId},
            {QStringLiteral("track_entity_uid"), kUid},
            {QStringLiteral("played_seconds"), 120}};
}

QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
    return QJsonDocument::fromJson(sent.body).object();
}

class OutboxTest : public MixxxDbTest {
  protected:
    OutboxTest()
            : MixxxDbTest(true),
              m_now(QDateTime(QDate(2026, 11, 16), QTime(22, 0), QTimeZone::utc())) {
    }

    void SetUp() override {
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        // The outbox does its own retries
        options.maxAttempts = 1;
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(options));
        m_pClient->setTokenSource(&m_tokens);
    }

    void TearDown() override {
        m_pOutbox.reset();
        m_pClient.reset();
    }

    Outbox* makeOutbox(bool start = true) {
        Outbox::Options options;
        options.pDbConnectionPool = dbConnectionPooler();
        options.clientFor = [this](const QString&) -> Client* {
            return m_online ? m_pClient.get() : nullptr;
        };
        options.schedule = m_scheduler.function();
        options.now = [this] {
            return m_now;
        };
        m_pOutbox = std::make_unique<Outbox>(std::move(options));
        if (!start) {
            return m_pOutbox.get();
        }
        m_pOutbox->start();
        EXPECT_TRUE(QTest::qWaitFor([this] {
            return m_pOutbox->isLoaded();
        }));
        return m_pOutbox.get();
    }

    int storedRows() {
        m_pOutbox->waitForStorage();
        QSqlQuery query(dbConnection());
        EXPECT_TRUE(query.exec(QStringLiteral("SELECT COUNT(*) FROM crateserver_outbox")));
        EXPECT_TRUE(query.next());
        return query.value(0).toInt();
    }

    QList<FakeNetworkAccessManager::Sent> sentTo(const QString& path) const {
        QList<FakeNetworkAccessManager::Sent> matching;
        for (const auto& sent : m_network.sent) {
            if (sent.request.url().path() == path) {
                matching.append(sent);
            }
        }
        return matching;
    }

    FakeNetworkAccessManager m_network;
    FakeScheduler m_scheduler;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<Outbox> m_pOutbox;
    QDateTime m_now;
    bool m_online = true;
};

TEST_F(OutboxTest, PersistsAcrossRestart) {
    // No session: the entries wait
    m_online = false;
    Outbox* pOutbox = makeOutbox();
    pOutbox->enqueue(kServer, outboxkind::kPlayEvent, playEvent(QStringLiteral("evt-1")));
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(kUid));
    EXPECT_TRUE(m_network.sent.isEmpty());
    EXPECT_EQ(2, pOutbox->pendingCount(kServer));
    EXPECT_EQ(2, storedRows());

    // Decks restarts, signed in this time
    m_pOutbox.reset();
    m_online = true;
    pOutbox = makeOutbox();

    EXPECT_EQ(2, pOutbox->pendingCount(kServer));
    ASSERT_EQ(1, m_network.sent.size());
    const auto first = m_network.last();
    EXPECT_EQ(QStringLiteral("/api/me/play-events"), first.request.url().path());
    EXPECT_EQ(QStringLiteral("evt-1"),
            bodyOf(first).value(QStringLiteral("client_event_id")).toString());
    first.pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "id": 7})"));

    ASSERT_EQ(2, m_network.sent.size());
    const auto second = m_network.last();
    EXPECT_EQ(QStringLiteral("/api/me/likes"), second.request.url().path());
    EXPECT_EQ(likePayload(kUid), bodyOf(second));
    second.pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "added": true})"));

    EXPECT_EQ(0, pOutbox->pendingCount());
    EXPECT_EQ(0, storedRows());
}

TEST_F(OutboxTest, BacksOff) {
    Outbox* pOutbox = makeOutbox();
    pOutbox->enqueue(kServer, outboxkind::kPlayEvent, playEvent(QStringLiteral("evt-1")));

    const QList<std::chrono::milliseconds> waits = {1s, 5s, 30s, 5min, 30min, 30min};
    for (int i = 0; i < waits.size(); ++i) {
        ASSERT_EQ(i + 1, m_network.sent.size());
        // A network failure, and once a server error: both pass
        if (i == 2) {
            m_network.last().pReply->respond(503);
        } else {
            m_network.last().pReply->failNetwork();
        }
        ASSERT_EQ(1, m_scheduler.tasks.size()) << "after failure " << i + 1;
        EXPECT_EQ(waits[i], m_scheduler.tasks[0].delay) << "after failure " << i + 1;
        EXPECT_EQ(i + 1, m_network.sent.size());
        m_scheduler.runAll();
    }
    // The same entry every time
    for (const auto& sent : std::as_const(m_network.sent)) {
        EXPECT_EQ(QStringLiteral("evt-1"),
                bodyOf(sent)
                        .value(QStringLiteral("client_event_id"))
                        .toString());
    }

    // Delivered at last: the backoff starts again for the next entry
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "id": 1})"));
    EXPECT_EQ(0, pOutbox->pendingCount());
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(kUid));
    m_network.last().pReply->failNetwork();
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(1s, m_scheduler.tasks[0].delay);
}

TEST_F(OutboxTest, DropsAfterSevenDays) {
    // Left by a run eight days ago
    QSqlQuery insert(dbConnection());
    ASSERT_TRUE(insert.exec(QStringLiteral(
            "INSERT INTO crateserver_outbox (id, server_id, kind, payload, created_at) "
            "VALUES ('old', 'srv-1', 'like', '{\"track_entity_uid\": \"x\"}', "
            "'2026-11-08T21:59:00.000Z')")));
    Outbox* pOutbox = makeOutbox();
    EXPECT_TRUE(m_network.sent.isEmpty());
    EXPECT_EQ(0, pOutbox->pendingCount());
    EXPECT_EQ(0, storedRows());

    // One that keeps failing until it is seven days old
    QSignalSpy dropped(pOutbox, &Outbox::dropped);
    pOutbox->enqueue(kServer, outboxkind::kPlayEvent, playEvent(QStringLiteral("evt-1")));
    m_network.last().pReply->failNetwork();
    m_now = m_now.addDays(7).addSecs(1);
    m_scheduler.runAll();

    EXPECT_EQ(1, m_network.sent.size());
    EXPECT_EQ(0, pOutbox->pendingCount());
    ASSERT_EQ(1, dropped.size());
    EXPECT_EQ(kServer, dropped[0][0].toString());
    EXPECT_EQ(outboxkind::kPlayEvent, dropped[0][1].toString());
    EXPECT_EQ(0, storedRows());
}

TEST_F(OutboxTest, SendsInOrder) {
    Outbox* pOutbox = makeOutbox();
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(QStringLiteral("a")));
    pOutbox->enqueue(kServer, outboxkind::kUnlike, likePayload(QStringLiteral("a")));
    pOutbox->enqueue(kServer, outboxkind::kPlayEvent, playEvent(QStringLiteral("evt-1")));
    // Another server does not wait for this one
    pOutbox->enqueue(kOtherServer, outboxkind::kLike, likePayload(QStringLiteral("b")));

    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_EQ(likePayload(QStringLiteral("a")), bodyOf(m_network.sent[0]));
    EXPECT_EQ(likePayload(QStringLiteral("b")), bodyOf(m_network.sent[1]));
    m_network.sent[1].pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    EXPECT_EQ(2, m_network.sent.size());

    m_network.sent[0].pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    ASSERT_EQ(3, m_network.sent.size());
    const auto unlike = m_network.last();
    EXPECT_EQ(QNetworkAccessManager::CustomOperation, unlike.operation);
    EXPECT_EQ(QByteArrayLiteral("DELETE"),
            unlike.request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray());
    EXPECT_EQ(QStringLiteral("/api/me/likes"), unlike.request.url().path());
    EXPECT_EQ(likePayload(QStringLiteral("a")), bodyOf(unlike));
    unlike.pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));

    ASSERT_EQ(4, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("/api/me/play-events"), m_network.last().request.url().path());
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "id": 3})"));
    EXPECT_EQ(0, pOutbox->pendingCount());
}

TEST_F(OutboxTest, DropsWhatTheServerRefuses) {
    Outbox* pOutbox = makeOutbox();
    QSignalSpy dropped(pOutbox, &Outbox::dropped);
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(QStringLiteral("gone")));
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(QStringLiteral("a")));

    m_network.last().pReply->respond(404, QByteArrayLiteral(R"({"detail": "Track not found"})"));

    EXPECT_TRUE(m_scheduler.tasks.isEmpty());
    ASSERT_EQ(1, dropped.size());
    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_EQ(likePayload(QStringLiteral("a")), bodyOf(m_network.last()));
}

TEST_F(OutboxTest, FlushesOnNetworkRecovery) {
    Outbox* pOutbox = makeOutbox();
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(kUid));
    m_network.last().pReply->failNetwork();
    m_scheduler.runAll();
    m_network.last().pReply->failNetwork();
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(5s, m_scheduler.tasks[0].delay);

    pOutbox->flush();

    ASSERT_EQ(3, m_network.sent.size());
    // The retry it replaced does nothing
    m_scheduler.runAll();
    EXPECT_EQ(3, m_network.sent.size());
    // And the backoff starts again
    m_network.last().pReply->failNetwork();
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(1s, m_scheduler.tasks[0].delay);

    // A server without a client waits for the flush too
    m_online = false;
    m_scheduler.runAll();
    EXPECT_EQ(3, m_network.sent.size());
    EXPECT_TRUE(m_scheduler.tasks.isEmpty());
    m_online = true;
    pOutbox->flush();
    EXPECT_EQ(4, m_network.sent.size());
}

TEST_F(OutboxTest, RateLooksUpTheNumericTrackId) {
    Outbox* pOutbox = makeOutbox();
    pOutbox->enqueue(kServer,
            outboxkind::kRate,
            QJsonObject{{QStringLiteral("entity_uid"), kUid}, {QStringLiteral("rating"), 4}});

    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_EQ(QNetworkAccessManager::GetOperation, m_network.last().operation);
    EXPECT_EQ(QStringLiteral("/api/tracks/by-entity/%1/info").arg(kUid),
            m_network.last().request.url().path());
    m_network.last().pReply->respond(200,
            QByteArrayLiteral(R"({"id": 4211, "entity_uid": ")") + kUid.toLatin1() +
                    QByteArrayLiteral(R"(", "title": "Windowlicker", "rating": 2})"));

    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("/api/track/rate"), m_network.last().request.url().path());
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_id"), 4211}, {QStringLiteral("rating"), 4}}),
            bodyOf(m_network.last()));
    // A retry of the rating does not look the id up again
    m_network.last().pReply->respond(503);
    m_scheduler.runAll();
    ASSERT_EQ(3, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("/api/track/rate"), m_network.last().request.url().path());
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "rating": 4})"));
    EXPECT_EQ(0, pOutbox->pendingCount());

    // A track the server does not have is dropped
    QSignalSpy dropped(pOutbox, &Outbox::dropped);
    pOutbox->enqueue(kServer,
            outboxkind::kRate,
            QJsonObject{{QStringLiteral("entity_uid"), QStringLiteral("gone")},
                    {QStringLiteral("rating"), 1}});
    m_network.last().pReply->respond(404, QByteArrayLiteral(R"({"detail": "Track not found"})"));
    EXPECT_EQ(1, dropped.size());
    EXPECT_EQ(0, pOutbox->pendingCount());
    EXPECT_EQ(4, m_network.sent.size());
}

TEST_F(OutboxTest, SetsAsideAWriteTheServerKeepsFailing) {
    Outbox* pOutbox = makeOutbox();
    QSignalSpy dropped(pOutbox, &Outbox::dropped);
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(QStringLiteral("a")));
    pOutbox->enqueue(kServer, outboxkind::kUnlike, likePayload(QStringLiteral("a")));
    pOutbox->enqueue(kServer, outboxkind::kPlayEvent, playEvent(QStringLiteral("evt-1")));
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(QStringLiteral("b")));

    // The like of "a" fails with 503 until its backoff would reach 30 min
    const QList<std::chrono::milliseconds> waits = {1s, 5s, 30s, 5min};
    for (int i = 0; i < waits.size(); ++i) {
        ASSERT_EQ(i + 1, m_network.sent.size());
        EXPECT_EQ(likePayload(QStringLiteral("a")), bodyOf(m_network.last()));
        m_network.last().pReply->respond(503);
        ASSERT_EQ(1, m_scheduler.tasks.size());
        EXPECT_EQ(waits[i], m_scheduler.tasks[0].delay);
        m_scheduler.runAll();
    }
    m_network.last().pReply->respond(503);

    // Set aside with the unlike after it: the others go now
    EXPECT_TRUE(m_scheduler.tasks.isEmpty());
    ASSERT_EQ(6, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("/api/me/play-events"), m_network.last().request.url().path());
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "id": 1})"));
    ASSERT_EQ(7, m_network.sent.size());
    EXPECT_EQ(likePayload(QStringLiteral("b")), bodyOf(m_network.last()));
    EXPECT_EQ(QNetworkAccessManager::PostOperation, m_network.last().operation);
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));

    // Its turn again, with a fresh backoff, and the unlike still after it
    ASSERT_EQ(8, m_network.sent.size());
    EXPECT_EQ(likePayload(QStringLiteral("a")), bodyOf(m_network.last()));
    EXPECT_EQ(QNetworkAccessManager::PostOperation, m_network.last().operation);
    m_network.last().pReply->respond(503);
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(1s, m_scheduler.tasks[0].delay);
    m_scheduler.runAll();
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    ASSERT_EQ(10, m_network.sent.size());
    EXPECT_EQ(QNetworkAccessManager::CustomOperation, m_network.last().operation);
    EXPECT_EQ(likePayload(QStringLiteral("a")), bodyOf(m_network.last()));
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));

    EXPECT_EQ(0, pOutbox->pendingCount());
    EXPECT_TRUE(dropped.isEmpty());
}

TEST_F(OutboxTest, KeepsTheOrderWhenTheNetworkFails) {
    Outbox* pOutbox = makeOutbox();
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(QStringLiteral("a")));
    pOutbox->enqueue(kServer, outboxkind::kLike, likePayload(QStringLiteral("b")));

    // No network is no answer about the write: nothing goes ahead of it
    for (int i = 0; i < 5; ++i) {
        m_network.last().pReply->failNetwork();
        m_scheduler.runAll();
    }
    m_network.last().pReply->failNetwork();
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(30min, m_scheduler.tasks[0].delay);
    for (const auto& sent : std::as_const(m_network.sent)) {
        EXPECT_EQ(likePayload(QStringLiteral("a")), bodyOf(sent));
    }
    EXPECT_EQ(2, pOutbox->pendingCount());
}

TEST_F(OutboxTest, KeepsWaitingWhenNothingElseIsPending) {
    Outbox* pOutbox = makeOutbox();
    pOutbox->enqueue(kServer,
            outboxkind::kCueSet,
            QJsonObject{{QStringLiteral("entity_uid"), kUid},
                    {QStringLiteral("body"),
                            QJsonObject{
                                    {QStringLiteral("cues"), QJsonArray()}}}});
    for (int i = 0; i < 4; ++i) {
        m_network.last().pReply->respond(500);
        m_scheduler.runAll();
    }
    m_network.last().pReply->respond(500);
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(30min, m_scheduler.tasks[0].delay);
    EXPECT_EQ(1, pOutbox->pendingCount());
}

TEST_F(OutboxTest, SendsAnEntryEnqueuedBeforeStartOnce) {
    Outbox* pOutbox = makeOutbox(false);
    pOutbox->enqueue(kServer, outboxkind::kPlayEvent, playEvent(QStringLiteral("evt-1")));
    EXPECT_TRUE(m_network.sent.isEmpty());

    // Its row is stored before start() reads the table
    pOutbox->start();
    ASSERT_TRUE(QTest::qWaitFor([pOutbox] {
        return pOutbox->isLoaded();
    }));

    EXPECT_EQ(1, pOutbox->pendingCount());
    ASSERT_EQ(1, m_network.sent.size());
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "id": 1})"));
    EXPECT_EQ(1, m_network.sent.size());
    EXPECT_EQ(0, pOutbox->pendingCount());
    EXPECT_EQ(0, storedRows());
}

} // namespace
