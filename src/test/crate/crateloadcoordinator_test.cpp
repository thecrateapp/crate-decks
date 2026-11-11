#include "crate/library/crateloadcoordinator.h"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "crate/client/crateclient.h"
#include "preferences/dialog/dlgprefdeck.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxtest.h"
#include "track/track.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::cache::AudioCache;
using crate::cache::Priority;
using crate::cache::StorageInfo;
using crate::cache::TrackHints;
using crate::library::CrateLoadCoordinator;
using crate::library::CrateTrackRef;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeReply;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");
const QString kUid1 = QStringLiteral("0f6b3d9e-5a1c-4e2b-8d7f-1a2b3c4d5e61");
const QString kUid2 = QStringLiteral("1a2b3c4d-5a1c-4e2b-8d7f-1a2b3c4d5e62");
const QString kUid3 = QStringLiteral("2b3c4d5e-5a1c-4e2b-8d7f-1a2b3c4d5e63");
const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");
constexpr int kWaitMs = 5000;

class FakeStorageInfo : public StorageInfo {
  public:
    qint64 bytesAvailable(const QString&) const override {
        return 100 * AudioCache::kGiB;
    }
};

FakeReply::Headers audioHeaders(qint64 contentLength) {
    return {{QByteArrayLiteral("Content-Length"), QByteArray::number(contentLength)},
            {QByteArrayLiteral("ETag"), QByteArrayLiteral("\"e1\"")},
            {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/flac")}};
}

CrateTrackRef trackRef(const QString& uid) {
    CrateTrackRef track;
    track.serverId = kServer;
    track.entityUid = uid;
    track.hints.format = QStringLiteral("flac");
    return track;
}

/// The loads the coordinator asked for.
struct Load {
    QString location;
    QString group;
    bool play = false;
};

class CrateLoadCoordinatorTest : public MixxxTest {
  protected:
    CrateLoadCoordinatorTest()
            : m_play1(ConfigKey(kDeck1, QStringLiteral("play"))),
              m_play2(ConfigKey(kDeck2, QStringLiteral("play"))) {
    }

    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        options.schedule = m_scheduler.function();
        options.random = [] { return 0.5; };
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, options);
        m_pClient->setTokenSource(&m_tokens);

        AudioCache::Options cacheOptions;
        cacheOptions.directory = m_dir.path();
        cacheOptions.clientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pClient.get() : nullptr;
        };
        cacheOptions.pStorageInfo = std::make_shared<FakeStorageInfo>();
        m_pCache = std::make_unique<AudioCache>(std::move(cacheOptions));
        ASSERT_TRUE(QTest::qWaitFor([this] { return m_pCache->isLoaded(); }, kWaitMs));

        CrateLoadCoordinator::Options coordinatorOptions;
        coordinatorOptions.pCache = m_pCache.get();
        coordinatorOptions.pConfig = config();
        coordinatorOptions.trackOfFile = [](const QString& path) {
            return Track::newTemporary(path);
        };
        coordinatorOptions.applyAnalysis =
                [this](const TrackPointer&, const QString&, const QString& entityUid) {
                    m_analysed.append(entityUid);
                };
        m_pCoordinator = std::make_unique<CrateLoadCoordinator>(std::move(coordinatorOptions));
        QObject::connect(m_pCoordinator.get(),
                &CrateLoadCoordinator::loadTrackToPlayer,
                m_pCoordinator.get(),
                [this](const TrackPointer& pTrack, const QString& group, bool play) {
                    m_loads.append({pTrack->getLocation(), group, play});
                });
    }

    void TearDown() override {
        m_pCoordinator.reset();
        m_pCache.reset();
    }

    /// The download request of `uid`, once the coordinator sent it.
    FakeReply* waitForDownload(const QString& uid) {
        const QString path = QStringLiteral("/api/tracks/by-entity/%1/stream").arg(uid);
        FakeReply* pFound = nullptr;
        QTest::qWaitFor(
                [&] {
                    for (const auto& sent : std::as_const(m_network.sent)) {
                        if (sent.request.url().path() == path && sent.pReply) {
                            pFound = sent.pReply.data();
                        }
                    }
                    return pFound != nullptr;
                },
                kWaitMs);
        return pFound;
    }

    QString cachedPathOf(const QString& uid) const {
        return m_dir.filePath(kServer + QLatin1Char('/') + uid.left(2) + QLatin1Char('/') + uid +
                QStringLiteral(".flac"));
    }

    void complete(FakeReply* pReply) {
        const QByteArray body = QByteArrayLiteral("fLaC-audio");
        pReply->respond(200, body, audioHeaders(body.size()));
    }

    QTemporaryDir m_dir;
    FakeNetworkAccessManager m_network;
    FakeScheduler m_scheduler;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<AudioCache> m_pCache;
    std::unique_ptr<CrateLoadCoordinator> m_pCoordinator;
    ControlPushButton m_play1;
    ControlPushButton m_play2;
    QList<Load> m_loads;
    QStringList m_analysed;
};

TEST_F(CrateLoadCoordinatorTest, LoadsAfterDownloadCompletes) {
    EXPECT_EQ(nullptr, m_pCoordinator->cachedTrack(kServer, kUid1));

    m_pCoordinator->requestLoad(trackRef(kUid1), kDeck1, false);

    FakeReply* pReply = waitForDownload(kUid1);
    ASSERT_NE(nullptr, pReply);
    EXPECT_TRUE(m_pCoordinator->isLoadPending(kDeck1));
    // The row shows the download; the deck is not touched yet
    EXPECT_EQ(crate::cache::TrackState::Kind::Downloading,
            m_pCache->trackState(kServer, kUid1).kind);
    EXPECT_TRUE(m_loads.isEmpty());

    complete(pReply);

    ASSERT_TRUE(QTest::qWaitFor([this] { return !m_loads.isEmpty(); }, kWaitMs));
    ASSERT_EQ(1, m_loads.size());
    EXPECT_EQ(cachedPathOf(kUid1), m_loads[0].location);
    EXPECT_EQ(kDeck1, m_loads[0].group);
    EXPECT_FALSE(m_loads[0].play);
    EXPECT_FALSE(m_pCoordinator->isLoadPending(kDeck1));
    EXPECT_EQ(QStringList{kUid1}, m_analysed);

    // Cached now: a track at once, without another download or analysis
    const TrackPointer pCached = m_pCoordinator->cachedTrack(kServer, kUid1);
    ASSERT_NE(nullptr, pCached);
    EXPECT_EQ(cachedPathOf(kUid1), pCached->getLocation());
    EXPECT_EQ(QStringList{kUid1}, m_analysed);
}

TEST_F(CrateLoadCoordinatorTest, CancelsPendingLoadWhenDeckGetsAnotherTrack) {
    m_pCoordinator->requestLoad(trackRef(kUid1), kDeck1, false);
    FakeReply* pFirst = waitForDownload(kUid1);
    ASSERT_NE(nullptr, pFirst);
    m_pCoordinator->requestLoad(trackRef(kUid3), kDeck2, false);
    ASSERT_NE(nullptr, waitForDownload(kUid3));

    // The DJ picks another track for deck 1 while the first downloads
    m_pCoordinator->requestLoad(trackRef(kUid2), kDeck1, true);

    // Nobody waits for the first download any more: it stops
    EXPECT_TRUE(QTest::qWaitFor([pFirst] { return pFirst->aborted(); }, kWaitMs));
    FakeReply* pSecond = waitForDownload(kUid2);
    ASSERT_NE(nullptr, pSecond);
    complete(pSecond);
    ASSERT_TRUE(QTest::qWaitFor([this] { return !m_loads.isEmpty(); }, kWaitMs));
    ASSERT_EQ(1, m_loads.size());
    EXPECT_EQ(cachedPathOf(kUid2), m_loads[0].location);
    EXPECT_EQ(kDeck1, m_loads[0].group);
    EXPECT_TRUE(m_loads[0].play);
    // The other deck's load goes on
    EXPECT_TRUE(m_pCoordinator->isLoadPending(kDeck2));
}

TEST_F(CrateLoadCoordinatorTest, DoesNotInterruptPlayingDeckWithoutConfirmation) {
    // Mixxx's default: a playing deck rejects new tracks
    config()->setValue(kConfigKeyLoadWhenDeckPlaying, static_cast<int>(LoadWhenDeckPlaying::Reject));
    m_pCoordinator->requestLoad(trackRef(kUid1), kDeck1, false);
    FakeReply* pReply = waitForDownload(kUid1);
    ASSERT_NE(nullptr, pReply);

    // The DJ starts the deck while the track downloads
    m_play1.set(1.0);
    complete(pReply);

    ASSERT_TRUE(QTest::qWaitFor([this] { return m_pCoordinator->isCached(kServer, kUid1); },
            kWaitMs));
    QTest::qWait(50);
    EXPECT_TRUE(m_loads.isEmpty());
    EXPECT_FALSE(m_pCoordinator->isLoadPending(kDeck1));

    // When the preferences allow loading into a playing deck, it loads
    config()->setValue(kConfigKeyLoadWhenDeckPlaying, static_cast<int>(LoadWhenDeckPlaying::Allow));
    m_pCoordinator->requestLoad(trackRef(kUid2), kDeck1, false);
    FakeReply* pAllowed = waitForDownload(kUid2);
    ASSERT_NE(nullptr, pAllowed);
    complete(pAllowed);
    ASSERT_TRUE(QTest::qWaitFor([this] { return !m_loads.isEmpty(); }, kWaitMs));
    EXPECT_EQ(cachedPathOf(kUid2), m_loads[0].location);

    // The preview deck always takes it
    EXPECT_TRUE(CrateLoadCoordinator::mayLoadInto(QStringLiteral("[PreviewDeck1]"), config()));
    config()->setValue(kConfigKeyLoadWhenDeckPlaying, static_cast<int>(LoadWhenDeckPlaying::Reject));
    EXPECT_FALSE(CrateLoadCoordinator::mayLoadInto(kDeck1, config()));
    EXPECT_TRUE(CrateLoadCoordinator::mayLoadInto(kDeck2, config()));
}

} // namespace
