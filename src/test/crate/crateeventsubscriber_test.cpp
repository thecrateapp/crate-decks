#include "crate/library/crateeventsubscriber.h"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QSqlQuery>
#include <QTest>
#include <memory>

#include "crate/db/crateserverschema.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarystore.h"
#include "crate/library/cratelibrarysync.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::library::EventSubscriber;
using crate::library::Invalidation;
using crate::library::LibrarySync;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");

class CrateEventSubscriberTest : public MixxxDbTest {
  protected:
    CrateEventSubscriberTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection()));
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(options));
        m_pClient->setTokenSource(&m_tokens);
        m_pSync = std::make_unique<LibrarySync>(kServer, m_pClient.get(), dbConnectionPooler());
        EventSubscriber::Options subscriberOptions;
        subscriberOptions.schedule = m_scheduler.function();
        m_pSubscriber = std::make_unique<EventSubscriber>(
                m_pClient.get(), m_pSync.get(), std::move(subscriberOptions));
    }

    void TearDown() override {
        m_pSubscriber.reset();
        m_pSync.reset();
        m_pClient.reset();
    }

    void storeTree() {
        const auto tree = crate::library::parseContainerTree(QByteArrayLiteral(R"({"containers": [
            {"id": "7", "kind": "playlist", "name": "Warm-up", "etag": "p7"},
            {"id": "8", "kind": "playlist", "name": "Sets", "etag": "p8"},
            {"id": "me", "kind": "likes", "name": "Likes", "etag": "l1"}]})"));
        ASSERT_TRUE(tree.ok());
        ASSERT_EQ(crate::library::WriteStatus::Ok,
                crate::library::storeContainerTree(dbConnection(), kServer, tree.value()));
        // As if their tracks were synced
        QSqlQuery synced(dbConnection());
        ASSERT_TRUE(synced.exec(QStringLiteral(
                "UPDATE crateserver_containers SET synced_etag = etag, "
                "fetched_at = '2026-11-09T10:00:00Z'")));
    }

    bool isDirty(const QString& kind, const QString& remoteId) const {
        QSqlQuery query(dbConnection());
        query.prepare(QStringLiteral(
                "SELECT dirty FROM crateserver_containers WHERE kind = :kind "
                "AND remote_id = :remote"));
        query.bindValue(QStringLiteral(":kind"), kind);
        query.bindValue(QStringLiteral(":remote"), remoteId);
        return query.exec() && query.next() && query.value(0).toBool();
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    FakeScheduler m_scheduler;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<LibrarySync> m_pSync;
    std::unique_ptr<EventSubscriber> m_pSubscriber;
};

TEST_F(CrateEventSubscriberTest, ReconnectsWithLastEventId) {
    m_pSubscriber->start();
    ASSERT_EQ(1, m_network.sent.size());
    const QNetworkRequest first = m_network.last().request;
    EXPECT_EQ(QStringLiteral("/api/cache/events"), first.url().path());
    EXPECT_EQ(QByteArrayLiteral("text/event-stream"), first.rawHeader("Accept"));
    EXPECT_FALSE(first.hasRawHeader("Last-Event-ID"));

    // A heartbeat, a comment and an event, then the connection drops
    m_network.last().pReply->respond(200,
            QByteArrayLiteral("event: heartbeat\ndata: {\"ts\": 1}\n\n: heartbeat\n\n"
                              "id: 41\r\ndata: home\r\n\r\n"));
    EXPECT_EQ(QStringLiteral("41"), m_pSubscriber->lastEventId());
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(std::chrono::milliseconds(1000), m_scheduler.tasks.first().delay);
    EXPECT_EQ(1, m_network.sent.size());

    m_scheduler.runAll();
    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_EQ(QByteArrayLiteral("41"), m_network.last().request.rawHeader("Last-Event-ID"));

    // Failures back off, up to the maximum
    m_network.last().pReply->failNetwork();
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(std::chrono::milliseconds(2000), m_scheduler.tasks.first().delay);
    m_scheduler.runAll();
    m_network.last().pReply->failNetwork();
    EXPECT_EQ(std::chrono::milliseconds(4000), m_scheduler.tasks.first().delay);

    // A reconnection scheduled before stop() does nothing
    m_pSubscriber->stop();
    m_scheduler.runAll();
    EXPECT_EQ(3, m_network.sent.size());
}

TEST_F(CrateEventSubscriberTest, MarksAffectedContainersDirty) {
    storeTree();
    QSignalSpy dirty(m_pSync.get(), &LibrarySync::containersDirty);
    QSignalSpy events(m_pSubscriber.get(), &EventSubscriber::eventReceived);
    m_pSubscriber->start();

    m_network.last().pReply->respond(200,
            QByteArrayLiteral("id: 1\ndata: playlist:7\n\nid: 2\ndata: likes\n\n"
                              "id: 3\ndata: home\n\n"));
    ASSERT_EQ(3, events.size());
    ASSERT_TRUE(QTest::qWaitFor([&] { return dirty.size() >= 2; }, 5000));

    EXPECT_TRUE(isDirty(QStringLiteral("playlist"), QStringLiteral("7")));
    EXPECT_TRUE(isDirty(QStringLiteral("likes"), QStringLiteral("me")));
    EXPECT_FALSE(isDirty(QStringLiteral("playlist"), QStringLiteral("8")));
    // Nothing fetches the tree for these
    EXPECT_EQ(1, m_network.sent.size());
}

TEST(CrateEventSubscriberScopeTest, MapsServerScopes) {
    using Scope = Invalidation::Scope;
    const auto playlist = EventSubscriber::invalidationOf(QStringLiteral("playlist:42"));
    ASSERT_TRUE(playlist.has_value());
    EXPECT_EQ(Scope::Container, playlist->scope);
    EXPECT_EQ(QStringLiteral("playlist"), playlist->kind);
    EXPECT_EQ(QStringLiteral("42"), playlist->remoteId);
    const auto crate = EventSubscriber::invalidationOf(
            QStringLiteral("crate:8c3c0f8e-1b9e-4f2e-9d55-2f7a1b0c4d6e"));
    ASSERT_TRUE(crate.has_value());
    EXPECT_EQ(QStringLiteral("crate"), crate->kind);
    EXPECT_EQ(QStringLiteral("8c3c0f8e-1b9e-4f2e-9d55-2f7a1b0c4d6e"), crate->remoteId);
    EXPECT_EQ(Scope::Tree, EventSubscriber::invalidationOf(QStringLiteral("playlists"))->scope);
    EXPECT_EQ(Scope::Tree, EventSubscriber::invalidationOf(QStringLiteral("crates"))->scope);
    EXPECT_EQ(Scope::Kind, EventSubscriber::invalidationOf(QStringLiteral("likes"))->scope);
    EXPECT_EQ(QStringLiteral("recent"),
            EventSubscriber::invalidationOf(QStringLiteral("history"))->kind);
    EXPECT_EQ(Scope::Everything, EventSubscriber::invalidationOf(QStringLiteral("library"))->scope);
    EXPECT_EQ(Scope::Everything, EventSubscriber::invalidationOf(QStringLiteral("*"))->scope);
    EXPECT_FALSE(EventSubscriber::invalidationOf(QStringLiteral("home")).has_value());
    EXPECT_FALSE(EventSubscriber::invalidationOf(QStringLiteral("album:3")).has_value());
}

} // namespace
