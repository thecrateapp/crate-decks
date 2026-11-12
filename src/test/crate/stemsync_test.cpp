// Stems of a cached track and the Crate server (plan ST5.PR2, STEM-D6, D8):
// the server's stem file first, Decks' own analysis back when allowed, and
// nothing kept for a file the track no longer has.

#include "crate/stems/stemsync.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <vector>

#include "crate/client/crateclient.h"
#include "crate/stems/stemsidecar.h"
#include "test/crate/fakecratenetwork.h"
#include "test/crate/wavwriter.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"

namespace {

using crate::StemSidecar;
using crate::StemSync;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeReply;

const QString kServer = QStringLiteral("srv-1");
const QString kUid = QStringLiteral("0f6b3d9e-5a1c-4e2b-8d7f-1a2b3c4d5e61");
const QString kRevision = QString(64, QLatin1Char('a'));
const QString kNewRevision = QString(64, QLatin1Char('b'));
constexpr int kWaitMs = 10000;

/// The original of the server stem fixture
/// (tools/crate-stems/make_server_stem_fixture.sh): three tones, 2 s.
double originalSample(SINT frame) {
    const double t = static_cast<double>(frame) / 44100.0;
    return 0.3 * std::sin(2.0 * std::numbers::pi * 60.0 * t) +
            0.3 * std::sin(2.0 * std::numbers::pi * 110.0 * t) +
            0.2 * std::sin(2.0 * std::numbers::pi * 440.0 * t);
}

double rms(const std::vector<float>& interleaved8, int stem) {
    double squares = 0.0;
    const std::size_t frames = interleaved8.size() / 8;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double sample = interleaved8[8 * frame + 2 * static_cast<std::size_t>(stem)];
        squares += sample * sample;
    }
    return std::sqrt(squares / static_cast<double>(frames));
}

class StemSyncTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        crate::ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        options.schedule = m_scheduler.function();
        options.random = [] { return 0.5; };
        m_pClient = std::make_unique<crate::Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, options);
        m_pClient->setTokenSource(&m_tokens);
        m_cachedPath = m_dir.filePath(QStringLiteral("0f/") + kUid + QStringLiteral(".wav"));
        ASSERT_TRUE(QDir().mkpath(m_dir.filePath(QStringLiteral("0f"))));
        crate::test::writeWav(m_cachedPath, 44100, 2 * 44100, [](SINT frame, int channel) {
            Q_UNUSED(channel);
            return static_cast<int>(std::lround(originalSample(frame) * 32767.0));
        });
    }

    std::unique_ptr<StemSync> makeSync() {
        StemSync::Options options;
        options.clientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pClient.get() : nullptr;
        };
        options.mayUpload = [this](const QString&) {
            return m_mayUpload;
        };
        options.currentRevision = [this](const QString&, const QString&) {
            return m_currentRevision;
        };
        return std::make_unique<StemSync>(options);
    }

    FakeReply* waitForRequest(const QString& pathEnd) {
        FakeReply* pFound = nullptr;
        const bool found = QTest::qWaitFor(
                [&] {
                    for (const auto& sent : m_network.sent) {
                        if (sent.request.url().path().endsWith(pathEnd) && sent.pReply &&
                                !sent.pReply->isFinished()) {
                            pFound = sent.pReply.data();
                            return true;
                        }
                    }
                    return false;
                },
                kWaitMs);
        return found ? pFound : nullptr;
    }

    QByteArray fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/stems/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    }

    /// Answers the two requests of a fetch: the analysis, then the file.
    void answer(int analysisStatus,
            const QByteArray& analysis,
            int stemsStatus,
            const QByteArray& stems) {
        FakeReply* pAnalysis = waitForRequest(QStringLiteral("/stem-analysis"));
        ASSERT_NE(nullptr, pAnalysis);
        pAnalysis->respond(analysisStatus,
                analysis,
                {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}});
        FakeReply* pStems = waitForRequest(QStringLiteral("/stems"));
        ASSERT_NE(nullptr, pStems);
        pStems->respond(stemsStatus,
                stems,
                {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/mp4")},
                        {QByteArrayLiteral("Content-Length"),
                                QByteArray::number(stems.size())}});
    }

    QTemporaryDir m_dir;
    FakeNetworkAccessManager m_network;
    crate::test::FakeScheduler m_scheduler;
    crate::test::FakeTokenSource m_tokens;
    std::unique_ptr<crate::Client> m_pClient;
    QString m_cachedPath;
    bool m_mayUpload = false;
    QString m_currentRevision = kRevision;
};

} // namespace

// The server keeps a stem file of the track: Decks writes its sidecar from
// it, with `other` the residual against its own decode, so the four stems
// add up to the original and the server's three are what they hold.
TEST_F(StemSyncTest, PrefersTheServerStemFileAndRebuildsOtherAsTheResidual) {
    const auto pSync = makeSync();
    std::optional<StemSync::Outcome> outcome;
    pSync->fetch(kServer, kUid, m_cachedPath, kRevision, [&](const StemSync::Outcome& done) {
        outcome = done;
    });
    answer(404,
            QByteArrayLiteral(R"({"detail":"No stem analysis"})"),
            200,
            fixture(QStringLiteral("server-stems.mp4")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.has_value(); }, kWaitMs));
    ASSERT_EQ(StemSync::Outcome::Result::ServerStems, outcome->result)
            << outcome->error.toStdString();
    EXPECT_FALSE(outcome->serverAnalysis.has_value());
    EXPECT_EQ(StemSidecar::pathFor(m_cachedPath), outcome->sidecarPath);
    // The download does not stay
    EXPECT_FALSE(QFile::exists(outcome->sidecarPath + QStringLiteral(".server.part")));

    QString error;
    EXPECT_TRUE(StemSidecar::isValidFor(Track::newTemporary(m_cachedPath), &error))
            << error.toStdString();
    const auto info = StemSidecar::readInfo(outcome->sidecarPath);
    ASSERT_TRUE(info);
    EXPECT_EQ(kRevision, info->sourceRevision);
    std::vector<float> stems;
    SINT frames = 0;
    ASSERT_TRUE(StemSidecar::decode(outcome->sidecarPath, &stems, &frames, &error));
    ASSERT_EQ(2 * 44100, frames);
    // The stems add up to the original: a 16-bit WAV, exactly
    const double fullScale = 8388608.0 * StemSidecar::kScale;
    for (SINT frame = 0; frame < frames; ++frame) {
        long long sum = 0;
        for (int stem = 0; stem < 4; ++stem) {
            sum += std::llround(stems[static_cast<std::size_t>(8 * frame + 2 * stem)] * fullScale);
        }
        const double original = std::lround(originalSample(frame) * 32767.0) / 32768.0;
        ASSERT_EQ(original, static_cast<double>(sum) / fullScale) << "frame " << frame;
    }
    // The server's stems carry the music: the residual is what lossy coding
    // and any decoder offset leave
    EXPECT_NEAR(0.3 / std::numbers::sqrt2, rms(stems, 0), 0.03);
    EXPECT_NEAR(0.3 / std::numbers::sqrt2, rms(stems, 1), 0.03);
    EXPECT_NEAR(0.2 / std::numbers::sqrt2, rms(stems, 3), 0.03);
    EXPECT_LT(rms(stems, 2), 0.05);
}

// No stem file on the server: Decks separates the track itself; the
// server's analysis of the track's current file still comes back.
TEST_F(StemSyncTest, ReportsNoServerStemsWithTheServerAnalysis) {
    const auto pSync = makeSync();
    std::optional<StemSync::Outcome> outcome;
    pSync->fetch(kServer, kUid, m_cachedPath, QString(64, QLatin1Char('5')), [&](const auto& done) {
        outcome = done;
    });
    m_currentRevision = QString(64, QLatin1Char('5'));
    answer(200, fixture(QStringLiteral("stem-analysis-fixture.json")), 404, QByteArray());
    ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.has_value(); }, kWaitMs));
    EXPECT_EQ(StemSync::Outcome::Result::NoServerStems, outcome->result);
    ASSERT_TRUE(outcome->serverAnalysis.has_value());
    EXPECT_FALSE(outcome->serverAnalysis->isFromDecks());
    EXPECT_FALSE(QFile::exists(StemSidecar::pathFor(m_cachedPath)));
}

TEST_F(StemSyncTest, DoesNotUploadWithoutTheCapability) {
    const auto pSync = makeSync();
    crate::StemAnalysis analysis =
            *crate::StemAnalysis::fromJson(fixture(QStringLiteral("stem-analysis-fixture.json")));
    analysis.producer = QStringLiteral("crate-decks/1.0.0");
    analysis.sourceRevision = kRevision;

    std::optional<StemSync::UploadResult> result;
    pSync->upload(kServer, kUid, analysis, [&](StemSync::UploadResult done) {
        result = done;
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(StemSync::UploadResult::NotUploaded, *result);
    EXPECT_TRUE(m_network.sent.isEmpty());

    // With `stem_upload`, it goes up
    m_mayUpload = true;
    result.reset();
    pSync->upload(kServer, kUid, analysis, [&](StemSync::UploadResult done) {
        result = done;
    });
    FakeReply* pPut = waitForRequest(QStringLiteral("/stem-analysis"));
    ASSERT_NE(nullptr, pPut);
    EXPECT_EQ(QNetworkAccessManager::PutOperation, m_network.last().operation);
    const QJsonObject sent = QJsonDocument::fromJson(m_network.last().body).object();
    EXPECT_EQ(QStringLiteral("crate-decks/1.0.0"),
            sent.value(QStringLiteral("producer")).toString());
    EXPECT_EQ(kRevision, sent.value(QStringLiteral("sourceRevision")).toString());
    pPut->respond(200,
            QByteArrayLiteral(R"({"result":"kept_stronger"})"),
            {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return result.has_value(); }, kWaitMs));
    EXPECT_EQ(StemSync::UploadResult::KeptStronger, *result);
}

TEST_F(StemSyncTest, DiscardsResultsForAChangedSourceRevision) {
    const auto pSync = makeSync();
    // The server's analysis is of another revision: not the track's file
    std::optional<StemSync::Outcome> outcome;
    pSync->fetch(kServer, kUid, m_cachedPath, kRevision, [&](const auto& done) {
        outcome = done;
    });
    answer(200, fixture(QStringLiteral("stem-analysis-fixture.json")), 404, QByteArray());
    ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.has_value(); }, kWaitMs));
    EXPECT_EQ(StemSync::Outcome::Result::NoServerStems, outcome->result);
    EXPECT_FALSE(outcome->serverAnalysis.has_value());

    // The track changes revision while its sidecar is made from the
    // server's stems: nothing stays
    outcome.reset();
    pSync->fetch(kServer, kUid, m_cachedPath, kRevision, [&](const auto& done) {
        outcome = done;
    });
    m_currentRevision = kNewRevision;
    answer(404, QByteArray(), 200, fixture(QStringLiteral("server-stems.mp4")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.has_value(); }, kWaitMs));
    EXPECT_EQ(StemSync::Outcome::Result::Discarded, outcome->result);
    EXPECT_FALSE(QFile::exists(StemSidecar::pathFor(m_cachedPath)));

    // Nor does an upload of an analysis of the old revision go up
    m_mayUpload = true;
    crate::StemAnalysis analysis =
            *crate::StemAnalysis::fromJson(fixture(QStringLiteral("stem-analysis-fixture.json")));
    analysis.producer = QStringLiteral("crate-decks/1.0.0");
    analysis.sourceRevision = kRevision;
    const qsizetype sent = m_network.sent.size();
    std::optional<StemSync::UploadResult> result;
    pSync->upload(kServer, kUid, analysis, [&](StemSync::UploadResult done) {
        result = done;
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(StemSync::UploadResult::NotUploaded, *result);
    EXPECT_EQ(sent, m_network.sent.size());
}
