// Session pins (design §9, decided by Diego on 2026-11-11): the Crate tracks
// in a player or in the Auto DJ queue stay in the audio cache when its limit
// drops or it is cleared, until they are ejected, replaced or dequeued.

#include "crate/library/cratesessionpins.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "crate/client/crateclient.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/trackcollection.h"
#include "mixer/playerinfo.h"
#include "test/crate/fakecratenetwork.h"
#include "test/librarytest.h"
#include "track/track.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::cache::AudioCache;
using crate::cache::StorageInfo;
using crate::library::CrateSessionPins;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");
const QString kTrackFile = QStringLiteral("id3-test-data/cover-test-øé~ł€˚-png.mp3");
const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");
const QString kSampler1 = QStringLiteral("[Sampler1]");
constexpr int kWaitMs = 5000;

class FakeStorageInfo : public StorageInfo {
  public:
    qint64 bytesAvailable(const QString&) const override {
        return 100 * AudioCache::kGiB;
    }
};

class CrateSessionPinsTest : public LibraryTest {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_cacheDir.isValid());
        // The controls PlayerInfo reads; CoreServices creates them first
        for (const auto& [group, item] : {
                     std::pair{QStringLiteral("[Master]"), QStringLiteral("crossfader")},
                     std::pair{QStringLiteral("[App]"), QStringLiteral("num_decks")},
                     std::pair{QStringLiteral("[App]"), QStringLiteral("num_samplers")},
                     std::pair{QStringLiteral("[App]"),
                             QStringLiteral("num_preview_decks")}}) {
            m_controls.push_back(std::make_unique<ControlObject>(ConfigKey(group, item)));
        }
        PlayerInfo::create();

        QFile audio(getTestDir().filePath(kTrackFile));
        ASSERT_TRUE(audio.open(QIODevice::ReadOnly));
        m_audio = audio.readAll();
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        options.schedule = m_scheduler.function();
        options.random = [] { return 0.5; };
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, options);
        m_pClient->setTokenSource(&m_tokens);
        AudioCache::Options cacheOptions;
        cacheOptions.directory = m_cacheDir.path();
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
        m_pPins.reset();
        m_pCache.reset();
        PlayerInfo::destroy();
    }

    CrateSessionPins* makePins() {
        CrateSessionPins::Options options;
        options.pCache = m_pCache.get();
        options.pDbConnectionPool = dbConnectionPooler();
        options.pPlaylistDao = &internalCollection()->getPlaylistDAO();
        options.pPlayerInfo = &PlayerInfo::instance();
        options.settleTime = std::chrono::milliseconds(1);
        m_pPins = std::make_unique<CrateSessionPins>(std::move(options));
        return m_pPins.get();
    }

    void waitForRefresh() {
        QSignalSpy refreshed(m_pPins.get(), &CrateSessionPins::refreshed);
        ASSERT_TRUE(refreshed.wait(kWaitMs));
    }

    QString crateFile(const QString& uid) const {
        return m_cacheDir.filePath(kServer + QLatin1Char('/') + uid.left(2) + QLatin1Char('/') +
                uid + QStringLiteral(".mp3"));
    }

    /// Downloads the Crate track `uid` into the cache.
    void cache(const QString& uid) {
        crate::cache::TrackHints hints;
        hints.format = QStringLiteral("mp3");
        m_pCache->ensure(kServer, uid, crate::cache::Priority::Prefetch, hints);
        const QString path = QStringLiteral("/api/tracks/by-entity/%1/stream").arg(uid);
        crate::test::FakeReply* pReply = nullptr;
        ASSERT_TRUE(QTest::qWaitFor(
                [&] {
                    for (const auto& sent : std::as_const(m_network.sent)) {
                        if (sent.request.url().path() == path && sent.pReply &&
                                !sent.pReply->isFinished()) {
                            pReply = sent.pReply.data();
                        }
                    }
                    return pReply != nullptr;
                },
                kWaitMs));
        pReply->respond(200,
                m_audio,
                {{QByteArrayLiteral("Content-Length"), QByteArray::number(m_audio.size())},
                        {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/mpeg")}});
        ASSERT_TRUE(QTest::qWaitFor(
                [&] { return m_pCache->cachedPath(kServer, uid).has_value(); }, kWaitMs));
    }

    /// The library track of the cached Crate track `uid`.
    TrackPointer cachedTrack(const QString& uid) {
        cache(uid);
        return getOrAddTrackByLocation(crateFile(uid));
    }

    /// A library track whose file is not there: a queued Crate track that is
    /// not cached yet.
    TrackId uncachedTrack(const QString& uid) {
        const QString path = crateFile(uid);
        QDir().mkpath(QFileInfo(path).absolutePath());
        EXPECT_TRUE(QFile::copy(getTestDir().filePath(kTrackFile), path));
        const TrackPointer pTrack = getOrAddTrackByLocation(path);
        EXPECT_TRUE(QFile::remove(path));
        return pTrack ? pTrack->getId() : TrackId();
    }

    TrackPointer localTrack(const QString& name) {
        const QString path = m_musicDir.filePath(name);
        EXPECT_TRUE(QFile::copy(getTestDir().filePath(kTrackFile), path));
        return getOrAddTrackByLocation(path);
    }

    QString key(const QString& uid) const {
        return kServer + QLatin1Char('/') + uid;
    }

    /// Whether the file of `uid` is still cached after the limit drops to
    /// nothing and the cache is cleared.
    bool survivesEviction(const QString& uid) {
        m_pCache->setLimitBytes(0);
        m_pCache->clear();
        return m_pCache->cachedPath(kServer, uid).has_value();
    }

    QTemporaryDir m_cacheDir;
    QTemporaryDir m_musicDir;
    std::vector<std::unique_ptr<ControlObject>> m_controls;
    QByteArray m_audio;
    FakeNetworkAccessManager m_network;
    FakeScheduler m_scheduler;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<AudioCache> m_pCache;
    std::unique_ptr<CrateSessionPins> m_pPins;
    int m_autoDj = -1;
};

TEST_F(CrateSessionPinsTest, PinsTrackLoadedInDeck) {
    // Loaded before the pins start: a sampler's
    const TrackPointer pInSampler = cachedTrack(QStringLiteral("e2"));
    PlayerInfo::instance().setTrackInfo(kSampler1, pInSampler);
    makePins();
    const TrackPointer pInDeck = cachedTrack(QStringLiteral("e1"));
    PlayerInfo::instance().setTrackInfo(kDeck1, pInDeck);
    // A local track is none of the cache's business
    PlayerInfo::instance().setTrackInfo(kDeck2, localTrack(QStringLiteral("local.mp3")));

    EXPECT_EQ((QStringList{key(QStringLiteral("e1")), key(QStringLiteral("e2"))}),
            m_pPins->pinnedTracks());
    // A lower limit and "Clear cache" leave them
    EXPECT_TRUE(survivesEviction(QStringLiteral("e1")));
    EXPECT_TRUE(survivesEviction(QStringLiteral("e2")));
    EXPECT_TRUE(QFileInfo::exists(crateFile(QStringLiteral("e1"))));
    EXPECT_EQ(2 * m_audio.size(), m_pCache->usage().inUseBytes);
    EXPECT_EQ(0, m_pCache->usage().pinnedBytes);
}

TEST_F(CrateSessionPinsTest, PinsQueuedAutoDjTracks) {
    PlaylistDAO& playlists = internalCollection()->getPlaylistDAO();
    const TrackPointer pCached = cachedTrack(QStringLiteral("e1"));
    const TrackId uncached = uncachedTrack(QStringLiteral("e2"));
    ASSERT_TRUE(uncached.isValid());
    playlists.appendTrackToPlaylist(localTrack(QStringLiteral("local.mp3"))->getId(), m_autoDj);
    playlists.appendTrackToPlaylist(pCached->getId(), m_autoDj);
    playlists.appendTrackToPlaylist(uncached, m_autoDj);

    makePins();
    waitForRefresh();
    // The whole queue, also the track that is not cached yet
    EXPECT_EQ((QStringList{key(QStringLiteral("e1")), key(QStringLiteral("e2"))}),
            m_pPins->pinnedTracks());
    EXPECT_TRUE(survivesEviction(QStringLiteral("e1")));

    // The queued track downloads later: it is kept from then on
    m_pCache->setLimitBytes(20 * AudioCache::kGiB);
    cache(QStringLiteral("e2"));
    EXPECT_TRUE(survivesEviction(QStringLiteral("e2")));
}

TEST_F(CrateSessionPinsTest, UnpinsWhenEjectedOrDequeued) {
    PlaylistDAO& playlists = internalCollection()->getPlaylistDAO();
    const TrackPointer pPlaying = cachedTrack(QStringLiteral("e1"));
    const TrackPointer pQueued = cachedTrack(QStringLiteral("e2"));
    const TrackPointer pReplaced = cachedTrack(QStringLiteral("e3"));
    // Auto DJ's next track is in deck 1 and still first in the queue
    playlists.appendTrackToPlaylist(pPlaying->getId(), m_autoDj);
    playlists.appendTrackToPlaylist(pQueued->getId(), m_autoDj);
    makePins();
    waitForRefresh();
    PlayerInfo::instance().setTrackInfo(kDeck1, pPlaying);
    PlayerInfo::instance().setTrackInfo(kDeck2, pReplaced);
    ASSERT_EQ((QStringList{key(QStringLiteral("e1")),
                      key(QStringLiteral("e2")),
                      key(QStringLiteral("e3"))}),
            m_pPins->pinnedTracks());

    // It leaves the queue, as Auto DJ does when it plays it: the deck keeps
    // it
    playlists.removeTrackFromPlaylist(m_autoDj, 1);
    waitForRefresh();
    EXPECT_TRUE(survivesEviction(QStringLiteral("e1")));
    // Deck 2 gets another track: the one it had goes
    PlayerInfo::instance().setTrackInfo(kDeck2, localTrack(QStringLiteral("local.mp3")));
    EXPECT_FALSE(survivesEviction(QStringLiteral("e3")));
    // Ejected from deck 1
    PlayerInfo::instance().setTrackInfo(kDeck1, TrackPointer());
    EXPECT_FALSE(survivesEviction(QStringLiteral("e1")));
    EXPECT_TRUE(survivesEviction(QStringLiteral("e2")));
    // Dequeued
    playlists.removeTrackFromPlaylist(m_autoDj, 1);
    waitForRefresh();
    EXPECT_TRUE(m_pPins->pinnedTracks().isEmpty());
    EXPECT_FALSE(survivesEviction(QStringLiteral("e2")));
    EXPECT_EQ(0, m_pCache->usage().inUseBytes);
}

} // namespace
