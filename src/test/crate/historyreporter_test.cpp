#include "crate/writeback/historyreporter.h"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QUuid>
#include <memory>

#include "crate/client/crateclient.h"
#include "crate/db/crateserverschema.h"
#include "crate/writeback/outbox.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"
#include "track/track.h"

namespace {

using namespace std::chrono_literals;
using crate::Client;
using crate::ClientOptions;
using crate::cache::TrackKey;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;
using crate::writeback::HistoryReporter;
using crate::writeback::Outbox;

const QString kServer = QStringLiteral("srv-1");
const QString kUidA = QStringLiteral("0b6f2c1e-5d6a-4c1b-9a51-2f0c8a3c9e10");
const QString kUidB = QStringLiteral("7d1e9b44-3f2a-4e8e-8c0d-61b5a2f4d7aa");
const QString kPlayEventsPath = QStringLiteral("/api/me/play-events");
const QString kNowPlayingPath = QStringLiteral("/api/me/now-playing");

QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
    return QJsonDocument::fromJson(sent.body).object();
}

class HistoryReporterTest : public MixxxDbTest {
  protected:
    HistoryReporterTest()
            : MixxxDbTest(true),
              m_now(QDateTime(QDate(2026, 11, 16), QTime(22, 0), QTimeZone::UTC)) {
    }

    void SetUp() override {
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ClientOptions clientOptions;
        clientOptions.deviceFingerprint = QStringLiteral("install-1");
        clientOptions.maxAttempts = 1;
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(clientOptions));
        m_pClient->setTokenSource(&m_tokens);

        Outbox::Options outboxOptions;
        outboxOptions.pDbConnectionPool = dbConnectionPooler();
        outboxOptions.clientFor = [this](const QString&) {
            return m_pClient.get();
        };
        outboxOptions.schedule = m_outboxScheduler.function();
        outboxOptions.now = [this] {
            return m_now;
        };
        m_pOutbox = std::make_unique<Outbox>(std::move(outboxOptions));
        m_pOutbox->start();
        ASSERT_TRUE(QTest::qWaitFor([this] {
            return m_pOutbox->isLoaded();
        }));

        HistoryReporter::Options options;
        options.pOutbox = m_pOutbox.get();
        options.trackOfFile = [](const QString& location) -> std::optional<TrackKey> {
            const QString prefix = QStringLiteral("/cache/") + kServer + QLatin1Char('/');
            if (!location.startsWith(prefix)) {
                return std::nullopt;
            }
            return TrackKey{kServer, location.mid(prefix.size()).section(QLatin1Char('.'), 0, 0)};
        };
        options.clientFor = [this](const QString&) {
            return m_pClient.get();
        };
        options.schedule = m_scheduler.function();
        options.now = [this] {
            return m_now;
        };
        options.mixSessionId = QStringLiteral("mix-1");
        m_pReporter = std::make_unique<HistoryReporter>(std::move(options));
    }

    void TearDown() override {
        m_pReporter.reset();
        m_pOutbox.reset();
        m_pClient.reset();
    }

    static TrackPointer crateTrack(const QString& uid, const QString& title) {
        TrackPointer pTrack = Track::newTemporary(
                QStringLiteral("/cache/") + kServer + QLatin1Char('/') + uid + ".flac");
        pTrack->setTitle(title);
        pTrack->setArtist(QStringLiteral("Aphex Twin"));
        pTrack->setAlbum(QStringLiteral("Windowlicker"));
        pTrack->setDuration(360.0);
        return pTrack;
    }

    void advance(std::chrono::seconds seconds) {
        m_now = m_now.addSecs(seconds.count());
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
    FakeScheduler m_outboxScheduler;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<Outbox> m_pOutbox;
    std::unique_ptr<HistoryReporter> m_pReporter;
    QDateTime m_now;
};

TEST_F(HistoryReporterTest, EmitsPlayEventOnTrackChange) {
    const TrackPointer pA = crateTrack(kUidA, QStringLiteral("Windowlicker"));
    const TrackPointer pB = crateTrack(kUidB, QStringLiteral("Flim"));
    m_pReporter->setPlayingDeck(1);
    m_pReporter->setPlayingTrack(pA);
    advance(100s);
    // Nothing on air for a while: it does not count, and it does not end A
    m_pReporter->setPlayingTrack(TrackPointer());
    advance(20s);
    m_pReporter->setPlayingTrack(pA);
    advance(50s);
    EXPECT_TRUE(sentTo(kPlayEventsPath).isEmpty());

    m_pReporter->setPlayingTrack(pB);

    const auto events = sentTo(kPlayEventsPath);
    ASSERT_EQ(1, events.size());
    const QJsonObject event = bodyOf(events[0]);
    EXPECT_EQ(kUidA, event.value(QStringLiteral("track_entity_uid")).toString());
    EXPECT_EQ(QStringLiteral("Windowlicker"), event.value(QStringLiteral("title")).toString());
    EXPECT_EQ(QStringLiteral("Aphex Twin"), event.value(QStringLiteral("artist")).toString());
    EXPECT_EQ(QStringLiteral("Windowlicker"), event.value(QStringLiteral("album")).toString());
    EXPECT_EQ(QStringLiteral("2026-11-16T22:00:00.000Z"),
            event.value(QStringLiteral("started_at")).toString());
    EXPECT_EQ(QStringLiteral("2026-11-16T22:02:50.000Z"),
            event.value(QStringLiteral("ended_at")).toString());
    EXPECT_DOUBLE_EQ(150.0, event.value(QStringLiteral("played_seconds")).toDouble());
    EXPECT_DOUBLE_EQ(360.0, event.value(QStringLiteral("track_duration_seconds")).toDouble());
    EXPECT_NEAR(150.0 / 360.0, event.value(QStringLiteral("completion_ratio")).toDouble(), 1e-9);
    EXPECT_EQ(QStringLiteral("decks"), event.value(QStringLiteral("device_type")).toString());
    EXPECT_EQ(QStringLiteral("decks"), event.value(QStringLiteral("app_platform")).toString());
    EXPECT_EQ(QStringLiteral("decks"), event.value(QStringLiteral("play_source_type")).toString());
    EXPECT_FALSE(event.contains(QStringLiteral("transition_source")));
    const QUuid clientEventId(event.value(QStringLiteral("client_event_id")).toString());
    EXPECT_FALSE(clientEventId.isNull());
    EXPECT_EQ(QUuid::Random, clientEventId.version());
}

TEST_F(HistoryReporterTest, SkipsBelowThreshold) {
    m_pReporter->setPlayingTrack(crateTrack(kUidA, QStringLiteral("Windowlicker")));
    advance(29s);
    m_pReporter->setPlayingTrack(crateTrack(kUidB, QStringLiteral("Flim")));
    advance(30s);
    m_pReporter->setPlayingTrack(crateTrack(kUidA, QStringLiteral("Windowlicker")));

    const auto events = sentTo(kPlayEventsPath);
    ASSERT_EQ(1, events.size());
    EXPECT_EQ(kUidB, bodyOf(events[0]).value(QStringLiteral("track_entity_uid")).toString());
}

TEST_F(HistoryReporterTest, IgnoresNonCrateTracks) {
    const TrackPointer pLocal = Track::newTemporary(QStringLiteral("/music/local.flac"));
    m_pReporter->setPlayingTrack(pLocal);
    advance(200s);
    m_pReporter->setPlayingTrack(crateTrack(kUidA, QStringLiteral("Windowlicker")));
    advance(10s);
    m_pReporter->setPlayingTrack(pLocal);
    m_scheduler.runAll();
    advance(200s);
    m_pReporter->finish();

    EXPECT_TRUE(sentTo(kPlayEventsPath).isEmpty());
    // Only the Crate track was on air for the server, and then it left
    const auto nowPlaying = sentTo(kNowPlayingPath);
    ASSERT_EQ(2, nowPlaying.size());
    EXPECT_EQ(kUidA, bodyOf(nowPlaying[0]).value(QStringLiteral("track_entity_uid")).toString());
    EXPECT_EQ((QJsonObject{{QStringLiteral("playing"), false}}), bodyOf(nowPlaying[1]));
}

TEST_F(HistoryReporterTest, NowPlayingEvery60s) {
    m_pReporter->setPlayingDeck(2);
    m_pReporter->setPlayingTrack(crateTrack(kUidA, QStringLiteral("Windowlicker")));

    auto nowPlaying = sentTo(kNowPlayingPath);
    ASSERT_EQ(1, nowPlaying.size());
    const QJsonObject first = bodyOf(nowPlaying[0]);
    EXPECT_TRUE(first.value(QStringLiteral("playing")).toBool());
    EXPECT_EQ(kUidA, first.value(QStringLiteral("track_entity_uid")).toString());
    EXPECT_EQ(QStringLiteral("Windowlicker"), first.value(QStringLiteral("title")).toString());
    EXPECT_EQ(3, first.value(QStringLiteral("deck")).toInt());
    EXPECT_EQ(QStringLiteral("mix-1"), first.value(QStringLiteral("mix_session_id")).toString());
    EXPECT_EQ(QStringLiteral("decks"), first.value(QStringLiteral("device_type")).toString());
    EXPECT_EQ(QStringLiteral("2026-11-16T22:00:00.000Z"),
            first.value(QStringLiteral("started_at")).toString());

    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(60s, m_scheduler.tasks[0].delay);
    advance(60s);
    m_scheduler.runAll();
    nowPlaying = sentTo(kNowPlayingPath);
    ASSERT_EQ(2, nowPlaying.size());
    EXPECT_EQ(kUidA, bodyOf(nowPlaying[1]).value(QStringLiteral("track_entity_uid")).toString());
    ASSERT_EQ(1, m_scheduler.tasks.size());

    // Off air: the state is cleared and the heartbeat stops
    m_pReporter->setPlayingDeck(-1);
    m_pReporter->setPlayingTrack(TrackPointer());
    nowPlaying = sentTo(kNowPlayingPath);
    ASSERT_EQ(3, nowPlaying.size());
    EXPECT_EQ((QJsonObject{{QStringLiteral("playing"), false}}), bodyOf(nowPlaying[2]));
    m_scheduler.runAll();
    EXPECT_EQ(3, sentTo(kNowPlayingPath).size());
    EXPECT_TRUE(m_scheduler.tasks.isEmpty());

    // A deck beyond the fourth is not sent
    for (auto* pReply : m_network.openReplies()) {
        pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    }
    m_pReporter->setPlayingDeck(5);
    m_pReporter->setPlayingTrack(crateTrack(kUidB, QStringLiteral("Flim")));
    nowPlaying = sentTo(kNowPlayingPath);
    ASSERT_EQ(4, nowPlaying.size());
    EXPECT_FALSE(bodyOf(nowPlaying[3]).contains(QStringLiteral("deck")));
}

TEST_F(HistoryReporterTest, UsesStableClientEventIdOnRetry) {
    m_pReporter->setPlayingTrack(crateTrack(kUidA, QStringLiteral("Windowlicker")));
    advance(45s);
    m_pReporter->setPlayingTrack(crateTrack(kUidB, QStringLiteral("Flim")));
    auto events = sentTo(kPlayEventsPath);
    ASSERT_EQ(1, events.size());
    const QString clientEventId =
            bodyOf(events[0]).value(QStringLiteral("client_event_id")).toString();
    ASSERT_FALSE(clientEventId.isEmpty());

    events[0].pReply->failNetwork();
    m_outboxScheduler.runAll();
    events = sentTo(kPlayEventsPath);
    ASSERT_EQ(2, events.size());
    EXPECT_EQ(clientEventId, bodyOf(events[1]).value(QStringLiteral("client_event_id")).toString());
    EXPECT_EQ(bodyOf(events[0]), bodyOf(events[1]));

    // Each play gets its own id
    events[1].pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "id": 1})"));
    advance(45s);
    m_pReporter->setPlayingTrack(crateTrack(kUidA, QStringLiteral("Windowlicker")));
    events = sentTo(kPlayEventsPath);
    ASSERT_EQ(3, events.size());
    EXPECT_NE(clientEventId, bodyOf(events[2]).value(QStringLiteral("client_event_id")).toString());
}

} // namespace
