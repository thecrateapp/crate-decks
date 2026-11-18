#include "crate/writeback/cratelikes.h"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTest>
#include <memory>

#include "crate/client/crateclient.h"
#include "crate/db/crateserverschema.h"
#include "crate/writeback/crateratings.h"
#include "crate/writeback/outbox.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"
#include "track/track.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::cache::TrackKey;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;
using crate::writeback::CrateLikes;
using crate::writeback::CrateRatings;
using crate::writeback::Outbox;

const QString kServer = QStringLiteral("srv-1");
const QString kUidA = QStringLiteral("0b6f2c1e-5d6a-4c1b-9a51-2f0c8a3c9e10");
const QString kUidB = QStringLiteral("7d1e9b44-3f2a-4e8e-8c0d-61b5a2f4d7aa");
const QString kLikesPath = QStringLiteral("/api/me/likes");
const QByteArray kOk = QByteArrayLiteral(R"({"ok": true})");

QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
    return QJsonDocument::fromJson(sent.body).object();
}

QString verbOf(const FakeNetworkAccessManager::Sent& sent) {
    if (sent.operation == QNetworkAccessManager::CustomOperation) {
        return QString::fromLatin1(
                sent.request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray());
    }
    return sent.operation == QNetworkAccessManager::PostOperation ? QStringLiteral("POST")
                                                                  : QStringLiteral("GET");
}

class CrateLikesTest : public MixxxDbTest {
  protected:
    CrateLikesTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ClientOptions clientOptions;
        clientOptions.deviceFingerprint = QStringLiteral("install-1");
        clientOptions.maxAttempts = 1;
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(clientOptions));
        m_pClient->setTokenSource(&m_tokens);
        Outbox::Options options;
        options.pDbConnectionPool = dbConnectionPooler();
        options.clientFor = [this](const QString&) {
            return m_pClient.get();
        };
        options.schedule = m_scheduler.function();
        m_pOutbox = std::make_unique<Outbox>(std::move(options));
        m_pOutbox->start();
        ASSERT_TRUE(QTest::qWaitFor([this] {
            return m_pOutbox->isLoaded();
        }));
    }

    void TearDown() override {
        m_pRatings.reset();
        m_pLikes.reset();
        m_pOutbox.reset();
        m_pClient.reset();
    }

    CrateLikes* makeLikes() {
        m_pLikes = std::make_unique<CrateLikes>(
                CrateLikes::Options{m_pOutbox.get(), dbConnectionPooler()});
        return m_pLikes.get();
    }

    CrateRatings* makeRatings() {
        CrateRatings::Options options;
        options.pOutbox = m_pOutbox.get();
        options.pDbConnectionPool = dbConnectionPooler();
        options.trackOfFile = [](const QString& location) -> std::optional<TrackKey> {
            const QString prefix = QStringLiteral("/cache/") + kServer + QLatin1Char('/');
            if (!location.startsWith(prefix)) {
                return std::nullopt;
            }
            return TrackKey{kServer, location.mid(prefix.size()).section(QLatin1Char('.'), 0, 0)};
        };
        options.trackById = [this](TrackId trackId) {
            return m_tracks.value(trackId);
        };
        m_pRatings = std::make_unique<CrateRatings>(std::move(options));
        m_pRatings->start();
        EXPECT_TRUE(QTest::qWaitFor([this] {
            return m_pRatings->isLoaded();
        }));
        return m_pRatings.get();
    }

    TrackPointer addTrack(int id, const QString& location) {
        const TrackPointer pTrack = Track::newDummy(location, TrackId(QVariant(id)));
        m_tracks.insert(TrackId(QVariant(id)), pTrack);
        return pTrack;
    }

    TrackPointer addCrateTrack(int id, const QString& uid) {
        return addTrack(id, QStringLiteral("/cache/") + kServer + QLatin1Char('/') + uid + ".flac");
    }

    /// The server's Likes container, as the library sync stores it.
    void storeLikes(const QStringList& uids) {
        QSqlQuery container(dbConnection());
        ASSERT_TRUE(container.exec(QStringLiteral(
                "INSERT OR IGNORE INTO crateserver_containers (id, server_id, kind, remote_id, "
                "name) VALUES (40, 'srv-1', 'likes', 'likes', 'Likes')")));
        QSqlQuery clear(dbConnection());
        ASSERT_TRUE(clear.exec(
                QStringLiteral("DELETE FROM crateserver_container_tracks WHERE "
                               "container_id = 40")));
        for (int i = 0; i < uids.size(); ++i) {
            QSqlQuery insert(dbConnection());
            insert.prepare(QStringLiteral(
                    "INSERT INTO crateserver_container_tracks (container_id, position, "
                    "entity_uid) VALUES (40, :position, :uid)"));
            insert.bindValue(QStringLiteral(":position"), i);
            insert.bindValue(QStringLiteral(":uid"), uids[i]);
            ASSERT_TRUE(insert.exec());
        }
    }

    void answerAll() {
        const QList<crate::test::FakeReply*> replies = m_network.openReplies();
        for (auto* pReply : std::as_const(replies)) {
            pReply->respond(200, kOk);
        }
    }

    FakeNetworkAccessManager m_network;
    FakeScheduler m_scheduler;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<Outbox> m_pOutbox;
    std::unique_ptr<CrateLikes> m_pLikes;
    std::unique_ptr<CrateRatings> m_pRatings;
    QHash<TrackId, TrackPointer> m_tracks;
};

TEST_F(CrateLikesTest, LikeTogglesAndQueues) {
    storeLikes({kUidA});
    CrateLikes* pLikes = makeLikes();
    QSignalSpy changed(pLikes, &CrateLikes::likesChanged);
    pLikes->reload(kServer);
    pLikes->waitForReads();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return !changed.isEmpty();
    }));
    EXPECT_TRUE(pLikes->isLiked(kServer, kUidA));
    EXPECT_TRUE(pLikes->isLiked(kServer, kUidA.toUpper()));
    EXPECT_FALSE(pLikes->isLiked(kServer, kUidB));

    // B is not liked yet: liking both sends only B's like
    const QList<TrackKey> both = {TrackKey{kServer, kUidA}, TrackKey{kServer, kUidB}};
    pLikes->toggle(both);
    EXPECT_TRUE(pLikes->isLiked(kServer, kUidB));
    EXPECT_TRUE(pLikes->allLiked(both));
    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("POST"), verbOf(m_network.last()));
    EXPECT_EQ(kLikesPath, m_network.last().request.url().path());
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_entity_uid"), kUidB}}),
            bodyOf(m_network.last()));
    answerAll();

    // Both liked: toggling unlikes both, in order
    pLikes->toggle(both);
    EXPECT_FALSE(pLikes->isLiked(kServer, kUidA));
    EXPECT_FALSE(pLikes->isLiked(kServer, kUidB));
    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("DELETE"), verbOf(m_network.last()));
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_entity_uid"), kUidA}}),
            bodyOf(m_network.last()));
    answerAll();
    ASSERT_EQ(3, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("DELETE"), verbOf(m_network.last()));
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_entity_uid"), kUidB}}),
            bodyOf(m_network.last()));
    answerAll();

    // A reload of the stale container does not undo what the user did
    pLikes->reload(kServer);
    pLikes->waitForReads();
    QTest::qWait(10);
    EXPECT_FALSE(pLikes->isLiked(kServer, kUidA));
    // Once synced, the server says the same
    storeLikes({});
    pLikes->reload(kServer);
    pLikes->waitForReads();
    QTest::qWait(10);
    EXPECT_FALSE(pLikes->isLiked(kServer, kUidA));
    storeLikes({kUidA});
    pLikes->reload(kServer);
    pLikes->waitForReads();
    QTest::qWait(10);
    EXPECT_TRUE(pLikes->isLiked(kServer, kUidA));
    EXPECT_GE(changed.size(), 3);
}

TEST_F(CrateLikesTest, RatingChangeQueuesRate) {
    const TrackPointer pTrack = addCrateTrack(1, kUidA);
    CrateRatings* pRatings = makeRatings();

    // Unrated in Decks: nothing to send
    pRatings->onTracksChanged({TrackId(QVariant(1))});
    EXPECT_TRUE(m_network.sent.isEmpty());

    pTrack->setRating(4);
    pRatings->onTracksChanged({TrackId(QVariant(1))});
    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("/api/tracks/by-entity/%1/info").arg(kUidA),
            m_network.last().request.url().path());
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"id": 88})"));
    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("/api/track/rate"), m_network.last().request.url().path());
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_id"), 88}, {QStringLiteral("rating"), 4}}),
            bodyOf(m_network.last()));
    answerAll();

    // Cleared after it was sent: the 0 goes too
    pTrack->setRating(0);
    pRatings->onTracksChanged({TrackId(QVariant(1))});
    ASSERT_EQ(3, m_network.sent.size());
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"id": 88})"));
    ASSERT_EQ(4, m_network.sent.size());
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_id"), 88}, {QStringLiteral("rating"), 0}}),
            bodyOf(m_network.last()));
    answerAll();

    // The last rating sent survives a restart
    pTrack->setRating(3);
    pRatings->onTracksChanged({TrackId(QVariant(1))});
    EXPECT_EQ(5, m_network.sent.size());
    pRatings->waitForStorage();
    m_pRatings.reset();
    pRatings = makeRatings();
    pRatings->onTracksChanged({TrackId(QVariant(1))});
    EXPECT_EQ(5, m_network.sent.size());
}

TEST_F(CrateLikesTest, NonRatingChangeDoesNotQueue) {
    const TrackPointer pTrack = addCrateTrack(1, kUidA);
    const TrackPointer pLocal = addTrack(2, QStringLiteral("/music/local.flac"));
    CrateRatings* pRatings = makeRatings();
    pTrack->setRating(5);
    pRatings->onTracksChanged({TrackId(QVariant(1))});
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"id": 88})"));
    answerAll();
    const int sent = static_cast<int>(m_network.sent.size());

    // Another field of the same track, a track not in memory and a track
    // that is not Crate's
    pTrack->trySetBpm(128.0);
    pTrack->setComment(QStringLiteral("peak time"));
    pLocal->setRating(5);
    pRatings->onTracksChanged({TrackId(QVariant(1)), TrackId(QVariant(2)), TrackId(QVariant(3))});

    EXPECT_EQ(sent, m_network.sent.size());
    EXPECT_EQ(0, m_pOutbox->pendingCount());
}

} // namespace
