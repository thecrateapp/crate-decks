#include "crate/library/crateprefetcher.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

#include "crate/client/crateclient.h"
#include "library/autodj/autodjprocessor.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/trackcollection.h"
#include "test/crate/fakecratenetwork.h"
#include "test/librarytest.h"
#include "track/track.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::cache::AudioCache;
using crate::cache::StorageInfo;
using crate::library::CratePrefetcher;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeReply;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");
const QString kTrackFile = QStringLiteral("id3-test-data/cover-test-øé~ł€˚-png.mp3");
constexpr int kWaitMs = 5000;

class FakeStorageInfo : public StorageInfo {
  public:
    qint64 bytesAvailable(const QString&) const override {
        return 100 * AudioCache::kGiB;
    }
};

QString uidOf(int number) {
    return QStringLiteral("%1a2b3c4d-5a1c-4e2b-8d7f-1a2b3c4d5e6%1").arg(number);
}

class CratePrefetcherTest : public LibraryTest {
  protected:
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

        PlaylistDAO& playlists = internalCollection()->getPlaylistDAO();
        m_autoDj = playlists.getPlaylistIdFromName(AUTODJ_TABLE);
        if (m_autoDj < 0) {
            m_autoDj = playlists.createPlaylist(AUTODJ_TABLE, PlaylistDAO::PLHT_AUTO_DJ);
        }
        ASSERT_GE(m_autoDj, 0);
    }

    void TearDown() override {
        m_pPrefetcher.reset();
        m_pCache.reset();
    }

    CratePrefetcher* makePrefetcher() {
        CratePrefetcher::Options options;
        options.pCache = m_pCache.get();
        options.pDbConnectionPool = dbConnectionPooler();
        options.pPlaylistDao = &internalCollection()->getPlaylistDAO();
        options.settleTime = std::chrono::milliseconds(1);
        m_pPrefetcher = std::make_unique<CratePrefetcher>(std::move(options));
        return m_pPrefetcher.get();
    }

    /// A library track whose file is `path`, which is not there: a Crate
    /// track the cache evicted, or any missing track.
    TrackId addMissingTrack(const QString& path) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        EXPECT_TRUE(QFile::copy(getTestDir().filePath(kTrackFile), path));
        const TrackPointer pTrack = getOrAddTrackByLocation(path);
        EXPECT_TRUE(QFile::remove(path));
        return pTrack ? pTrack->getId() : TrackId();
    }

    QString crateFile(int number) const {
        const QString uid = uidOf(number);
        return m_dir.filePath(
                kServer + QLatin1Char('/') + uid.left(2) + QLatin1Char('/') + uid + ".mp3");
    }

    QString key(int number) const {
        return kServer + QLatin1Char('/') + uidOf(number);
    }

    bool sentFor(int number) const {
        const QString path = QStringLiteral("/api/tracks/by-entity/%1/stream").arg(uidOf(number));
        for (const auto& sent : m_network.sent) {
            if (sent.request.url().path() == path) {
                return true;
            }
        }
        return false;
    }

    void waitForRefresh() {
        QSignalSpy refreshed(m_pPrefetcher.get(), &CratePrefetcher::refreshed);
        ASSERT_TRUE(refreshed.wait(kWaitMs));
    }

    QTemporaryDir m_dir;
    QTemporaryDir m_musicDir;
    FakeNetworkAccessManager m_network;
    FakeScheduler m_scheduler;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<AudioCache> m_pCache;
    std::unique_ptr<CratePrefetcher> m_pPrefetcher;
    int m_autoDj = -1;
};

TEST_F(CratePrefetcherTest, FetchesTheNextThreeCrateTracksOfAutoDj) {
    PlaylistDAO& playlists = internalCollection()->getPlaylistDAO();
    const TrackId local = addMissingTrack(m_musicDir.filePath(QStringLiteral("local.mp3")));
    playlists.appendTrackToPlaylist(local, m_autoDj);
    for (int number = 1; number <= 4; ++number) {
        playlists.appendTrackToPlaylist(addMissingTrack(crateFile(number)), m_autoDj);
    }
    makePrefetcher();
    waitForRefresh();

    // The first three places: a local track, which is left alone, and two
    // Crate ones
    EXPECT_EQ((QStringList{key(1), key(2)}), m_pPrefetcher->prefetchedTracks());
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentFor(1) && sentFor(2); }, kWaitMs));
    EXPECT_FALSE(sentFor(3));

    // The local track leaves the queue: the third Crate track moves up
    playlists.removeTrackFromPlaylist(m_autoDj, 1);
    waitForRefresh();
    EXPECT_EQ((QStringList{key(1), key(2), key(3)}), m_pPrefetcher->prefetchedTracks());
    EXPECT_EQ(crate::cache::TrackState::Kind::Queued,
            m_pCache->trackState(kServer, uidOf(3)).kind);

    // A track that leaves the first places stops its download
    playlists.removeTrackFromPlaylist(m_autoDj, 1);
    waitForRefresh();
    EXPECT_EQ((QStringList{key(2), key(3), key(4)}), m_pPrefetcher->prefetchedTracks());
    EXPECT_EQ(crate::cache::TrackState::Kind::None, m_pCache->trackState(kServer, uidOf(1)).kind);
}

TEST_F(CratePrefetcherTest, AnswersAutoDjForCrateTracksOnly) {
    makePrefetcher();
    const TrackId crate = addMissingTrack(crateFile(1));
    const TrackId local = addMissingTrack(m_musicDir.filePath(QStringLiteral("local.mp3")));
    ASSERT_TRUE(crate.isValid());
    ASSERT_TRUE(local.isValid());

    // A missing Crate file is on its way: the cache fetches it
    EXPECT_TRUE(m_pPrefetcher->isOnItsWay(crateFile(1)));
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentFor(1); }, kWaitMs));
    EXPECT_TRUE(m_pPrefetcher->isOnItsWay(crateFile(1)));
    // Any other missing file is not
    EXPECT_FALSE(m_pPrefetcher->isOnItsWay(m_musicDir.filePath(QStringLiteral("local.mp3"))));

    // A download that fails is not waited for again
    for (const auto& sent : std::as_const(m_network.sent)) {
        if (sent.pReply && sent.request.url().path().contains(uidOf(1))) {
            sent.pReply->respond(404, QByteArrayLiteral(R"({"detail": "gone"})"));
        }
    }
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                const auto kind = m_pCache->trackState(kServer, uidOf(1)).kind;
                return kind != crate::cache::TrackState::Kind::Queued &&
                        kind != crate::cache::TrackState::Kind::Downloading;
            },
            kWaitMs));
    QTest::qWait(10);
    EXPECT_FALSE(m_pPrefetcher->isOnItsWay(crateFile(1)));
}

} // namespace
