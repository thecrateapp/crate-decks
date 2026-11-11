#include "crate/cache/audiocache.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>
#include <optional>

#include "crate/cache/audiocacheindex.h"
#include "crate/cache/cachepaths.h"
#include "crate/client/crateclient.h"
#include "test/crate/fakecratenetwork.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::Error;
using crate::cache::AudioCache;
using crate::cache::AudioCacheIndex;
using crate::cache::CacheEntry;
using crate::cache::CacheTicket;
using crate::cache::Priority;
using crate::cache::StorageInfo;
using crate::cache::TrackHints;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeReply;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;

constexpr qint64 kGiB = AudioCache::kGiB;
const QString kServer = QStringLiteral("srv-1");
const QString kUid1 = QStringLiteral("0f6b3d9e-5a1c-4e2b-8d7f-1a2b3c4d5e61");
const QString kUid2 = QStringLiteral("1a2b3c4d-5a1c-4e2b-8d7f-1a2b3c4d5e62");
const QString kUid3 = QStringLiteral("2b3c4d5e-5a1c-4e2b-8d7f-1a2b3c4d5e63");
const QString kUid4 = QStringLiteral("3c4d5e6f-5a1c-4e2b-8d7f-1a2b3c4d5e64");
const QByteArray kEtag1 = QByteArrayLiteral("\"e1\"");
const QByteArray kEtag2 = QByteArrayLiteral("\"e2\"");
constexpr int kWaitMs = 5000;

class FakeStorageInfo : public StorageInfo {
  public:
    qint64 free = 100 * kGiB;

    qint64 bytesAvailable(const QString&) const override {
        return free;
    }
};

/// What a ticket ended with, if it did.
struct Outcome {
    std::optional<QString> path;
    std::optional<Error> error;
    qint64 lastProgress = -1;

    explicit Outcome(CacheTicket* pTicket) {
        QObject::connect(pTicket, &CacheTicket::ready, pTicket, [this](const QString& readyPath) {
            path = readyPath;
        });
        QObject::connect(pTicket, &CacheTicket::failed, pTicket, [this](const Error& failure) {
            error = failure;
        });
        QObject::connect(pTicket,
                &CacheTicket::progress,
                pTicket,
                [this](qint64 bytesReceived, qint64) { lastProgress = bytesReceived; });
    }

    bool waitForReady() {
        return QTest::qWaitFor([this] { return path.has_value() || error.has_value(); },
                       kWaitMs) &&
                path.has_value();
    }
    bool waitForFailure() {
        return QTest::qWaitFor([this] { return path.has_value() || error.has_value(); },
                       kWaitMs) &&
                error.has_value();
    }
};

FakeReply::Headers audioHeaders(qint64 contentLength, const QByteArray& etag = kEtag1) {
    return {{QByteArrayLiteral("Content-Length"), QByteArray::number(contentLength)},
            {QByteArrayLiteral("ETag"), etag},
            {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/flac")},
            {QByteArrayLiteral("Accept-Ranges"), QByteArrayLiteral("bytes")}};
}

TrackHints flacHints(qint64 byteLength) {
    TrackHints hints;
    hints.format = QStringLiteral("flac");
    hints.byteLength = byteLength;
    return hints;
}

QByteArray contentsOf(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

class AudioCacheTest : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        options.schedule = m_scheduler.function();
        options.random = [] { return 0.5; };
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test/base")), &m_network, options);
        m_pClient->setTokenSource(&m_tokens);
    }

    std::unique_ptr<AudioCache> makeCache(qint64 limitBytes = 20 * kGiB) {
        AudioCache::Options options;
        options.directory = m_dir.path();
        options.clientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pClient.get() : nullptr;
        };
        options.limitBytes = limitBytes;
        options.pStorageInfo = m_pStorage;
        options.nowMs = [this] { return m_nowMs; };
        auto pCache = std::make_unique<AudioCache>(std::move(options));
        EXPECT_TRUE(QTest::qWaitFor([&] { return pCache->isLoaded(); }, kWaitMs));
        return pCache;
    }

    bool waitForRequests(int count) {
        return QTest::qWaitFor([&] { return m_network.sent.size() >= count; }, kWaitMs);
    }

    FakeReply* replyOf(int index) const {
        return m_network.sent.value(index).pReply.data();
    }

    QString finalPath(const QString& uid, const QString& extension = QStringLiteral("flac")) const {
        return m_dir.filePath(kServer + QLatin1Char('/') + uid.left(2) + QLatin1Char('/') + uid +
                QLatin1Char('.') + extension);
    }

    bool waitForPartSize(const QString& uid, qint64 size) const {
        const QString part = finalPath(uid) + QStringLiteral(".part");
        return QTest::qWaitFor([&] { return QFileInfo(part).size() == size; }, kWaitMs);
    }

    /// Downloads a whole track of `body`.
    void download(AudioCache* pCache, const QString& uid, const QByteArray& body) {
        Outcome outcome(pCache->ensure(kServer, uid, Priority::Prefetch, flacHints(body.size())));
        const int index = static_cast<int>(m_network.sent.size());
        ASSERT_TRUE(waitForRequests(index + 1));
        replyOf(index)->respond(200, body, audioHeaders(body.size()));
        ASSERT_TRUE(outcome.waitForReady());
    }

    QTemporaryDir m_dir;
    FakeNetworkAccessManager m_network;
    FakeScheduler m_scheduler;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::shared_ptr<FakeStorageInfo> m_pStorage = std::make_shared<FakeStorageInfo>();
    qint64 m_nowMs = 1000;
};

TEST_F(AudioCacheTest, DownloadsToPartThenRenames) {
    const auto pCache = makeCache();

    Outcome outcome(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));

    ASSERT_TRUE(waitForRequests(1));
    const QNetworkRequest& request = m_network.last().request;
    EXPECT_EQ(QStringLiteral("/base/api/tracks/by-entity/") + kUid1 + QStringLiteral("/stream"),
            request.url().path());
    // The token goes in the header, never in the URL
    EXPECT_FALSE(request.url().hasQuery());
    EXPECT_EQ(QByteArrayLiteral("Bearer access-1"), request.rawHeader("Authorization"));
    EXPECT_FALSE(request.hasRawHeader("Range"));
    FakeReply* pReply = replyOf(0);
    pReply->respondHeaders(200, audioHeaders(10));
    pReply->sendBody(QByteArrayLiteral("01234"));
    ASSERT_TRUE(waitForPartSize(kUid1, 5));
    EXPECT_FALSE(QFileInfo::exists(finalPath(kUid1)));
    EXPECT_FALSE(pCache->cachedPath(kServer, kUid1).has_value());
    pReply->sendBody(QByteArrayLiteral("56789"));
    pReply->finishBody();

    ASSERT_TRUE(outcome.waitForReady());
    EXPECT_EQ(finalPath(kUid1), *outcome.path);
    EXPECT_EQ(QByteArrayLiteral("0123456789"), contentsOf(finalPath(kUid1)));
    EXPECT_FALSE(QFileInfo::exists(finalPath(kUid1) + QStringLiteral(".part")));
    EXPECT_EQ(10, outcome.lastProgress);
    EXPECT_EQ(std::optional<QString>(finalPath(kUid1)), pCache->cachedPath(kServer, kUid1));
    EXPECT_EQ(10, pCache->usage().readyBytes);
    EXPECT_EQ(1, pCache->usage().readyFiles);
}

TEST_F(AudioCacheTest, ResumesWithRangeWhenEtagMatches) {
    {
        const auto pCache = makeCache();
        Outcome first(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));
        ASSERT_TRUE(waitForRequests(1));
        replyOf(0)->respondHeaders(200, audioHeaders(10));
        replyOf(0)->sendBody(QByteArrayLiteral("0123"));
        ASSERT_TRUE(waitForPartSize(kUid1, 4));
    } // The app stops: the part file stays

    const auto pCache = makeCache();
    Outcome second(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));

    ASSERT_TRUE(waitForRequests(2));
    const QNetworkRequest& request = m_network.last().request;
    EXPECT_EQ(QByteArrayLiteral("bytes=4-"), request.rawHeader("Range"));
    EXPECT_EQ(kEtag1, request.rawHeader("If-Range"));
    replyOf(1)->respond(206,
            QByteArrayLiteral("456789"),
            {{QByteArrayLiteral("Content-Range"), QByteArrayLiteral("bytes 4-9/10")},
                    {QByteArrayLiteral("Content-Length"), QByteArrayLiteral("6")},
                    {QByteArrayLiteral("ETag"), kEtag1}});
    ASSERT_TRUE(second.waitForReady());
    EXPECT_EQ(QByteArrayLiteral("0123456789"), contentsOf(finalPath(kUid1)));
    EXPECT_EQ(10, second.lastProgress);
}

TEST_F(AudioCacheTest, ResumesAfterBrokenTransfer) {
    const auto pCache = makeCache();
    Outcome outcome(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));
    ASSERT_TRUE(waitForRequests(1));
    replyOf(0)->respondHeaders(200, audioHeaders(10));
    replyOf(0)->sendBody(QByteArrayLiteral("0123"));

    replyOf(0)->breakOff();

    ASSERT_TRUE(waitForRequests(2));
    EXPECT_EQ(QByteArrayLiteral("bytes=4-"), m_network.last().request.rawHeader("Range"));
    EXPECT_EQ(kEtag1, m_network.last().request.rawHeader("If-Range"));
    replyOf(1)->respond(206,
            QByteArrayLiteral("456789"),
            {{QByteArrayLiteral("Content-Range"), QByteArrayLiteral("bytes 4-9/10")},
                    {QByteArrayLiteral("ETag"), kEtag1}});
    ASSERT_TRUE(outcome.waitForReady());
    EXPECT_EQ(QByteArrayLiteral("0123456789"), contentsOf(finalPath(kUid1)));
}

TEST_F(AudioCacheTest, RestartsWhenEtagChanged) {
    {
        const auto pCache = makeCache();
        Outcome first(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));
        ASSERT_TRUE(waitForRequests(1));
        replyOf(0)->respondHeaders(200, audioHeaders(10));
        replyOf(0)->sendBody(QByteArrayLiteral("0123"));
        ASSERT_TRUE(waitForPartSize(kUid1, 4));
    }

    const auto pCache = makeCache();
    Outcome second(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));

    ASSERT_TRUE(waitForRequests(2));
    EXPECT_EQ(kEtag1, m_network.last().request.rawHeader("If-Range"));
    // The file changed on the server: If-Range does not match, the whole
    // file comes
    replyOf(1)->respond(200, QByteArrayLiteral("abcdefghij"), audioHeaders(10, kEtag2));
    ASSERT_TRUE(second.waitForReady());
    EXPECT_EQ(QByteArrayLiteral("abcdefghij"), contentsOf(finalPath(kUid1)));
}

TEST_F(AudioCacheTest, RejectsShortDownload) {
    const auto pCache = makeCache();
    // The server's Content-Length...
    Outcome withLength(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));
    ASSERT_TRUE(waitForRequests(1));
    replyOf(0)->respond(200, QByteArrayLiteral("012345"), audioHeaders(10));
    // ...or, without it, the track's byte_length
    Outcome withoutLength(pCache->ensure(kServer, kUid2, Priority::Deck, flacHints(10)));
    ASSERT_TRUE(waitForRequests(2));
    replyOf(1)->respond(200,
            QByteArrayLiteral("0123"),
            {{QByteArrayLiteral("ETag"), kEtag1},
                    {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/flac")}});

    ASSERT_TRUE(withLength.waitForFailure());
    ASSERT_TRUE(withoutLength.waitForFailure());
    for (const QString& uid : {kUid1, kUid2}) {
        EXPECT_FALSE(QFileInfo::exists(finalPath(uid)));
        EXPECT_FALSE(QFileInfo::exists(finalPath(uid) + QStringLiteral(".part")));
        EXPECT_FALSE(pCache->cachedPath(kServer, uid).has_value());
    }
    EXPECT_EQ(0, pCache->usage().readyBytes);
    // A file that came short is not retried on its own
    QTest::qWait(50);
    EXPECT_EQ(2, m_network.sent.size());
}

TEST_F(AudioCacheTest, PrioritisesDeckOverPrefetch) {
    const auto pCache = makeCache();
    Outcome a(pCache->ensure(kServer, kUid1, Priority::Prefetch, flacHints(1)));
    Outcome b(pCache->ensure(kServer, kUid2, Priority::Prefetch, flacHints(1)));
    Outcome c(pCache->ensure(kServer, kUid3, Priority::OfflineSet, flacHints(1)));
    Outcome d(pCache->ensure(kServer, kUid4, Priority::Deck, flacHints(1)));

    // Two downloads per server
    ASSERT_TRUE(waitForRequests(2));
    QTest::qWait(50);
    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_TRUE(m_network.sent[0].request.url().path().contains(kUid1));
    EXPECT_TRUE(m_network.sent[1].request.url().path().contains(kUid2));
    EXPECT_EQ(2, pCache->usage().activeDownloads);
    EXPECT_EQ(2, pCache->usage().queuedDownloads);
    replyOf(0)->respond(200, QByteArrayLiteral("a"), audioHeaders(1));

    ASSERT_TRUE(waitForRequests(3));
    EXPECT_TRUE(m_network.sent[2].request.url().path().contains(kUid4));
    replyOf(1)->respond(200, QByteArrayLiteral("b"), audioHeaders(1));
    ASSERT_TRUE(waitForRequests(4));
    EXPECT_TRUE(m_network.sent[3].request.url().path().contains(kUid3));
}

TEST_F(AudioCacheTest, EvictsLruButNeverPinned) {
    const auto pCache = makeCache(35);
    QSignalSpy evicted(pCache.get(), &AudioCache::evicted);
    m_nowMs = 1000;
    download(pCache.get(), kUid1, QByteArrayLiteral("1111111111"));
    m_nowMs = 2000;
    download(pCache.get(), kUid2, QByteArrayLiteral("2222222222"));
    m_nowMs = 3000;
    download(pCache.get(), kUid3, QByteArrayLiteral("3333333333"));
    // Using a cached track makes it recent
    m_nowMs = 4000;
    Outcome used(pCache->ensure(kServer, kUid1, Priority::Deck));
    ASSERT_TRUE(used.waitForReady());
    EXPECT_EQ(3, m_network.sent.size());
    pCache->pin(kServer, kUid2, QStringLiteral("deck"));

    m_nowMs = 5000;
    download(pCache.get(), kUid4, QByteArrayLiteral("4444444444"));

    // The least recently used unpinned one goes: not 2 (pinned) but 3
    ASSERT_EQ(1, evicted.size());
    EXPECT_EQ(kUid3, evicted.first().at(1).toString());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !QFileInfo::exists(finalPath(kUid3)); }, kWaitMs));
    EXPECT_FALSE(pCache->cachedPath(kServer, kUid3).has_value());
    for (const QString& uid : {kUid1, kUid2, kUid4}) {
        EXPECT_TRUE(QFileInfo::exists(finalPath(uid))) << uid.toStdString();
        EXPECT_TRUE(pCache->cachedPath(kServer, uid).has_value());
    }
    EXPECT_EQ(30, pCache->usage().readyBytes);
    EXPECT_EQ(10, pCache->usage().pinnedBytes);

    // A lower limit evicts at once, still never the pinned one
    pCache->setLimitBytes(15);
    EXPECT_TRUE(pCache->cachedPath(kServer, kUid2).has_value());
    EXPECT_FALSE(pCache->cachedPath(kServer, kUid1).has_value());
    EXPECT_FALSE(pCache->cachedPath(kServer, kUid4).has_value());
    EXPECT_EQ(10, pCache->usage().readyBytes);
}

TEST_F(AudioCacheTest, KeepsTwoGiBFree) {
    const auto pCache = makeCache();
    download(pCache.get(), kUid1, QByteArrayLiteral("1111111111"));
    m_pStorage->free = 2 * kGiB + 5;

    // 10 bytes more would leave less than 2 GiB free: the cached file goes
    download(pCache.get(), kUid2, QByteArrayLiteral("2222222222"));
    EXPECT_FALSE(pCache->cachedPath(kServer, kUid1).has_value());
    EXPECT_TRUE(pCache->cachedPath(kServer, kUid2).has_value());

    // With nothing left to evict, the download fails instead
    pCache->pin(kServer, kUid2, QStringLiteral("deck"));
    Outcome tooBig(pCache->ensure(kServer, kUid3, Priority::Deck, flacHints(100)));
    ASSERT_TRUE(waitForRequests(3));
    FakeReply* pReply = replyOf(2);
    pReply->respondHeaders(200, audioHeaders(100));
    ASSERT_TRUE(tooBig.waitForFailure());
    EXPECT_EQ(Error::Kind::Storage, tooBig.error->kind);
    EXPECT_TRUE(pReply->aborted());
    EXPECT_TRUE(pCache->cachedPath(kServer, kUid2).has_value());
    EXPECT_FALSE(QFileInfo::exists(finalPath(kUid3) + QStringLiteral(".part")));
}

TEST_F(AudioCacheTest, ReconcilesIndexOnStartup) {
    {
        const auto pCache = makeCache();
        download(pCache.get(), kUid1, QByteArrayLiteral("1111111111"));
        Outcome partial(pCache->ensure(kServer, kUid2, Priority::Deck, flacHints(10)));
        ASSERT_TRUE(waitForRequests(2));
        replyOf(1)->respondHeaders(200, audioHeaders(10));
        replyOf(1)->sendBody(QByteArrayLiteral("2222"));
        ASSERT_TRUE(waitForPartSize(kUid2, 4));
        // An offline set pins a track before it is downloaded
        pCache->pin(kServer, kUid4, QStringLiteral("offline-set:7"));
    }
    // Meanwhile: a cached file is deleted, a part file is left without an
    // entry, and other files are in the directory
    ASSERT_TRUE(QFile::remove(finalPath(kUid1)));
    const QString orphanPart = finalPath(kUid3) + QStringLiteral(".part");
    ASSERT_TRUE(QDir().mkpath(QFileInfo(orphanPart).absolutePath()));
    for (const QString& path : {orphanPart, m_dir.filePath(QStringLiteral("notes.part"))}) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    {
        const auto pCache = makeCache();
        EXPECT_FALSE(pCache->cachedPath(kServer, kUid1).has_value());
        EXPECT_FALSE(QFileInfo::exists(orphanPart));
        EXPECT_TRUE(QFileInfo::exists(finalPath(kUid2) + QStringLiteral(".part")));
        EXPECT_TRUE(QFileInfo::exists(m_dir.filePath(QStringLiteral("notes.part"))));
    }

    AudioCacheIndex index(m_dir.path());
    ASSERT_TRUE(index.open());
    EXPECT_FALSE(index.entry(kServer, kUid1).has_value());
    EXPECT_FALSE(index.entry(kServer, kUid3).has_value());
    const std::optional<CacheEntry> resumable = index.entry(kServer, kUid2);
    ASSERT_TRUE(resumable.has_value());
    EXPECT_EQ(CacheEntry::State::Pending, resumable->state);
    EXPECT_EQ(QString::fromLatin1(kEtag1), resumable->etag);
    const std::optional<CacheEntry> pinned = index.entry(kServer, kUid4);
    ASSERT_TRUE(pinned.has_value());
    EXPECT_EQ(QStringList{QStringLiteral("offline-set:7")}, pinned->pinnedReasons);
}

TEST_F(AudioCacheTest, ConcurrentEnsureForSameUidSharesDownload) {
    const auto pCache = makeCache();
    Outcome prefetch(pCache->ensure(kServer, kUid1, Priority::Prefetch, flacHints(10)));
    Outcome deck(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));

    ASSERT_TRUE(waitForRequests(1));
    QTest::qWait(50);
    EXPECT_EQ(1, m_network.sent.size());
    replyOf(0)->respond(200, QByteArrayLiteral("0123456789"), audioHeaders(10));

    ASSERT_TRUE(prefetch.waitForReady());
    ASSERT_TRUE(deck.waitForReady());
    EXPECT_EQ(*prefetch.path, *deck.path);
    // Once cached, it comes without a request
    Outcome again(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));
    ASSERT_TRUE(again.waitForReady());
    EXPECT_EQ(*deck.path, *again.path);
    EXPECT_EQ(1, m_network.sent.size());
}

TEST_F(AudioCacheTest, DroppingTheLastTicketStopsTheDownload) {
    const auto pCache = makeCache();
    CacheTicket* pTicket = pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10));
    ASSERT_TRUE(waitForRequests(1));
    replyOf(0)->respondHeaders(200, audioHeaders(10));
    replyOf(0)->sendBody(QByteArrayLiteral("0123"));

    pTicket->cancel();

    EXPECT_TRUE(replyOf(0)->aborted());
    // Its part file stays, to resume
    ASSERT_TRUE(waitForPartSize(kUid1, 4));
    EXPECT_EQ(0, pCache->usage().activeDownloads);
}

TEST_F(AudioCacheTest, RefusesIdsThatLeaveTheCacheDirectory) {
    const auto pCache = makeCache();

    Outcome outcome(pCache->ensure(kServer, QStringLiteral("../../etc/passwd"), Priority::Deck));

    ASSERT_TRUE(outcome.waitForFailure());
    EXPECT_TRUE(m_network.sent.isEmpty());
}

TEST(CachePathsTest, PathIsStableAndExtensionComesFromFormatOrContentType) {
    using crate::cache::extensionFor;
    using crate::cache::relativeAudioPath;
    EXPECT_EQ(QStringLiteral("srv-1/0f/0f6b.flac"),
            relativeAudioPath(kServer, QStringLiteral("0f6b"), QStringLiteral("flac")));
    EXPECT_EQ(QStringLiteral("flac"), extensionFor(QStringLiteral("FLAC"), "audio/mpeg"));
    EXPECT_EQ(QStringLiteral("m4a"), extensionFor(QStringLiteral("alac"), QByteArray()));
    EXPECT_EQ(QStringLiteral("mp3"), extensionFor(QString(), "audio/mpeg"));
    EXPECT_EQ(QStringLiteral("m4a"), extensionFor(QString(), "audio/mp4; codecs=alac"));
    EXPECT_EQ(QString(), extensionFor(QString(), "application/octet-stream"));
    EXPECT_FALSE(crate::cache::isSafePathId(QStringLiteral("a/b")));
    EXPECT_FALSE(crate::cache::isSafePathId(QStringLiteral("..")));
    EXPECT_TRUE(crate::cache::isSafePathId(kUid1));
}

} // namespace

TEST_F(AudioCacheTest, ReportsTrackStateFindsFilesAndRemoves) {
    const auto pCache = makeCache();
    QStringList changed;
    QObject::connect(pCache.get(),
            &AudioCache::trackChanged,
            pCache.get(),
            [&changed](const QString&, const QString& entityUid) { changed.append(entityUid); });
    EXPECT_EQ(crate::cache::TrackState::Kind::None, pCache->trackState(kServer, kUid1).kind);

    Outcome outcome(pCache->ensure(kServer, kUid1, Priority::Deck, flacHints(10)));
    ASSERT_TRUE(waitForRequests(1));
    replyOf(0)->respondHeaders(200, audioHeaders(10));
    replyOf(0)->sendBody(QByteArrayLiteral("0123"));
    ASSERT_TRUE(waitForPartSize(kUid1, 4));
    const crate::cache::TrackState downloading = pCache->trackState(kServer, kUid1);
    EXPECT_EQ(crate::cache::TrackState::Kind::Downloading, downloading.kind);
    EXPECT_EQ(4, downloading.bytesReceived);
    EXPECT_EQ(10, downloading.bytesTotal);
    // A downloading track stays
    EXPECT_FALSE(pCache->remove(kServer, kUid1));
    replyOf(0)->sendBody(QByteArrayLiteral("456789"));
    replyOf(0)->finishBody();
    ASSERT_TRUE(outcome.waitForReady());
    EXPECT_EQ(crate::cache::TrackState::Kind::Ready, pCache->trackState(kServer, kUid1).kind);
    EXPECT_TRUE(changed.contains(kUid1));

    // The file of a track, and nothing else, names it
    const auto track = pCache->trackOfFile(finalPath(kUid1));
    ASSERT_TRUE(track.has_value());
    EXPECT_EQ(kServer, track->serverId);
    EXPECT_EQ(kUid1, track->entityUid);
    EXPECT_FALSE(pCache->trackOfFile(QStringLiteral("/music/") + kUid1 + QStringLiteral(".flac"))
                    .has_value());
    EXPECT_FALSE(pCache->trackOfFile(m_dir.filePath(QStringLiteral("index.sqlite"))).has_value());

    pCache->pin(kServer, kUid1, QStringLiteral("offline-set:7"));
    changed.clear();
    EXPECT_TRUE(pCache->remove(kServer, kUid1));
    EXPECT_EQ(QStringList{kUid1}, changed);
    EXPECT_EQ(crate::cache::TrackState::Kind::None, pCache->trackState(kServer, kUid1).kind);
    EXPECT_FALSE(pCache->cachedPath(kServer, kUid1).has_value());
    EXPECT_TRUE(QTest::qWaitFor([&] { return !QFileInfo::exists(finalPath(kUid1)); }, kWaitMs));
    EXPECT_EQ(0, pCache->usage().pinnedBytes);
}
