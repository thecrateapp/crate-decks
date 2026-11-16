#include "crate/recs/crateradiosource.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <memory>

#include "crate/recs/recommendationqueue.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxtest.h"

// Crate's Radio as a source of Auto DJ (plan F5.C-PR2): it keeps five of its
// tracks in Auto DJ's queue, sends the DJ's likes and dislikes, and stops
// when the DJ clears the queue.

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::cache::TrackKey;
using crate::recs::CrateRadioSource;
using crate::recs::RadioSeed;
using crate::recs::RecommendedTrack;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");
const QString kSession = QStringLiteral("a564d2dd-0cdf-472c-8140-0e09b4fc6d6c");
/// The seed of the real radio: "I Feel Loved (Danny Tenaglia's Labor Of Love
/// Dub Edit)", from the dev stack.
const QString kSeed = QStringLiteral("afe794ee-23ea-53d3-8280-ff633310da60");

/// Auto DJ's queue: what the radio appends, and what the test says is
/// queued when the radio reads it.
class FakeQueue : public crate::recs::RecommendationQueue {
  public:
    QList<TrackKey> queued;
    QList<QList<RecommendedTrack>> appended;
    int reads = 0;

    void enqueue(const QString& serverId, const QList<RecommendedTrack>& tracks) override {
        appended.append(tracks);
        for (const RecommendedTrack& track : tracks) {
            queued.append(TrackKey{serverId, track.entityUid});
        }
        emit queueChanged();
    }

    void readQueue(std::function<void(const QList<TrackKey>&)> done) override {
        ++reads;
        // Later, as the application's read off the main thread
        QMetaObject::invokeMethod(
                this,
                [this, done] {
                    done(queued);
                },
                Qt::QueuedConnection);
    }

    QString saveAsPlaylist(const QString& name,
            const QString& serverId,
            const QList<RecommendedTrack>& tracks) override {
        Q_UNUSED(serverId);
        Q_UNUSED(tracks);
        return name;
    }

    /// Auto DJ takes the first track into a deck.
    void takeFirst() {
        queued.removeFirst();
        emit queueChanged();
    }

    void clear() {
        queued.clear();
        emit queueChanged();
    }

    int appendedTracks() const {
        int count = 0;
        for (const auto& tracks : appended) {
            count += static_cast<int>(tracks.size());
        }
        return count;
    }
};

class CrateRadioSourceTest : public MixxxTest {
  protected:
    void SetUp() override {
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(options));
        m_pClient->setTokenSource(&m_tokens);
        CrateRadioSource::Options radio;
        radio.newSessionId = [] {
            return kSession;
        };
        radio.schedule = m_scheduler.function();
        m_pRadio = std::make_unique<CrateRadioSource>(&m_queue, std::move(radio));
    }

    void TearDown() override {
        m_pRadio.reset();
        m_pClient.reset();
    }

    QByteArray fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/recs/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return file.readAll();
    }

    bool waitForRequests(int count) {
        return QTest::qWaitFor(
                [&] {
                    return m_network.sent.size() >= count;
                },
                2000);
    }

    static QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
        return QJsonDocument::fromJson(sent.body).object();
    }

    /// A batch of `next` with `count` new tracks.
    static QByteArray nextBatch(int batchIndex, int count) {
        QJsonArray tracks;
        for (int i = 0; i < count; ++i) {
            tracks.append(QJsonObject{
                    {QStringLiteral("track_id"), 90000 + batchIndex * 10 + i},
                    {QStringLiteral("track_entity_uid"), QJsonValue::Null},
                    {QStringLiteral("entity_uid"),
                            QStringLiteral("00000000-0000-4000-8000-0000000%1%2")
                                    .arg(batchIndex, 3, 10, QLatin1Char('0'))
                                    .arg(i, 2, 10, QLatin1Char('0'))},
                    {QStringLiteral("title"), QStringLiteral("Next %1").arg(i)},
                    {QStringLiteral("format"), QStringLiteral("flac")},
                    {QStringLiteral("duration"), 300.5},
            });
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("session_id"), kSession},
                                     {QStringLiteral("tracks"), tracks},
                                     {QStringLiteral("mode"), QJsonValue::Null},
                                     {QStringLiteral("seed_label"), QJsonValue::Null},
                                     {QStringLiteral("batch_index"), batchIndex},
                                     {QStringLiteral("status"), QStringLiteral("ok")},
                                     {QStringLiteral("exhausted"), false}})
                .toJson();
    }

    /// Starts the radio from the real seed and answers with the real batch
    /// of five.
    void startRadio() {
        m_pRadio->start(m_pClient.get(), kServer, RadioSeed::track(kSeed, QStringLiteral("Seed")));
        ASSERT_TRUE(waitForRequests(1));
        m_network.last().pReply->respond(200, fixture(QStringLiteral("real-radio-start.json")));
        ASSERT_TRUE(QTest::qWaitFor(
                [&] {
                    return m_queue.appendedTracks() == 5;
                },
                2000));
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    FakeScheduler m_scheduler;
    FakeQueue m_queue;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<CrateRadioSource> m_pRadio;
};

TEST_F(CrateRadioSourceTest, KeepsFiveAhead) {
    startRadio();
    // The indexed start: our session id, five tracks
    const auto start = m_network.sent.first();
    EXPECT_EQ(QStringLiteral("/api/radio/start"), start.request.url().path());
    const QJsonObject body = bodyOf(start);
    EXPECT_QSTRING_EQ(QStringLiteral("seeded"), body.value(QStringLiteral("mode")).toString());
    EXPECT_QSTRING_EQ(QStringLiteral("track"), body.value(QStringLiteral("seed_type")).toString());
    EXPECT_QSTRING_EQ(kSeed, body.value(QStringLiteral("seed_value")).toString());
    EXPECT_QSTRING_EQ(kSession, body.value(QStringLiteral("session_id")).toString());
    EXPECT_EQ(5, body.value(QStringLiteral("count")).toInt());
    // In the server's order, with what queueing them needs
    ASSERT_EQ(1, m_queue.appended.size());
    const RecommendedTrack& first = m_queue.appended.first().first();
    EXPECT_QSTRING_EQ(QStringLiteral("9e8c5fc0-0728-50b5-8237-cbb24efce49e"), first.entityUid);
    EXPECT_EQ(22248, first.trackId);
    EXPECT_QSTRING_EQ(QStringLiteral("Blue Monday (2023 Digital Master)"), first.title);
    EXPECT_QSTRING_EQ(QStringLiteral("flac"), first.format);
    EXPECT_EQ(449160, first.durationMs.value_or(0));
    EXPECT_TRUE(m_pRadio->isRunning());
    EXPECT_QSTRING_EQ(QStringLiteral(
                              "I Feel Loved (Danny Tenaglia's Labor Of Love Dub Edit) — Depeche "
                              "Mode"),
            m_pRadio->label());

    // Five ahead: nothing more to ask
    QTest::qWait(50);
    EXPECT_EQ(1, m_network.sent.size());

    // Auto DJ plays two: the radio asks for two, with the three still queued
    m_queue.takeFirst();
    m_queue.takeFirst();
    ASSERT_TRUE(waitForRequests(2));
    const auto next = m_network.last();
    EXPECT_EQ(QStringLiteral("/api/radio/next"), next.request.url().path());
    const QJsonObject nextBody = bodyOf(next);
    EXPECT_QSTRING_EQ(kSession, nextBody.value(QStringLiteral("session_id")).toString());
    EXPECT_EQ(2, nextBody.value(QStringLiteral("count")).toInt());
    EXPECT_EQ(1, nextBody.value(QStringLiteral("batch_index")).toInt());
    const QJsonArray refs = nextBody.value(QStringLiteral("buffer_refs")).toArray();
    ASSERT_EQ(3, refs.size());
    EXPECT_QSTRING_EQ(m_queue.queued.first().entityUid, refs.first().toString());
    // Only one request while the queue changes again
    m_queue.takeFirst();
    QTest::qWait(50);
    EXPECT_EQ(2, m_network.sent.size());

    // The real second batch: back to five, and one more for the track
    // Auto DJ took meanwhile
    m_network.last().pReply->respond(200, fixture(QStringLiteral("real-radio-next.json")));
    ASSERT_TRUE(waitForRequests(3));
    EXPECT_EQ(7, m_queue.appendedTracks());
    const QJsonObject thirdBody = bodyOf(m_network.last());
    EXPECT_EQ(1, thirdBody.value(QStringLiteral("count")).toInt());
    EXPECT_EQ(2, thirdBody.value(QStringLiteral("batch_index")).toInt());
    m_network.last().pReply->respond(200, nextBatch(2, 1));
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                return m_queue.appendedTracks() == 8;
            },
            2000));
    EXPECT_EQ(5, m_queue.queued.size());
    QTest::qWait(50);
    EXPECT_EQ(3, m_network.sent.size());
    EXPECT_EQ(8, m_pRadio->tracks().size());
}

TEST_F(CrateRadioSourceTest, AsksAgainWhenTheServerIsBusy) {
    startRadio();
    m_queue.takeFirst();
    ASSERT_TRUE(waitForRequests(2));
    // The session is busy with another request: the same batch, a second
    // later
    m_network.last().pReply->respond(409,
            QByteArrayLiteral(R"({"detail":"Radio session is busy; retry the request"})"),
            {{QByteArrayLiteral("Retry-After"), QByteArrayLiteral("1")}});
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                return !m_scheduler.tasks.isEmpty();
            },
            2000));
    EXPECT_EQ(2, m_network.sent.size());
    m_scheduler.runAll();
    ASSERT_TRUE(waitForRequests(3));
    EXPECT_EQ(1, bodyOf(m_network.last()).value(QStringLiteral("batch_index")).toInt());
}

TEST_F(CrateRadioSourceTest, SendsFeedback) {
    startRadio();
    const RecommendedTrack liked = m_queue.appended.first()[0];
    const RecommendedTrack disliked = m_queue.appended.first()[1];
    EXPECT_TRUE(m_pRadio->hasTrack(kServer, liked.entityUid.toUpper()));

    ASSERT_TRUE(m_pRadio->sendFeedback(liked.entityUid, CrateRadioSource::Feedback::Like));
    ASSERT_TRUE(waitForRequests(2));
    auto sent = m_network.last();
    EXPECT_EQ(QStringLiteral("/api/radio/feedback"), sent.request.url().path());
    QJsonObject body = bodyOf(sent);
    EXPECT_QSTRING_EQ(kSession, body.value(QStringLiteral("session_id")).toString());
    EXPECT_EQ(liked.trackId, body.value(QStringLiteral("track_id")).toInteger());
    EXPECT_QSTRING_EQ(QStringLiteral("like"), body.value(QStringLiteral("action")).toString());
    sent.pReply->respond(200, fixture(QStringLiteral("real-radio-feedback.json")));

    ASSERT_TRUE(m_pRadio->sendFeedback(disliked.entityUid, CrateRadioSource::Feedback::Dislike));
    ASSERT_TRUE(waitForRequests(3));
    body = bodyOf(m_network.last());
    EXPECT_EQ(disliked.trackId, body.value(QStringLiteral("track_id")).toInteger());
    EXPECT_QSTRING_EQ(QStringLiteral("dislike"), body.value(QStringLiteral("action")).toString());

    // A track the radio did not queue has no say
    EXPECT_FALSE(m_pRadio->sendFeedback(kSeed, CrateRadioSource::Feedback::Like));
    QTest::qWait(20);
    EXPECT_EQ(3, m_network.sent.size());
}

TEST_F(CrateRadioSourceTest, StopsWhenUserClearsQueue) {
    startRadio();
    QSignalSpy stopped(m_pRadio.get(), &CrateRadioSource::stopped);
    m_queue.clear();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !stopped.isEmpty(); }, 2000));
    EXPECT_EQ(CrateRadioSource::StopReason::QueueCleared,
            stopped.first().first().value<CrateRadioSource::StopReason>());
    EXPECT_FALSE(m_pRadio->isRunning());
    // The server ends the session, and nothing more is queued
    ASSERT_TRUE(waitForRequests(2));
    EXPECT_EQ(QNetworkAccessManager::DeleteOperation, m_network.last().operation);
    EXPECT_EQ(QStringLiteral("/api/radio/session/") + kSession,
            m_network.last().request.url().path());
    m_network.last().pReply->respond(200, QByteArrayLiteral(R"({"status":"ended"})"));
    QTest::qWait(50);
    EXPECT_EQ(2, m_network.sent.size());
    EXPECT_EQ(5, m_queue.appendedTracks());
    EXPECT_FALSE(m_pRadio->sendFeedback(
            m_queue.appended.first().first().entityUid, CrateRadioSource::Feedback::Like));
}

TEST_F(CrateRadioSourceTest, StopsWhenTheServerRunsOut) {
    startRadio();
    QSignalSpy stopped(m_pRadio.get(), &CrateRadioSource::stopped);
    m_queue.takeFirst();
    ASSERT_TRUE(waitForRequests(2));
    QJsonObject exhausted = QJsonDocument::fromJson(nextBatch(1, 0)).object();
    exhausted.insert(QStringLiteral("status"), QStringLiteral("exhausted"));
    exhausted.insert(QStringLiteral("exhausted"), true);
    m_network.last().pReply->respond(200, QJsonDocument(exhausted).toJson());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !stopped.isEmpty(); }, 2000));
    EXPECT_EQ(CrateRadioSource::StopReason::Exhausted,
            stopped.first().first().value<CrateRadioSource::StopReason>());
    // Its tracks stay queued
    EXPECT_EQ(4, m_queue.queued.size());
}

TEST_F(CrateRadioSourceTest, SeedsFromPlaylistsAndCrates) {
    const auto playlist = RadioSeed::container(
            QStringLiteral("playlist"), QStringLiteral("42"), QStringLiteral("Warm-up"));
    ASSERT_TRUE(playlist);
    EXPECT_QSTRING_EQ(QStringLiteral("playlist"), playlist->type);
    EXPECT_QSTRING_EQ(QStringLiteral("42"), playlist->value);
    const auto crate = RadioSeed::container(QStringLiteral("crate"),
            QStringLiteral("0b6c3e2a-7f4d-4d8e-9a51-1c2b3d4e5f60"),
            QStringLiteral("Peak"));
    ASSERT_TRUE(crate);
    EXPECT_QSTRING_EQ(QStringLiteral("crate"), crate->type);
    // The server's genre seed is a slug, the container's id a number
    EXPECT_FALSE(RadioSeed::container(
            QStringLiteral("genre"), QStringLiteral("17"), QStringLiteral("Techno")));
    EXPECT_FALSE(RadioSeed::container(
            QStringLiteral("mood"), QStringLiteral("dark"), QStringLiteral("Dark")));
}

} // namespace
