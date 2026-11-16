#include "crate/writeback/cratecorrections.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
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
#include "track/beats.h"
#include "track/track.h"

namespace {

using namespace std::chrono_literals;
using crate::Client;
using crate::ClientOptions;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;
using crate::writeback::Correction;
using crate::writeback::CrateCorrections;
using crate::writeback::Outbox;
using mixxx::track::io::key::ChromaticKey;

const QString kServer = QStringLiteral("srv-1");
const QString kUid = QStringLiteral("0b6f2c1e-5d6a-4c1b-9a51-2f0c8a3c9e10");
const QString kInstallation = QStringLiteral("install-1");
const QString kQueryPath = QStringLiteral("/api/decks/corrections/query");
const QString kPutPath = QStringLiteral("/api/decks/corrections/") + kUid;
const QString kAudioFile = QStringLiteral("id3-test-data/cover-test-øé~ł€˚.flac");
constexpr int kSampleRate = 44100;

QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
    return QJsonDocument::fromJson(sent.body).object();
}

QJsonObject correction(qint64 revision,
        QJsonValue bpm,
        QJsonValue grid,
        QJsonValue key,
        const QString& clientId = QStringLiteral("install-2"),
        const QString& updatedAt = QStringLiteral("2026-11-16T18:10:02.771940Z")) {
    return QJsonObject{{QStringLiteral("entity_uid"), kUid},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("updated_at"), updatedAt},
            {QStringLiteral("client_id"), clientId},
            {QStringLiteral("bpm"), bpm},
            {QStringLiteral("beat_grid_format"),
                    grid.isArray() ? QJsonValue(QStringLiteral("delta-ms-v1")) : QJsonValue()},
            {QStringLiteral("beat_grid_ms"), grid},
            {QStringLiteral("key"), key}};
}

/// The server's clock in the fakes' answers, the test's local clock.
const QString kServerTime = QStringLiteral("2026-11-16T22:00:00.000000Z");

/// `server_time`, which every S7 answer carries.
QJsonObject withServerTime(QJsonObject answer) {
    answer.insert(QStringLiteral("server_time"), kServerTime);
    return answer;
}

QByteArray queryAnswer(const QList<QJsonObject>& corrections, const QStringList& missing) {
    QJsonObject byUid;
    for (const QJsonObject& entry : corrections) {
        byUid.insert(entry.value(QStringLiteral("entity_uid")).toString(), entry);
    }
    return QJsonDocument(withServerTime(QJsonObject{{QStringLiteral("corrections"), byUid},
                                 {QStringLiteral("missing"), QJsonArray::fromStringList(missing)}}))
            .toJson();
}

/// A grid of `count` beats every `periodMs` from `firstMs`.
QJsonArray grid(qint64 firstMs, double periodMs, int count) {
    QJsonArray beats;
    for (int i = 0; i < count; ++i) {
        beats.append(static_cast<qint64>(
                std::llround(static_cast<double>(firstMs) + i * periodMs)));
    }
    return beats;
}

/// The rules of delta-ms-v1 (encode_beat_grid on the server).
void expectValidGrid(const QJsonArray& beats) {
    ASSERT_GE(beats.size(), 2);
    ASSERT_LE(beats.size(), 100000);
    qint64 previous = 0;
    for (const QJsonValue& beat : beats) {
        ASSERT_TRUE(beat.isDouble());
        const double value = beat.toDouble();
        ASSERT_EQ(std::floor(value), value) << "not a whole millisecond";
        ASSERT_GT(static_cast<qint64>(value), previous) << "not strictly increasing after 0";
        ASSERT_LE(value, 86400000.0);
        previous = static_cast<qint64>(value);
    }
}

class CrateCorrectionsTest : public MixxxDbTest, SoundSourceProviderRegistration {
  protected:
    CrateCorrectionsTest()
            : MixxxDbTest(true),
              m_now(QDateTime(QDate(2026, 11, 16), QTime(22, 0), QTimeZone::UTC)) {
    }

    void SetUp() override {
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ClientOptions clientOptions;
        clientOptions.deviceFingerprint = kInstallation;
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
        CrateCorrections::Options syncOptions;
        syncOptions.pOutbox = m_pOutbox.get();
        syncOptions.pDbConnectionPool = dbConnectionPooler();
        syncOptions.clientFor = [this](const QString&) {
            return m_pClient.get();
        };
        syncOptions.clientId = kInstallation;
        syncOptions.schedule = m_scheduler.function();
        syncOptions.now = [this] {
            return m_now;
        };
        m_pSync = std::make_unique<CrateCorrections>(std::move(syncOptions));
        m_pSync->start();
        ASSERT_TRUE(QTest::qWaitFor([this] {
            return m_pSync->isLoaded();
        }));
    }

    void TearDown() override {
        m_pSync->waitForStorage();
        m_pSync.reset();
        m_pOutbox.reset();
        m_pClient.reset();
    }

    /// A track with a sample rate and upstream's grid at 120 BPM.
    static TrackPointer analysedTrack() {
        TrackPointer pTrack = Track::newTemporary(QStringLiteral("/cache/srv-1/track.flac"));
        pTrack->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
                mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::Bitrate(320),
                mixxx::Duration::fromSeconds(240));
        pTrack->trySetBeats(mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::FramePos(0.5 * kSampleRate),
                mixxx::Bpm(120.0)));
        pTrack->setKey(mixxx::track::io::key::C_MAJOR, mixxx::track::io::key::ANALYZER);
        return pTrack;
    }

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

    QList<FakeNetworkAccessManager::Sent> sentTo(
            QNetworkAccessManager::Operation operation, const QString& path) const {
        QList<FakeNetworkAccessManager::Sent> matching;
        for (const auto& sent : m_network.sent) {
            if (sent.operation == operation && sent.request.url().path() == path) {
                matching.append(sent);
            }
        }
        return matching;
    }

    QList<FakeNetworkAccessManager::Sent> puts() const {
        return sentTo(QNetworkAccessManager::PutOperation, kPutPath);
    }

    void attachAndAnswer(const TrackPointer& pTrack, const QByteArray& answer) {
        const auto queries = sentTo(QNetworkAccessManager::PostOperation, kQueryPath).size();
        m_pSync->attach(pTrack, kServer, kUid);
        m_scheduler.runAll();
        ASSERT_TRUE(QTest::qWaitFor([&] {
            return sentTo(QNetworkAccessManager::PostOperation, kQueryPath).size() > queries;
        }));
        sentTo(QNetworkAccessManager::PostOperation, kQueryPath)
                .constLast()
                .pReply->respond(200, answer);
        QCoreApplication::processEvents();
    }

    void waitForPuts(int count) {
        EXPECT_TRUE(QTest::qWaitFor([&] {
            return puts().size() >= count;
        })) << "PUTs sent: "
            << puts().size();
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    FakeScheduler m_scheduler;
    FakeScheduler m_outboxScheduler;
    QDateTime m_now;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<Outbox> m_pOutbox;
    std::unique_ptr<CrateCorrections> m_pSync;
};

TEST_F(CrateCorrectionsTest, OffersUploadAfterManualGridEdit) {
    QSignalSpy offered(m_pSync.get(), &CrateCorrections::correctionOffered);
    const TrackPointer pTrack = analysedTrack();
    attachAndAnswer(pTrack, queryAnswer({}, {kUid}));

    // Upstream's analysis changes the grid: not a correction
    pTrack->trySetBeats(mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::FramePos(0.25 * kSampleRate),
            mixxx::Bpm(120.0)));
    pTrack->analysisFinished();
    m_scheduler.runAll();
    EXPECT_TRUE(offered.isEmpty());
    EXPECT_FALSE(m_pSync->offer(kServer, kUid));

    // The user unlocks the BPM and moves the grid
    pTrack->setBpmLocked(true);
    EXPECT_FALSE(pTrack->trySetBeats(*pTrack->getBeats()->tryTranslate(441.0)));
    pTrack->setBpmLocked(false);
    ASSERT_TRUE(pTrack->trySetBeats(*pTrack->getBeats()->tryTranslate(441.0)));
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(2000ms, m_scheduler.tasks.first().delay);
    EXPECT_TRUE(offered.isEmpty());
    m_scheduler.runAll();
    ASSERT_EQ(1, offered.size());
    EXPECT_EQ(kServer, offered.first().at(0).toString());
    EXPECT_EQ(kUid, offered.first().at(1).toString());
    const auto offer = m_pSync->offer(kServer, kUid);
    ASSERT_TRUE(offer);
    EXPECT_TRUE(offer->grid);
    EXPECT_FALSE(offer->key);
    // Nothing goes to Crate until the user says so
    EXPECT_TRUE(puts().isEmpty());

    ASSERT_TRUE(m_pSync->save(kServer, kUid));
    waitForPuts(1);
    const QJsonObject body = bodyOf(puts().constFirst());
    EXPECT_EQ(QStringList({QStringLiteral("base_revision"),
                      QStringLiteral("beat_grid_format"),
                      QStringLiteral("beat_grid_ms"),
                      QStringLiteral("bpm"),
                      QStringLiteral("client_id"),
                      QStringLiteral("key")}),
            body.keys());
    EXPECT_EQ(0, body.value(QStringLiteral("base_revision")).toInt(-1));
    EXPECT_EQ(kInstallation, body.value(QStringLiteral("client_id")).toString());
    EXPECT_DOUBLE_EQ(120.0, body.value(QStringLiteral("bpm")).toDouble());
    EXPECT_EQ(QStringLiteral("delta-ms-v1"), body.value(QStringLiteral("beat_grid_format")));
    const QJsonArray beats = body.value(QStringLiteral("beat_grid_ms")).toArray();
    expectValidGrid(beats);
    // 0.25 s plus the 10 ms the user moved it
    EXPECT_EQ(260, beats.first().toInteger());
    EXPECT_EQ(760, beats.at(1).toInteger());
    EXPECT_TRUE(body.value(QStringLiteral("key")).isNull());
    EXPECT_FALSE(m_pSync->offer(kServer, kUid));

    puts().constFirst().pReply->respond(200,
            QJsonDocument(withServerTime(correction(1, 120.0, beats, QJsonValue(), kInstallation)))
                    .toJson());
    ASSERT_TRUE(QTest::qWaitFor([this] {
        return m_pSync->revision(kServer, kUid) == 1;
    }));
    EXPECT_TRUE(m_pSync->hasCorrection(kServer, kUid));
}

TEST_F(CrateCorrectionsTest, OffersTheKeyTheUserTyped) {
    QSignalSpy offered(m_pSync.get(), &CrateCorrections::correctionOffered);
    const TrackPointer pTrack = analysedTrack();
    attachAndAnswer(pTrack,
            queryAnswer({correction(2, 128.0, QJsonValue(), QJsonValue())}, {}));

    // The tags' key is not a correction; the user's is
    pTrack->setKey(mixxx::track::io::key::D_MAJOR, mixxx::track::io::key::FILE_METADATA);
    m_scheduler.runAll();
    EXPECT_TRUE(offered.isEmpty());
    pTrack->setKeyText(QStringLiteral("Am"), mixxx::track::io::key::USER);
    m_scheduler.runAll();
    ASSERT_EQ(1, offered.size());
    ASSERT_TRUE(m_pSync->offer(kServer, kUid)->key);

    ASSERT_TRUE(m_pSync->save(kServer, kUid));
    waitForPuts(1);
    const QJsonObject body = bodyOf(puts().constFirst());
    EXPECT_EQ(2, body.value(QStringLiteral("base_revision")).toInt());
    EXPECT_EQ(QStringLiteral("8A"), body.value(QStringLiteral("key")).toString());
    // The stored BPM stays corrected
    EXPECT_DOUBLE_EQ(128.0, body.value(QStringLiteral("bpm")).toDouble());
    EXPECT_TRUE(body.value(QStringLiteral("beat_grid_ms")).isNull());
}

TEST_F(CrateCorrectionsTest, AppliesCorrectionOverProfile) {
    QSignalSpy offered(m_pSync.get(), &CrateCorrections::correctionOffered);
    const crate::analysis::MixProfile mixProfile = profile();
    crate::analysis::Policy policy;
    policy.crateBeats = true;
    policy.crateKey = true;
    // The profile's grid (124 BPM, locked) and key (8A) are on the track
    const TrackPointer pTrack = openedTrack();
    const crate::analysis::ImportOutcome outcome =
            crate::analysis::AnalysisImporter::apply(pTrack, mixProfile, policy);
    ASSERT_TRUE(outcome.beatsImported) << outcome.beatsSkipped.toStdString();
    ASSERT_TRUE(outcome.keySet);
    m_pSync->absorbAutomaticChanges(pTrack);
    ASSERT_NEAR(124.0, pTrack->getBpm(), 1e-6);

    // The user's correction from another computer: 125 BPM from 600 ms, 9B
    attachAndAnswer(pTrack,
            queryAnswer({correction(2, 125.0, grid(600, 480.0, 64), QStringLiteral("9b"))}, {}));
    EXPECT_EQ(2, m_pSync->revision(kServer, kUid));
    EXPECT_NEAR(125.0, pTrack->getBpm(), 1e-6);
    EXPECT_TRUE(pTrack->isBpmLocked());
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    ASSERT_NE(nullptr, pBeats);
    EXPECT_EQ(crate::writeback::kCorrectionBeatsSubVersion, pBeats->getSubVersion());
    const double perMs = pTrack->getSampleRate().toDouble() / 1000.0;
    EXPECT_NEAR(600.0 * perMs,
            pBeats->findClosestBeat(mixxx::audio::FramePos(600.0 * perMs)).value(),
            1.0);
    EXPECT_EQ(mixxx::track::io::key::G_MAJOR, pTrack->getKey());

    // Crate's profile again, of a new revision: the correction stays
    crate::analysis::MixProfile newer = mixProfile;
    newer.profileRevision = QStringLiteral("a-newer-revision");
    const crate::analysis::ImportOutcome again =
            crate::analysis::AnalysisImporter::apply(pTrack, newer, policy, outcome.appliedCues);
    m_pSync->absorbAutomaticChanges(pTrack);
    EXPECT_FALSE(again.beatsImported);
    EXPECT_FALSE(again.keySet);
    EXPECT_NEAR(125.0, pTrack->getBpm(), 1e-6);
    EXPECT_EQ(mixxx::track::io::key::G_MAJOR, pTrack->getKey());

    // Applying it is no correction of this computer
    m_scheduler.runAll();
    EXPECT_TRUE(offered.isEmpty());
    EXPECT_TRUE(puts().isEmpty());
}

TEST_F(CrateCorrectionsTest, EncodesTheGridAsDeltaMsV1) {
    const TrackPointer pTrack = analysedTrack();
    // 0.25 s from the start, at 128 BPM
    pTrack->trySetBeats(mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::FramePos(0.25 * kSampleRate),
            mixxx::Bpm(128.0)));
    const QVector<qint64> beats = crate::writeback::beatGridOf(pTrack);
    QJsonArray json;
    for (const qint64 beat : beats) {
        json.append(beat);
    }
    expectValidGrid(json);
    EXPECT_EQ(250, beats.first());
    EXPECT_EQ(719, beats.at(1)); // 250 + 468.75, rounded
    // To the end of the 240 s track
    EXPECT_EQ(512, beats.size());

    // Back to the same grid
    const mixxx::BeatsPointer pBeats = crate::writeback::beatsOfGrid(
            mixxx::audio::SampleRate(kSampleRate), beats, std::nullopt);
    ASSERT_NE(nullptr, pBeats);
    EXPECT_TRUE(pBeats->hasConstantTempo());
    EXPECT_NEAR(128.0, pBeats->getLastMarkerBpm().value(), 0.01);
    EXPECT_NEAR(0.25 * kSampleRate, pBeats->firstBeat().value(), 0.5 * kSampleRate / 1000.0);

    // A grid that is not constant becomes a beat map
    const QVector<qint64> irregular = {500, 1000, 1480, 2000, 2600};
    const mixxx::BeatsPointer pMap = crate::writeback::beatsOfGrid(
            mixxx::audio::SampleRate(kSampleRate), irregular, std::nullopt);
    ASSERT_NE(nullptr, pMap);
    EXPECT_FALSE(pMap->hasConstantTempo());

    // What the server would refuse is read as not corrected
    const Correction read = crate::writeback::correctionFromJson(
            correction(1, 400.0, QJsonArray{0, 500}, QStringLiteral("13A")));
    EXPECT_TRUE(read.isEmpty());
}

TEST_F(CrateCorrectionsTest, ClearsTheCorrection) {
    const TrackPointer pTrack = analysedTrack();
    attachAndAnswer(pTrack,
            queryAnswer({correction(3, 126.0, QJsonValue(), QStringLiteral("8A"))}, {}));
    ASSERT_TRUE(m_pSync->hasCorrection(kServer, kUid));
    ASSERT_TRUE(pTrack->isBpmLocked());
    ASSERT_EQ(mixxx::track::io::key::USER, crate::analysis::keySourceOf(pTrack));

    ASSERT_TRUE(m_pSync->clear(kServer, kUid));
    // Diego, 2026-11-16: the grid is unlocked and the correction's key is no
    // longer the user's, so that Crate's analysis applies again
    EXPECT_FALSE(pTrack->isBpmLocked());
    EXPECT_NE(mixxx::track::io::key::USER, crate::analysis::keySourceOf(pTrack));
    waitForPuts(1);
    const QJsonObject body = bodyOf(puts().constFirst());
    EXPECT_EQ(3, body.value(QStringLiteral("base_revision")).toInt());
    for (const QString& value : {QStringLiteral("bpm"),
                 QStringLiteral("beat_grid_format"),
                 QStringLiteral("beat_grid_ms"),
                 QStringLiteral("key")}) {
        EXPECT_TRUE(body.value(value).isNull()) << value.toStdString();
    }
    EXPECT_FALSE(m_pSync->hasCorrection(kServer, kUid));
    puts().constFirst().pReply->respond(200,
            QJsonDocument(withServerTime(correction(
                                  4, QJsonValue(), QJsonValue(), QJsonValue(), kInstallation)))
                    .toJson());
    ASSERT_TRUE(QTest::qWaitFor([this] {
        return m_pSync->revision(kServer, kUid) == 4;
    }));
    // Nothing more to clear
    EXPECT_FALSE(m_pSync->clear(kServer, kUid));
}

// Diego, 2026-11-16: clearing a correction puts Crate's profile back on the
// track, here and on the other computers once they see it is gone.
TEST_F(CrateCorrectionsTest, ClearingRestoresTheProfile) {
    const crate::analysis::MixProfile mixProfile = profile();
    crate::analysis::Policy policy;
    policy.crateBeats = true;
    policy.crateKey = true;
    QStringList restored;
    m_pSync->setProfileRestorer(
            [&](const TrackPointer& pTrack, const QString& serverId, const QString& uid) {
                restored.append(serverId + QLatin1Char('/') + uid);
                crate::analysis::AnalysisImporter::apply(pTrack, mixProfile, policy);
                m_pSync->absorbAutomaticChanges(pTrack);
            });
    const TrackPointer pTrack = openedTrack();
    ASSERT_TRUE(crate::analysis::AnalysisImporter::apply(pTrack, mixProfile, policy).beatsImported);
    attachAndAnswer(pTrack,
            queryAnswer({correction(2, 125.0, grid(600, 480.0, 64), QStringLiteral("9B"))}, {}));
    ASSERT_NEAR(125.0, pTrack->getBpm(), 1e-6);

    ASSERT_TRUE(m_pSync->clear(kServer, kUid));
    EXPECT_EQ(QStringList{kServer + QStringLiteral("/") + kUid}, restored);
    EXPECT_NEAR(124.0, pTrack->getBpm(), 1e-6);
    EXPECT_EQ(mixxx::track::io::key::A_MINOR, pTrack->getKey());
    // Crate's grid, which the profile locks as its own
    EXPECT_EQ(crate::analysis::CrateBeatsImporter::subVersion(mixProfile.profileRevision),
            pTrack->getBeats()->getSubVersion());
    m_scheduler.runAll();
    EXPECT_FALSE(m_pSync->offer(kServer, kUid));
}

TEST_F(CrateCorrectionsTest, RevertsWhenTheCorrectionIsGoneElsewhere) {
    const crate::analysis::MixProfile mixProfile = profile();
    crate::analysis::Policy policy;
    policy.crateBeats = true;
    policy.crateKey = true;
    int restores = 0;
    m_pSync->setProfileRestorer([&](const TrackPointer& pTrack, const QString&, const QString&) {
        ++restores;
        crate::analysis::AnalysisImporter::apply(pTrack, mixProfile, policy);
        m_pSync->absorbAutomaticChanges(pTrack);
    });
    const TrackPointer pTrack = openedTrack();
    ASSERT_TRUE(crate::analysis::AnalysisImporter::apply(pTrack, mixProfile, policy).beatsImported);
    attachAndAnswer(pTrack,
            queryAnswer({correction(2, 125.0, grid(600, 480.0, 64), QStringLiteral("9B"))}, {}));
    ASSERT_NEAR(125.0, pTrack->getBpm(), 1e-6);
    EXPECT_EQ(0, restores);

    // Another computer cleared it
    attachAndAnswer(pTrack,
            queryAnswer({correction(3, QJsonValue(), QJsonValue(), QJsonValue())}, {}));
    EXPECT_EQ(1, restores);
    EXPECT_EQ(3, m_pSync->revision(kServer, kUid));
    EXPECT_NEAR(124.0, pTrack->getBpm(), 1e-6);
    EXPECT_EQ(mixxx::track::io::key::A_MINOR, pTrack->getKey());
    EXPECT_FALSE(m_pSync->hasCorrection(kServer, kUid));
}

TEST_F(CrateCorrectionsTest, ConflictAppliesTheNewerCorrection) {
    QSignalSpy replaced(m_pSync.get(), &CrateCorrections::localEditsReplaced);
    const TrackPointer pTrack = analysedTrack();
    attachAndAnswer(pTrack, queryAnswer({}, {kUid}));
    pTrack->setKeyText(QStringLiteral("Am"), mixxx::track::io::key::USER);
    m_scheduler.runAll();
    ASSERT_TRUE(m_pSync->save(kServer, kUid));
    waitForPuts(1);

    // Another computer saved one at 22:10, after the edit here
    puts().constFirst().pReply->respond(409,
            QJsonDocument(QJsonObject{{QStringLiteral("server_time"), kServerTime},
                                  {QStringLiteral("detail"),
                                          QStringLiteral("Correction revision conflict")},
                                  {QStringLiteral("current"),
                                          correction(1,
                                                  122.0,
                                                  QJsonValue(),
                                                  QStringLiteral("5A"),
                                                  QStringLiteral("install-2"),
                                                  QStringLiteral("2026-11-16T22:10:00Z"))}})
                    .toJson());
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return replaced.size() == 1;
    }));
    EXPECT_EQ(1, m_pSync->revision(kServer, kUid));
    EXPECT_EQ(mixxx::track::io::key::C_MINOR, pTrack->getKey());
    EXPECT_NEAR(122.0, pTrack->getBpm(), 1e-6);
    EXPECT_TRUE(pTrack->isBpmLocked());
    EXPECT_EQ(1, puts().size());
    EXPECT_EQ(0, m_pOutbox->pendingCount());
}

} // namespace
