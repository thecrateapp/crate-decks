#include "crate/writeback/cratecuesync.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTest>
#include <memory>

#include "crate/analysis/analysisimporter.h"
#include "crate/analysis/mixprofile.h"
#include "crate/client/crateclient.h"
#include "crate/db/crateserverschema.h"
#include "crate/writeback/outbox.h"
#include "sources/soundsourceproxy.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/cue.h"
#include "track/track.h"
#include "util/color/predefinedcolorpalettes.h"
#include "util/color/rgbcolor.h"

namespace {

using namespace std::chrono_literals;
using crate::Client;
using crate::ClientOptions;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;
using crate::writeback::CrateCueSync;
using crate::writeback::Outbox;
using crate::writeback::SyncedCue;
using crate::writeback::SyncedCues;
namespace cuetype = crate::writeback::cuetype;

const QString kServer = QStringLiteral("srv-1");
const QString kUid = QStringLiteral("0b6f2c1e-5d6a-4c1b-9a51-2f0c8a3c9e10");
const QString kUidB = QStringLiteral("7d1e9b44-3f2a-4e8e-8c0d-61b5a2f4d7aa");
const QString kUidC = QStringLiteral("5f0c6e9a-0d1b-4d55-9b3e-6f1d9c1a7e20");
const QString kInstallation = QStringLiteral("install-1");
const QString kOtherInstallation = QStringLiteral("install-2");
const QString kQueryPath = QStringLiteral("/api/decks/cues/query");
const QString kAudioFile = QStringLiteral("id3-test-data/cover-test-øé~ł€˚.flac");
constexpr int kSampleRate = 44100;

QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
    return QJsonDocument::fromJson(sent.body).object();
}

QString verbOf(const FakeNetworkAccessManager::Sent& sent) {
    switch (sent.operation) {
    case QNetworkAccessManager::PostOperation:
        return QStringLiteral("POST");
    case QNetworkAccessManager::PutOperation:
        return QStringLiteral("PUT");
    case QNetworkAccessManager::GetOperation:
        return QStringLiteral("GET");
    default:
        return QStringLiteral("OTHER");
    }
}

QJsonObject cue(const QString& type,
        int slot,
        QJsonValue startMs,
        QJsonValue endMs = QJsonValue::Null,
        QJsonValue label = QJsonValue::Null,
        QJsonValue color = QJsonValue::Null) {
    return QJsonObject{{QStringLiteral("cue_type"), type},
            {QStringLiteral("slot"), slot},
            {QStringLiteral("start_ms"), startMs},
            {QStringLiteral("end_ms"), endMs},
            {QStringLiteral("label"), label},
            {QStringLiteral("color"), color}};
}

QJsonObject cueSet(const QString& uid,
        qint64 revision,
        const QJsonArray& cues,
        const QString& clientId = kOtherInstallation,
        const QString& updatedAt = QStringLiteral("2026-11-16T18:02:11.418204Z")) {
    return QJsonObject{{QStringLiteral("entity_uid"), uid},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("updated_at"), updatedAt},
            {QStringLiteral("client_id"), clientId},
            {QStringLiteral("cues"), cues}};
}

/// The server's clock in the fakes' answers: the same as the test's local
/// clock unless a test says otherwise.
const QString kServerTime = QStringLiteral("2026-11-16T22:00:00.000000Z");

/// `server_time`, which every S7 answer carries; none for an empty one.
QJsonObject withServerTime(QJsonObject answer, const QString& serverTime = kServerTime) {
    if (!serverTime.isEmpty()) {
        answer.insert(QStringLiteral("server_time"), serverTime);
    }
    return answer;
}

QByteArray queryAnswer(const QList<QJsonObject>& sets,
        const QStringList& missing,
        const QString& serverTime = kServerTime) {
    QJsonObject cueSets;
    for (const QJsonObject& set : sets) {
        cueSets.insert(set.value(QStringLiteral("entity_uid")).toString(), set);
    }
    return QJsonDocument(withServerTime(QJsonObject{{QStringLiteral("cue_sets"), cueSets},
                                                {QStringLiteral("missing"),
                                                        QJsonArray::fromStringList(missing)}},
                                 serverTime))
            .toJson();
}

/// A 409 of a PUT.
QByteArray conflictAnswer(const QJsonObject& current, const QString& serverTime = kServerTime) {
    return QJsonDocument(withServerTime(QJsonObject{{QStringLiteral("detail"),
                                                            QStringLiteral(
                                                                    "Cue set revision conflict")},
                                                {QStringLiteral("current"), current}},
                                 serverTime))
            .toJson();
}

double framesOf(double ms) {
    return ms * kSampleRate / 1000.0;
}

class CrateCueSyncTest : public MixxxDbTest, SoundSourceProviderRegistration {
  protected:
    CrateCueSyncTest()
            : MixxxDbTest(true),
              m_now(QDateTime(QDate(2026, 11, 16), QTime(22, 0), QTimeZone::UTC)) {
    }

    void SetUp() override {
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ClientOptions clientOptions;
        clientOptions.deviceFingerprint = kInstallation;
        // The outbox does its own retries
        clientOptions.maxAttempts = 1;
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(clientOptions));
        m_pClient->setTokenSource(&m_tokens);
        Outbox::Options options;
        options.pDbConnectionPool = dbConnectionPooler();
        options.clientFor = [this](const QString&) {
            return m_pClient.get();
        };
        options.schedule = m_outboxScheduler.function();
        options.now = [this] {
            return m_now;
        };
        m_pOutbox = std::make_unique<Outbox>(std::move(options));
        m_pOutbox->start();
        ASSERT_TRUE(QTest::qWaitFor([this] {
            return m_pOutbox->isLoaded();
        }));
    }

    void TearDown() override {
        if (m_pSync) {
            m_pSync->waitForStorage();
        }
        m_pSync.reset();
        m_pOutbox.reset();
        m_pClient.reset();
    }

    CrateCueSync* makeSync() {
        CrateCueSync::Options options;
        options.pOutbox = m_pOutbox.get();
        options.pDbConnectionPool = dbConnectionPooler();
        options.clientFor = [this](const QString&) {
            return m_pClient.get();
        };
        options.enabledFor = [this](const QString&) {
            return m_cuesFeature;
        };
        options.clientId = kInstallation;
        options.schedule = m_scheduler.function();
        options.now = [this] {
            return m_now;
        };
        m_pSync = std::make_unique<CrateCueSync>(std::move(options));
        m_pSync->start();
        EXPECT_TRUE(QTest::qWaitFor([this] {
            return m_pSync->isLoaded();
        }));
        return m_pSync.get();
    }

    /// A track with a sample rate, as after its tags were read.
    static TrackPointer dummyTrack(const QString& name = QStringLiteral("track.flac")) {
        TrackPointer pTrack = Track::newTemporary(QStringLiteral("/cache/srv-1/") + name);
        pTrack->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
                mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::Bitrate(320),
                mixxx::Duration::fromSeconds(240));
        return pTrack;
    }

    /// A track whose file upstream has opened, as after a first load.
    TrackPointer openedTrack() const {
        const QString path = getTestDir().filePath(kAudioFile);
        EXPECT_TRUE(QFile::exists(path));
        TrackPointer pTrack = Track::newTemporary(path);
        SoundSourceProxy proxy(pTrack);
        EXPECT_TRUE(proxy.openAudioSource());
        return pTrack;
    }

    crate::analysis::MixProfile profile() const {
        QFile file(getTestDir().filePath(
                QStringLiteral("crate/data/mix-profiles/full-constant-124.json")));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        const auto profile = crate::analysis::parseMixProfile(file.readAll());
        EXPECT_TRUE(profile.ok());
        return profile.ok() ? profile.value() : crate::analysis::MixProfile();
    }

    QList<FakeNetworkAccessManager::Sent> sentTo(const QString& verb, const QString& path) const {
        QList<FakeNetworkAccessManager::Sent> matching;
        for (const auto& sent : m_network.sent) {
            if (verbOf(sent) == verb && sent.request.url().path() == path) {
                matching.append(sent);
            }
        }
        return matching;
    }

    QList<FakeNetworkAccessManager::Sent> puts(const QString& uid = kUid) const {
        return sentTo(QStringLiteral("PUT"), QStringLiteral("/api/decks/cues/") + uid);
    }

    /// Attaches the track and answers its query with `answer`.
    void attachAndAnswer(const TrackPointer& pTrack, const QString& uid, const QByteArray& answer) {
        const int queries = static_cast<int>(sentTo(QStringLiteral("POST"), kQueryPath).size());
        m_pSync->attach(pTrack, kServer, uid);
        m_scheduler.runAll();
        ASSERT_TRUE(QTest::qWaitFor([&] {
            return sentTo(QStringLiteral("POST"), kQueryPath).size() > queries;
        }));
        sentTo(QStringLiteral("POST"), kQueryPath).constLast().pReply->respond(200, answer);
        QCoreApplication::processEvents();
    }

    void waitForPuts(int count, const QString& uid = kUid) {
        EXPECT_TRUE(QTest::qWaitFor([&] {
            return puts(uid).size() >= count;
        })) << "PUTs sent: "
            << puts(uid).size();
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    FakeScheduler m_scheduler;
    FakeScheduler m_outboxScheduler;
    QDateTime m_now;
    bool m_cuesFeature = true;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<Outbox> m_pOutbox;
    std::unique_ptr<CrateCueSync> m_pSync;
};

TEST_F(CrateCueSyncTest, AppliesUserCuesOverProfileCues) {
    makeSync();
    const crate::analysis::MixProfile mixProfile = profile();
    // The profile's Intro and Outro are on the track, as when it was loaded
    const TrackPointer pTrack = openedTrack();
    const double perMs = pTrack->getSampleRate().toDouble() / 1000.0;
    const crate::analysis::ImportOutcome outcome =
            crate::analysis::AnalysisImporter::apply(pTrack, mixProfile, {});
    ASSERT_TRUE(outcome.appliedCues);
    m_pSync->absorbAutomaticChanges(pTrack);
    ASSERT_NE(nullptr, pTrack->findCueByType(mixxx::CueType::Intro));
    ASSERT_NE(nullptr, pTrack->findCueByType(mixxx::CueType::Outro));

    // The user's set: their own Intro and a hot cue, no Outro
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid,
                                3,
                                QJsonArray{cue(cuetype::kIntro, 0, 1000.0, 16000.0),
                                        cue(cuetype::kHotcue,
                                                0,
                                                1500.517,
                                                QJsonValue::Null,
                                                QStringLiteral("Drop"),
                                                QStringLiteral("#ff8800"))})},
                    {}));

    const CuePointer pIntro = pTrack->findCueByType(mixxx::CueType::Intro);
    ASSERT_NE(nullptr, pIntro);
    EXPECT_NEAR(1000.0 * perMs, pIntro->getPosition().value(), 0.01);
    EXPECT_NEAR(16000.0 * perMs, pIntro->getEndPosition().value(), 0.01);
    const CuePointer pHotcue = pTrack->findHotcueByIndex(0);
    ASSERT_NE(nullptr, pHotcue);
    EXPECT_NEAR(1500.517 * perMs, pHotcue->getPosition().value(), 0.01);
    EXPECT_EQ(QStringLiteral("Drop"), pHotcue->getLabel());
    EXPECT_EQ(mixxx::RgbColor(0xff8800), pHotcue->getColor());
    // The user's set is the track's whole set: it has no Outro, so the
    // profile's goes (Diego, 2026-11-16: deletions propagate)
    EXPECT_EQ(nullptr, pTrack->findCueByType(mixxx::CueType::Outro));
    EXPECT_EQ(3, m_pSync->revision(kServer, kUid));

    // The profile again, of a new revision: it puts back no Outro and does
    // not move the user's Intro
    crate::analysis::MixProfile newer = mixProfile;
    newer.profileRevision = QStringLiteral("a-newer-revision");
    crate::analysis::AnalysisImporter::apply(pTrack, newer, {}, outcome.appliedCues);
    m_pSync->absorbAutomaticChanges(pTrack);
    EXPECT_NEAR(1000.0 * perMs,
            pTrack->findCueByType(mixxx::CueType::Intro)->getPosition().value(),
            0.01);
    EXPECT_EQ(nullptr, pTrack->findCueByType(mixxx::CueType::Outro));

    // Nothing of it is an edit to send
    m_scheduler.runAll();
    QCoreApplication::processEvents();
    EXPECT_TRUE(puts().isEmpty());
}

TEST_F(CrateCueSyncTest, UploadsEditsDebounced) {
    makeSync();
    const TrackPointer pTrack = dummyTrack();
    // Upstream's main cue and Intro, before the user did anything
    pTrack->setMainCuePosition(mixxx::audio::FramePos(framesOf(250.0)));
    pTrack->createAndAddCue(mixxx::CueType::Intro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(framesOf(250.0)),
            mixxx::audio::kInvalidFramePos);
    attachAndAnswer(pTrack, kUid, queryAnswer({}, {kUid}));
    // The analysis' cues are not the user's: nothing to send
    m_scheduler.runAll();
    EXPECT_TRUE(puts().isEmpty());

    // Two edits in a row
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(framesOf(15000.0)),
            mixxx::audio::kInvalidFramePos,
            mixxx::RgbColor(0x00aaff));
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(CrateCueSync::kDebounce, m_scheduler.tasks.first().delay);
    EXPECT_EQ(2000ms, CrateCueSync::kDebounce);
    pTrack->createAndAddCue(mixxx::CueType::Loop,
            2,
            mixxx::audio::FramePos(framesOf(60000.0)),
            mixxx::audio::FramePos(framesOf(67500.0)));
    QCoreApplication::processEvents();
    // Not before the debounce
    EXPECT_TRUE(puts().isEmpty());

    m_scheduler.runAll();
    waitForPuts(1);
    ASSERT_EQ(1, puts().size());
    const QJsonObject body = bodyOf(puts().constFirst());
    EXPECT_EQ(0, body.value(QStringLiteral("base_revision")).toInt(-1));
    EXPECT_EQ(kInstallation, body.value(QStringLiteral("client_id")).toString());
    const QJsonArray cues = body.value(QStringLiteral("cues")).toArray();
    ASSERT_EQ(4, cues.size());
    EXPECT_EQ(cuetype::kMain, cues[0].toObject().value(QStringLiteral("cue_type")).toString());
    EXPECT_EQ(cuetype::kIntro, cues[1].toObject().value(QStringLiteral("cue_type")).toString());
    EXPECT_EQ(cue(cuetype::kHotcue,
                      0,
                      15000.0,
                      QJsonValue::Null,
                      QJsonValue::Null,
                      QStringLiteral("#00aaff")),
            cues[2].toObject());
    EXPECT_EQ(cuetype::kLoop, cues[3].toObject().value(QStringLiteral("cue_type")).toString());
    EXPECT_DOUBLE_EQ(67500.0, cues[3].toObject().value(QStringLiteral("end_ms")).toDouble());
    // No other field: the server forbids them
    EXPECT_EQ(QStringList({QStringLiteral("base_revision"),
                      QStringLiteral("client_id"),
                      QStringLiteral("cues")}),
            body.keys());

    puts().constFirst().pReply->respond(
            200, QJsonDocument(withServerTime(cueSet(kUid, 1, cues, kInstallation))).toJson());
    ASSERT_TRUE(QTest::qWaitFor([this] {
        return m_pSync->revision(kServer, kUid) == 1;
    }));

    // The next edit goes on top of revision 1
    pTrack->findHotcueByIndex(0)->setLabel(QStringLiteral("Drop"));
    m_scheduler.runAll();
    waitForPuts(2);
    EXPECT_EQ(1, bodyOf(puts().constLast()).value(QStringLiteral("base_revision")).toInt(-1));
}

TEST_F(CrateCueSyncTest, ResolvesConflictServerWins) {
    makeSync();
    QSignalSpy replaced(m_pSync.get(), &CrateCueSync::localEditsReplaced);
    const TrackPointer pTrack = dummyTrack();
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid, 3, QJsonArray{cue(cuetype::kHotcue, 0, 1000.0)})}, {}));
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(0));

    // An edit here at 22:00, while another computer wrote revision 5 at 22:05
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            1,
            mixxx::audio::FramePos(framesOf(2000.0)),
            mixxx::audio::kInvalidFramePos);
    m_scheduler.runAll();
    waitForPuts(1);
    EXPECT_EQ(3, bodyOf(puts().constFirst()).value(QStringLiteral("base_revision")).toInt());
    const QJsonObject current = cueSet(kUid,
            5,
            QJsonArray{cue(cuetype::kHotcue,
                    4,
                    4000.0,
                    QJsonValue::Null,
                    QStringLiteral("Theirs"))},
            kOtherInstallation,
            QStringLiteral("2026-11-16T22:05:00.000000Z"));
    puts().constFirst().pReply->respond(409, conflictAnswer(current));

    // The server's set is the track's now, and the user hears of it
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return replaced.size() == 1;
    }));
    EXPECT_EQ(kUid, replaced.first().at(1).toString());
    EXPECT_EQ(nullptr, pTrack->findHotcueByIndex(0));
    EXPECT_EQ(nullptr, pTrack->findHotcueByIndex(1));
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(4));
    EXPECT_EQ(QStringLiteral("Theirs"), pTrack->findHotcueByIndex(4)->getLabel());
    EXPECT_EQ(5, m_pSync->revision(kServer, kUid));
    EXPECT_EQ(0, m_pOutbox->pendingCount());
    // Applying it is not an edit
    m_scheduler.runAll();
    QCoreApplication::processEvents();
    EXPECT_EQ(1, puts().size());
}

TEST_F(CrateCueSyncTest, TellsWhenANewerServerSetReplacesEditsNotSent) {
    makeSync();
    QSignalSpy replaced(m_pSync.get(), &CrateCueSync::localEditsReplaced);
    const TrackPointer pTrack = dummyTrack();
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid, 3, QJsonArray{cue(cuetype::kHotcue, 0, 1000.0)})}, {}));
    ASSERT_EQ(3, m_pSync->revision(kServer, kUid));
    EXPECT_TRUE(replaced.isEmpty());

    // An edit here, still in its debounce when the track loads again and the
    // server has revision 4 from another computer
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            1,
            mixxx::audio::FramePos(framesOf(2000.0)),
            mixxx::audio::kInvalidFramePos);
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid,
                                4,
                                QJsonArray{cue(cuetype::kHotcue,
                                        4,
                                        4000.0,
                                        QJsonValue::Null,
                                        QStringLiteral("Theirs"))})},
                    {}));

    // The server's set wins, and the user hears of it
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return replaced.size() == 1;
    }));
    EXPECT_EQ(kServer, replaced.first().at(0).toString());
    EXPECT_EQ(kUid, replaced.first().at(1).toString());
    EXPECT_EQ(nullptr, pTrack->findHotcueByIndex(1));
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(4));
    EXPECT_EQ(4, m_pSync->revision(kServer, kUid));
    m_scheduler.runAll();
    QCoreApplication::processEvents();
    EXPECT_TRUE(puts().isEmpty());

    // A newer set without edits here is no news
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid, 5, QJsonArray{cue(cuetype::kHotcue, 2, 3000.0)})}, {}));
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return m_pSync->revision(kServer, kUid) == 5;
    }));
    EXPECT_EQ(1, replaced.size());
}

TEST_F(CrateCueSyncTest, AsksAgainWhenTheQueryFails) {
    makeSync();
    const TrackPointer pTrack = dummyTrack();
    m_pSync->attach(pTrack, kServer, kUid);
    m_scheduler.runAll();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return sentTo(QStringLiteral("POST"), kQueryPath).size() == 1;
    }));
    sentTo(QStringLiteral("POST"), kQueryPath).constLast().pReply->respond(503);
    QCoreApplication::processEvents();

    // Again 5 s later, then 30 s
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(5s, m_scheduler.tasks.first().delay);
    m_scheduler.runAll();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return sentTo(QStringLiteral("POST"), kQueryPath).size() == 2;
    }));
    EXPECT_EQ((QJsonArray{kUid}),
            bodyOf(sentTo(QStringLiteral("POST"), kQueryPath).constLast())
                    .value(QStringLiteral("entity_uids"))
                    .toArray());
    sentTo(QStringLiteral("POST"), kQueryPath).constLast().pReply->failNetwork();
    QCoreApplication::processEvents();
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(30s, m_scheduler.tasks.first().delay);
    m_scheduler.runAll();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return sentTo(QStringLiteral("POST"), kQueryPath).size() == 3;
    }));
    sentTo(QStringLiteral("POST"), kQueryPath)
            .constLast()
            .pReply->respond(200,
                    queryAnswer({cueSet(kUid, 2, QJsonArray{cue(cuetype::kHotcue, 0, 1000.0)})},
                            {}));
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return m_pSync->revision(kServer, kUid) == 2;
    }));
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(0));

    // Reconciled: an edit goes up
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            1,
            mixxx::audio::FramePos(framesOf(2000.0)),
            mixxx::audio::kInvalidFramePos);
    m_scheduler.runAll();
    waitForPuts(1);
    EXPECT_EQ(2, bodyOf(puts().constFirst()).value(QStringLiteral("base_revision")).toInt());
}

TEST_F(CrateCueSyncTest, DoesNotAskAgainWhatTheServerRefuses) {
    makeSync();
    m_pSync->attach(dummyTrack(), kServer, kUid);
    m_scheduler.runAll();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return sentTo(QStringLiteral("POST"), kQueryPath).size() == 1;
    }));
    sentTo(QStringLiteral("POST"), kQueryPath).constLast().pReply->respond(422);
    QCoreApplication::processEvents();
    EXPECT_TRUE(m_scheduler.tasks.isEmpty());
}

TEST_F(CrateCueSyncTest, ResendsOnConflictWithAnOlderWrite) {
    makeSync();
    QSignalSpy replaced(m_pSync.get(), &CrateCueSync::localEditsReplaced);
    const TrackPointer pTrack = dummyTrack();
    attachAndAnswer(pTrack, kUid, queryAnswer({}, {kUid}));
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(framesOf(1000.0)),
            mixxx::audio::kInvalidFramePos);
    m_scheduler.runAll();
    waitForPuts(1);

    // Another computer wrote revision 2 before the edit here: the edit wins
    const QJsonObject current = cueSet(kUid,
            2,
            QJsonArray{cue(cuetype::kHotcue, 7, 7000.0)},
            kOtherInstallation,
            QStringLiteral("2026-11-16T21:00:00.000000Z"));
    puts().constFirst().pReply->respond(409, conflictAnswer(current));
    waitForPuts(2);
    const QJsonObject body = bodyOf(puts().constLast());
    EXPECT_EQ(2, body.value(QStringLiteral("base_revision")).toInt());
    EXPECT_EQ(1, body.value(QStringLiteral("cues")).toArray().size());
    EXPECT_TRUE(replaced.isEmpty());
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(0));
    EXPECT_EQ(nullptr, pTrack->findHotcueByIndex(7));

    puts().constLast().pReply->respond(200,
            QJsonDocument(withServerTime(
                                  cueSet(kUid, 3, body.value(QStringLiteral("cues")).toArray())))
                    .toJson());
    ASSERT_TRUE(QTest::qWaitFor([this] {
        return m_pSync->revision(kServer, kUid) == 3;
    }));
}

// Diego, 2026-11-16: "more recent" is decided on the server's clock. Every S7
// answer carries `server_time`; an edit is stamped in server time with the
// offset of the latest answer.
TEST_F(CrateCueSyncTest, DecidesConflictsOnTheServerClock) {
    makeSync();
    QSignalSpy replaced(m_pSync.get(), &CrateCueSync::localEditsReplaced);
    const TrackPointer pTrack = dummyTrack();
    // This computer's clock is 30 minutes behind the server's
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid, 3, QJsonArray{cue(cuetype::kHotcue, 0, 1000.0)})},
                    {},
                    QStringLiteral("2026-11-16T22:30:00.664907Z")));
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            1,
            mixxx::audio::FramePos(framesOf(2000.0)),
            mixxx::audio::kInvalidFramePos);
    m_scheduler.runAll();
    waitForPuts(1);
    // The edit, at 22:00 here, is 22:30 on the server
    const QList<QJsonObject> pending =
            m_pOutbox->pendingPayloads(kServer, crate::writeback::outboxkind::kCueSet);
    ASSERT_EQ(1, pending.size());
    // The server's microseconds are read too
    EXPECT_NEAR(0,
            QDateTime(QDate(2026, 11, 16), QTime(22, 30, 0, 665), QTimeZone::UTC)
                    .msecsTo(QDateTime::fromString(
                            pending.first().value(QStringLiteral("edited_at")).toString(),
                            Qt::ISODateWithMs)),
            1);

    // Another computer wrote at 22:10 server time: before the edit, which
    // the local clock alone would have put at 22:00
    puts().constFirst().pReply->respond(409,
            conflictAnswer(cueSet(kUid,
                                   5,
                                   QJsonArray{cue(cuetype::kHotcue, 4, 4000.0)},
                                   kOtherInstallation,
                                   QStringLiteral("2026-11-16T22:10:00.000000Z")),
                    QStringLiteral("2026-11-16T22:30:02.000000Z")));
    waitForPuts(2);
    EXPECT_TRUE(replaced.isEmpty());
    EXPECT_EQ(5, bodyOf(puts().constLast()).value(QStringLiteral("base_revision")).toInt());
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(1));
}

// A server that sends no server_time (one older than e463d653e): the edit
// keeps the local clock, as before.
TEST_F(CrateCueSyncTest, FallsBackToTheLocalClockWithoutServerTime) {
    makeSync();
    QSignalSpy replaced(m_pSync.get(), &CrateCueSync::localEditsReplaced);
    const TrackPointer pTrack = dummyTrack();
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid, 3, QJsonArray{cue(cuetype::kHotcue, 0, 1000.0)})},
                    {},
                    QString()));
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            1,
            mixxx::audio::FramePos(framesOf(2000.0)),
            mixxx::audio::kInvalidFramePos);
    m_scheduler.runAll();
    waitForPuts(1);
    const QList<QJsonObject> pending =
            m_pOutbox->pendingPayloads(kServer, crate::writeback::outboxkind::kCueSet);
    ASSERT_EQ(1, pending.size());
    EXPECT_EQ(m_now,
            QDateTime::fromString(
                    pending.first().value(QStringLiteral("edited_at")).toString(),
                    Qt::ISODateWithMs));

    // 22:10 on the server is after 22:00 here: the server wins
    puts().constFirst().pReply->respond(409,
            conflictAnswer(cueSet(kUid,
                                   5,
                                   QJsonArray{cue(cuetype::kHotcue, 4, 4000.0)},
                                   kOtherInstallation,
                                   QStringLiteral("2026-11-16T22:10:00.000000Z")),
                    QStringLiteral("2026-11-16T22:30:02.000000Z")));
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return replaced.size() == 1;
    }));
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(4));
    EXPECT_EQ(nullptr, pTrack->findHotcueByIndex(1));
}

// Diego, 2026-11-16: the first sync of a track with hot cues on both sides
// keeps the server's and moves the local ones to free slots, then uploads.
TEST_F(CrateCueSyncTest, MergesLocalHotCuesOnFirstSync) {
    makeSync();
    QSignalSpy merged(m_pSync.get(), &CrateCueSync::cuesMerged);
    const TrackPointer pTrack = dummyTrack();
    // Set on this computer before it ever synced the track
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(framesOf(1000.0)),
            mixxx::audio::kInvalidFramePos);
    pTrack->createAndAddCue(mixxx::CueType::Loop,
            1,
            mixxx::audio::FramePos(framesOf(3000.0)),
            mixxx::audio::FramePos(framesOf(4000.0)));
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            5,
            mixxx::audio::FramePos(framesOf(5000.0)),
            mixxx::audio::kInvalidFramePos);

    // The server's: slot 0 is taken, slot 1 has the same loop
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid,
                                2,
                                QJsonArray{cue(cuetype::kIntro, 0, 500.0),
                                        cue(cuetype::kHotcue, 0, 2000.0),
                                        cue(cuetype::kLoop, 1, 3000.0, 4000.0)})},
                    {}));

    // The server's slot 0 stays; the local one moved to the first free slot
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(0));
    EXPECT_NEAR(framesOf(2000.0), pTrack->findHotcueByIndex(0)->getPosition().value(), 0.01);
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(2));
    EXPECT_NEAR(framesOf(1000.0), pTrack->findHotcueByIndex(2)->getPosition().value(), 0.01);
    EXPECT_EQ(mixxx::CueType::Loop, pTrack->findHotcueByIndex(1)->getType());
    ASSERT_NE(nullptr, pTrack->findHotcueByIndex(5));
    EXPECT_NE(nullptr, pTrack->findCueByType(mixxx::CueType::Intro));
    ASSERT_EQ(1, merged.size());
    EXPECT_EQ(1, merged.first().at(3).toInt()); // moved
    EXPECT_EQ(0, merged.first().at(4).toInt()); // dropped

    // And the merged set goes up on top of revision 2
    waitForPuts(1);
    const QJsonObject body = bodyOf(puts().constFirst());
    EXPECT_EQ(2, body.value(QStringLiteral("base_revision")).toInt());
    EXPECT_EQ(5, body.value(QStringLiteral("cues")).toArray().size());
}

TEST_F(CrateCueSyncTest, DropsLocalCuesWithoutAFreeSlot) {
    makeSync();
    QSignalSpy merged(m_pSync.get(), &CrateCueSync::cuesMerged);
    const TrackPointer pTrack = dummyTrack();
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            3,
            mixxx::audio::FramePos(framesOf(99000.0)),
            mixxx::audio::kInvalidFramePos);
    QJsonArray full;
    for (int slot = 0; slot < crate::writeback::kSyncedHotcueSlots; ++slot) {
        full.append(cue(cuetype::kHotcue, slot, 1000.0 * (slot + 1)));
    }
    attachAndAnswer(pTrack, kUid, queryAnswer({cueSet(kUid, 2, full)}, {}));

    EXPECT_NEAR(framesOf(4000.0), pTrack->findHotcueByIndex(3)->getPosition().value(), 0.01);
    ASSERT_EQ(1, merged.size());
    EXPECT_EQ(0, merged.first().at(3).toInt());
    EXPECT_EQ(1, merged.first().at(4).toInt());
    // Nothing to add to the server's set
    m_scheduler.runAll();
    QCoreApplication::processEvents();
    EXPECT_TRUE(puts().isEmpty());
}

// Diego, 2026-11-16: once synced, the server's set rules every cue type, so
// deleting the intro on one computer deletes it on the others.
TEST_F(CrateCueSyncTest, PropagatesDeletionsOfAutomaticCues) {
    makeSync();
    const TrackPointer pTrack = dummyTrack();
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid,
                                3,
                                QJsonArray{cue(cuetype::kMain, 0, 100.0),
                                        cue(cuetype::kIntro, 0, 500.0),
                                        cue(cuetype::kHotcue, 0, 1000.0)})},
                    {}));
    ASSERT_NE(nullptr, pTrack->findCueByType(mixxx::CueType::Intro));

    // Another computer deleted the intro
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid,
                                4,
                                QJsonArray{cue(cuetype::kMain, 0, 100.0),
                                        cue(cuetype::kHotcue, 0, 1000.0)})},
                    {}));
    EXPECT_EQ(nullptr, pTrack->findCueByType(mixxx::CueType::Intro));
    EXPECT_EQ(4, m_pSync->revision(kServer, kUid));

    // Upstream's silence analysis puts one back: the synced set wins
    pTrack->createAndAddCue(mixxx::CueType::Intro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(framesOf(250.0)),
            mixxx::audio::kInvalidFramePos);
    pTrack->analysisFinished();
    QCoreApplication::processEvents();
    EXPECT_EQ(nullptr, pTrack->findCueByType(mixxx::CueType::Intro));
    m_scheduler.runAll();
    QCoreApplication::processEvents();
    EXPECT_TRUE(puts().isEmpty());
}

TEST_F(CrateCueSyncTest, OutboxRetryKeepsTheSameBody) {
    makeSync();
    const TrackPointer pTrack = dummyTrack();
    attachAndAnswer(pTrack, kUid, queryAnswer({}, {kUid}));
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(framesOf(1000.0)),
            mixxx::audio::kInvalidFramePos);
    m_scheduler.runAll();
    waitForPuts(1);
    puts().constFirst().pReply->respond(503);
    ASSERT_TRUE(QTest::qWaitFor([this] {
        return !m_outboxScheduler.tasks.isEmpty();
    }));
    m_outboxScheduler.runAll();
    waitForPuts(2);
    // The same base revision and content: if the first one was stored, the
    // server answers 200 to this one too
    EXPECT_EQ(puts().constFirst().body, puts().constLast().body);
}

TEST_F(CrateCueSyncTest, QueriesLoadedTracksInOneBatch) {
    makeSync();
    const TrackPointer pA = dummyTrack(QStringLiteral("a.flac"));
    const TrackPointer pB = dummyTrack(QStringLiteral("b.flac"));
    const TrackPointer pC = dummyTrack(QStringLiteral("c.flac"));
    m_pSync->attach(pA, kServer, kUid);
    m_pSync->attach(pB, kServer, kUidB.toUpper());
    m_pSync->attach(pC, kServer, kUidC);
    // One wait for all of them
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(CrateCueSync::kBatchWindow, m_scheduler.tasks.first().delay);
    m_scheduler.runAll();
    ASSERT_TRUE(QTest::qWaitFor([this] {
        return !sentTo(QStringLiteral("POST"), kQueryPath).isEmpty();
    }));
    ASSERT_EQ(1, sentTo(QStringLiteral("POST"), kQueryPath).size());
    EXPECT_EQ(QJsonArray({kUid, kUidB, kUidC}),
            bodyOf(sentTo(QStringLiteral("POST"), kQueryPath).constFirst())
                    .value(QStringLiteral("entity_uids"))
                    .toArray());
    sentTo(QStringLiteral("POST"), kQueryPath)
            .constFirst()
            .pReply->respond(200,
                    queryAnswer({cueSet(kUidB, 1, QJsonArray{cue(cuetype::kHotcue, 3, 900.0)})},
                            {kUid, kUidC}));
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return pB->findHotcueByIndex(3) != nullptr;
    }));
    EXPECT_EQ(1, m_pSync->revision(kServer, kUidB));
}

TEST_F(CrateCueSyncTest, SyncsOnlyWithTheCuesFeature) {
    m_cuesFeature = false;
    makeSync();
    const TrackPointer pTrack = dummyTrack();
    m_pSync->attach(pTrack, kServer, kUid);
    m_scheduler.runAll();
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(framesOf(1000.0)),
            mixxx::audio::kInvalidFramePos);
    m_scheduler.runAll();
    QCoreApplication::processEvents();
    EXPECT_TRUE(m_network.sent.isEmpty());
    EXPECT_EQ(0, m_pOutbox->pendingCount());
}

TEST_F(CrateCueSyncTest, KeepsTheRevisionAcrossRestarts) {
    makeSync();
    const TrackPointer pTrack = dummyTrack();
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid, 4, QJsonArray{cue(cuetype::kHotcue, 0, 1000.0)})}, {}));
    m_pSync->waitForStorage();
    m_pSync.reset();

    // The next run knows revision 4 and that the track is in sync with it
    makeSync();
    EXPECT_EQ(4, m_pSync->revision(kServer, kUid));
    attachAndAnswer(pTrack,
            kUid,
            queryAnswer({cueSet(kUid, 4, QJsonArray{cue(cuetype::kHotcue, 0, 1000.0)})}, {}));
    m_scheduler.runAll();
    QCoreApplication::processEvents();
    EXPECT_TRUE(puts().isEmpty());
}

TEST_F(CrateCueSyncTest, MapsCuesBothWays) {
    const TrackPointer pTrack = dummyTrack();
    pTrack->setMainCuePosition(mixxx::audio::FramePos(framesOf(120.0)));
    pTrack->createAndAddCue(mixxx::CueType::Intro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(framesOf(500.0)),
            mixxx::audio::kInvalidFramePos);
    pTrack->createAndAddCue(mixxx::CueType::Outro,
            Cue::kNoHotCue,
            mixxx::audio::kInvalidFramePos,
            mixxx::audio::FramePos(framesOf(230000.0)));
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
                  0,
                  mixxx::audio::FramePos(framesOf(15240.517)),
                  mixxx::audio::kInvalidFramePos,
                  mixxx::RgbColor(0xFF8800))
            ->setLabel(QStringLiteral("Drop"));
    pTrack->createAndAddCue(mixxx::CueType::Loop,
            2,
            mixxx::audio::FramePos(framesOf(61875.0)),
            mixxx::audio::FramePos(framesOf(69375.0)));
    // A jump stores where it jumps to as its start and where from as its end
    pTrack->createAndAddCue(mixxx::CueType::Jump,
            3,
            mixxx::audio::FramePos(framesOf(32000.0)),
            mixxx::audio::FramePos(framesOf(96000.0)));
    // What the contract has no place for
    pTrack->createAndAddCue(mixxx::CueType::Loop,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(framesOf(1000.0)),
            mixxx::audio::FramePos(framesOf(2000.0)));
    pTrack->createAndAddCue(mixxx::CueType::N60dBSound,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(framesOf(10.0)),
            mixxx::audio::FramePos(framesOf(239000.0)));
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            36,
            mixxx::audio::FramePos(framesOf(5000.0)),
            mixxx::audio::kInvalidFramePos);

    const SyncedCues cues = crate::writeback::syncedCuesOf(pTrack);
    const QJsonArray json = crate::writeback::toJson(cues);
    EXPECT_EQ(QJsonArray({cue(cuetype::kMain, 0, 120.0),
                      cue(cuetype::kIntro, 0, 500.0),
                      cue(cuetype::kOutro, 0, QJsonValue::Null, 230000.0),
                      cue(cuetype::kHotcue,
                              0,
                              15240.517,
                              QJsonValue::Null,
                              QStringLiteral("Drop"),
                              QStringLiteral("#ff8800")),
                      cue(cuetype::kLoop,
                              2,
                              61875.0,
                              69375.0,
                              QJsonValue::Null,
                              mixxx::RgbColor::toQString(
                                      mixxx::PredefinedColorPalettes::kDefaultCueColor)
                                      .toLower()),
                      cue(cuetype::kJump,
                              3,
                              32000.0,
                              96000.0,
                              QJsonValue::Null,
                              mixxx::RgbColor::toQString(
                                      mixxx::PredefinedColorPalettes::kDefaultCueColor)
                                      .toLower())}),
            json);

    // Back on another track: the same cues
    const TrackPointer pOther = dummyTrack(QStringLiteral("other.flac"));
    pOther->createAndAddCue(mixxx::CueType::HotCue,
            5,
            mixxx::audio::FramePos(framesOf(9000.0)),
            mixxx::audio::kInvalidFramePos);
    ASSERT_TRUE(crate::writeback::applySyncedCues(
            pOther, crate::writeback::syncedCuesFromJson(json)));
    EXPECT_EQ(cues, crate::writeback::syncedCuesOf(pOther));
    // The old slot went with the set
    EXPECT_EQ(nullptr, pOther->findHotcueByIndex(5));
    EXPECT_NEAR(framesOf(120.0), pOther->getMainCuePosition().value(), 0.01);

    // What the server would refuse is never read
    const SyncedCues read = crate::writeback::syncedCuesFromJson(
            QJsonArray{cue(cuetype::kLoop, 1, 5000.0, 4000.0),
                    cue(cuetype::kHotcue, 36, 1000.0),
                    cue(QStringLiteral("beat"), 0, 1000.0),
                    cue(cuetype::kHotcue, 1, 1000.0, QJsonValue::Null, QStringLiteral("   "))});
    ASSERT_EQ(1, read.size());
    EXPECT_EQ(1, read.first().slot);
    EXPECT_TRUE(read.first().label.isEmpty());
}

} // namespace
