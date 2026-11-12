// The stems of Smart Mix playlists in Crate Decks (plan ST5.PR2; STEM-D2,
// D7, D8): the playlists are offline sets, their tracks' stems come first,
// nothing separates while a deck plays, and the stems count in the cache.

#include "crate/stems/stemjobs.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "crate/client/crateclient.h"
#include "crate/stems/stemsidecar.h"
#include "test/crate/fakecratenetwork.h"
#include "test/crate/wavwriter.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"

namespace {

using crate::StemJobs;
using crate::StemSidecar;
using crate::cache::AudioCache;
using crate::library::CrateTrackRef;
using crate::test::FakeReply;

const QString kServer = QStringLiteral("srv-1");
const QString kUidA = QStringLiteral("0a6b3d9e-5a1c-4e2b-8d7f-1a2b3c4d5e61");
const QString kUidB = QStringLiteral("0b6b3d9e-5a1c-4e2b-8d7f-1a2b3c4d5e62");
const QString kUidC = QStringLiteral("0c6b3d9e-5a1c-4e2b-8d7f-1a2b3c4d5e63");
const QString kRevision = QString(64, QLatin1Char('a'));
constexpr qint64 kPlaylist = 7;
constexpr int kWaitMs = 10000;

class FakeStorage : public crate::cache::StorageInfo {
  public:
    qint64 bytesAvailable(const QString&) const override {
        return 100 * AudioCache::kGiB;
    }
};

/// Separates by handing the sink quiet tones, when the test says so.
class FakeSeparator : public crate::LocalStemSeparator {
  public:
    bool isAvailable() const override {
        return true;
    }
    void separate(const TrackPointer& pTrack,
            std::shared_ptr<crate::StemSink> pSink,
            std::function<void(bool, const QString&)> finished) override {
        Q_UNUSED(pTrack);
        ++started;
        m_pSink = std::move(pSink);
        m_finished = std::move(finished);
        if (finishAtOnce) {
            QTimer::singleShot(0, &m_context, [this] {
                finish();
            });
        }
    }
    void cancel() override {
        ++cancelled;
        m_pSink.reset();
        m_finished = nullptr;
    }

    bool running() const {
        return m_finished != nullptr;
    }
    void finish() {
        if (!m_finished) {
            return;
        }
        crate::StemSeparationSource source;
        source.frames = 44100;
        QString error;
        bool done = m_pSink->begin(source, &error);
        std::vector<float> stems(8 * 44100);
        for (std::size_t i = 0; i < 44100; ++i) {
            for (std::size_t row = 0; row < 8; ++row) {
                stems[row * 44100 + i] = static_cast<float>(0.1 *
                        std::sin(2.0 * std::numbers::pi * 100.0 *
                                static_cast<double>(i) / 44100.0));
            }
        }
        done = done && m_pSink->write(0, 44100, stems, &error);
        const auto finished = std::move(m_finished);
        m_finished = nullptr;
        m_pSink.reset();
        finished(done, error);
    }

    int started = 0;
    int cancelled = 0;
    bool finishAtOnce = false;

  private:
    QObject m_context;
    std::shared_ptr<crate::StemSink> m_pSink;
    std::function<void(bool, const QString&)> m_finished;
};

class StemJobsTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        crate::ClientOptions clientOptions;
        clientOptions.deviceFingerprint = QStringLiteral("install-1");
        clientOptions.schedule = m_scheduler.function();
        clientOptions.random = [] { return 0.5; };
        m_pClient = std::make_unique<crate::Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, clientOptions);
        m_pClient->setTokenSource(&m_tokens);

        AudioCache::Options cacheOptions;
        cacheOptions.directory = m_dir.path();
        cacheOptions.clientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pClient.get() : nullptr;
        };
        cacheOptions.pStorageInfo = std::make_shared<FakeStorage>();
        cacheOptions.companionsOf = [](const QString& path) {
            return QStringList{StemSidecar::pathFor(path)};
        };
        m_pCache = std::make_unique<AudioCache>(std::move(cacheOptions));
        ASSERT_TRUE(QTest::qWaitFor([this] { return m_pCache->isLoaded(); }, kWaitMs));

        crate::StemSync::Options syncOptions;
        syncOptions.clientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pClient.get() : nullptr;
        };
        syncOptions.mayUpload = [](const QString&) {
            return false;
        };
        syncOptions.currentRevision = [](const QString&, const QString&) {
            return kRevision;
        };
        m_pSync = std::make_unique<crate::StemSync>(syncOptions);

        // A WAV for the tracks' downloads
        const QString wav = m_dir.filePath(QStringLiteral("source.wav"));
        crate::test::writeWav(wav, 44100, 2 * 44100, [](SINT frame, int channel) {
            Q_UNUSED(channel);
            const double t = static_cast<double>(frame) / 44100.0;
            return static_cast<int>(std::lround(
                    9000.0 * std::sin(2.0 * std::numbers::pi * 60.0 * t) +
                    9000.0 * std::sin(2.0 * std::numbers::pi * 110.0 * t) +
                    6000.0 * std::sin(2.0 * std::numbers::pi * 440.0 * t)));
        });
        QFile file(wav);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        m_wav = file.readAll();
        QFile stems(getTestDir().filePath(QStringLiteral("crate/data/stems/server-stems.mp4")));
        ASSERT_TRUE(stems.open(QIODevice::ReadOnly));
        m_serverStems = stems.readAll();
    }

    void TearDown() override {
        m_pJobs.reset();
        m_pSync.reset();
        m_pCache.reset();
    }

    void makeJobs(bool withSeparator) {
        StemJobs::Options options;
        options.pCache = m_pCache.get();
        options.pSync = m_pSync.get();
        options.clientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pClient.get() : nullptr;
        };
        options.readPlaylist = [this](const QString& serverId,
                                       qint64 playlistId,
                                       const std::function<void(std::optional<QList<CrateTrackRef>>)>& done) {
            EXPECT_EQ(kServer, serverId);
            EXPECT_EQ(kPlaylist, playlistId);
            done(QList<CrateTrackRef>{track(kUidA), track(kUidB)});
        };
        options.confirmDownload =
                [this](const QString&, qint64, int tracks, qint64 bytes, const auto& answer) {
                    m_confirmed.append({tracks, bytes});
                    answer(true);
                };
        options.download = [this](const QList<CrateTrackRef>& tracks, const QString& reason) {
            for (const CrateTrackRef& one : tracks) {
                m_pCache->pin(one.serverId, one.entityUid, reason);
                m_pCache->ensure(one.serverId,
                        one.entityUid,
                        crate::cache::Priority::OfflineSet,
                        one.hints);
            }
        };
        options.pSeparator = withSeparator ? &m_separator : nullptr;
        options.separateWhilePlaying = [this] {
            return m_separateWhilePlaying;
        };
        options.anyDeckPlays = [this] {
            return m_playing;
        };
        options.pollInterval = std::chrono::hours(1);
        options.pauseCheckInterval = std::chrono::milliseconds(10);
        m_pJobs = std::make_unique<StemJobs>(std::move(options));
    }

    static CrateTrackRef track(const QString& uid) {
        CrateTrackRef ref;
        ref.serverId = kServer;
        ref.entityUid = uid;
        ref.hints.format = QStringLiteral("wav");
        ref.hints.sourceRevision = kRevision;
        ref.hints.byteLength = 2 * 44100 * 4 + 44;
        return ref;
    }

    /// Answers every open request: the tracks' files, the server's stems
    /// (if m_serverHasStems) and its Smart Mix playlists.
    void serve() {
        const QList<FakeReply*> replies = m_network.openReplies();
        for (FakeReply* pReply : replies) {
            const QString path = pReply->request().url().path();
            if (path.endsWith(QStringLiteral("/stream"))) {
                pReply->respond(200,
                        m_wav,
                        {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/wav")},
                                {QByteArrayLiteral("Content-Length"),
                                        QByteArray::number(m_wav.size())}});
            } else if (path.endsWith(QStringLiteral("/stem-analysis"))) {
                pReply->respond(404, QByteArrayLiteral("{}"));
            } else if (path.endsWith(QStringLiteral("/stems"))) {
                if (m_serverHasStems) {
                    pReply->respond(200,
                            m_serverStems,
                            {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/mp4")},
                                    {QByteArrayLiteral("Content-Length"),
                                            QByteArray::number(m_serverStems.size())}});
                } else {
                    pReply->respond(404, QByteArrayLiteral("{}"));
                }
            } else if (path.endsWith(QStringLiteral("/api/smart-mix/playlists"))) {
                m_playlistQueries.append(QUrlQuery(pReply->request().url()));
                pReply->respond(200,
                        m_playlists,
                        {{QByteArrayLiteral("Content-Type"),
                                QByteArrayLiteral("application/json")}});
            }
        }
    }

    bool serveUntil(const std::function<bool()>& done) {
        return QTest::qWaitFor(
                [&] {
                    serve();
                    return done();
                },
                kWaitMs);
    }

    int requestsTo(const QString& pathEnd) const {
        int count = 0;
        for (const auto& sent : m_network.sent) {
            if (sent.request.url().path().endsWith(pathEnd)) {
                ++count;
            }
        }
        return count;
    }

    /// Downloads a track into the cache, unpinned.
    void cache(const QString& uid) {
        m_pCache->ensure(kServer, uid, crate::cache::Priority::Prefetch, track(uid).hints);
        ASSERT_TRUE(serveUntil([&] { return m_pCache->cachedPath(kServer, uid).has_value(); }));
    }

    qint64 sizeOnDisk(const QString& uid) const {
        const QString path = *m_pCache->cachedPath(kServer, uid);
        return QFileInfo(path).size() + QFileInfo(StemSidecar::pathFor(path)).size();
    }

    QTemporaryDir m_dir;
    crate::test::FakeNetworkAccessManager m_network;
    crate::test::FakeScheduler m_scheduler;
    crate::test::FakeTokenSource m_tokens;
    std::unique_ptr<crate::Client> m_pClient;
    std::unique_ptr<AudioCache> m_pCache;
    std::unique_ptr<crate::StemSync> m_pSync;
    std::unique_ptr<StemJobs> m_pJobs;
    FakeSeparator m_separator;
    QByteArray m_wav;
    QByteArray m_serverStems;
    bool m_serverHasStems = false;
    bool m_playing = false;
    bool m_separateWhilePlaying = false;
    QByteArray m_playlists = QByteArrayLiteral(R"({"revision":0,"playlists":[]})");
    QList<QUrlQuery> m_playlistQueries;
    QList<std::pair<int, qint64>> m_confirmed;
};

} // namespace

// STEM-D7: while a deck plays nothing starts, and a separation in progress
// stops and starts again once no deck plays, unless the preference allows
// separating while playing.
TEST_F(StemJobsTest, PausesWhileADeckPlays) {
    makeJobs(true);
    cache(kUidC);
    m_pJobs->startServer(kServer);
    m_playing = true;
    m_pJobs->requestStems(track(kUidC));
    QTest::qWait(100);
    serve();
    EXPECT_TRUE(m_pJobs->isPaused());
    EXPECT_EQ(StemJobs::TrackState::Queued, m_pJobs->trackState(kServer, kUidC));
    EXPECT_EQ(0, requestsTo(QStringLiteral("/stem-analysis")));

    // The deck stops: the server has no stems, so Decks separates
    m_playing = false;
    ASSERT_TRUE(serveUntil([&] { return m_separator.running(); }));
    EXPECT_FALSE(m_pJobs->isPaused());
    EXPECT_EQ(StemJobs::TrackState::Working, m_pJobs->trackState(kServer, kUidC));

    // A deck starts while it separates: it stops and waits
    m_playing = true;
    ASSERT_TRUE(QTest::qWaitFor([&] { return m_separator.cancelled == 1; }, kWaitMs));
    EXPECT_EQ(StemJobs::TrackState::Queued, m_pJobs->trackState(kServer, kUidC));
    QTest::qWait(50);
    EXPECT_EQ(1, m_separator.started);

    // Allowed while playing, it starts again and finishes
    m_separateWhilePlaying = true;
    ASSERT_TRUE(serveUntil([&] { return m_separator.running(); }));
    EXPECT_EQ(2, m_separator.started);
    m_separator.finish();
    ASSERT_TRUE(serveUntil([&] {
        return m_pJobs->trackState(kServer, kUidC) == StemJobs::TrackState::Ready;
    }));
    const QString cached = *m_pCache->cachedPath(kServer, kUidC);
    QString error;
    EXPECT_TRUE(StemSidecar::isValidFor(Track::newTemporary(cached), &error))
            << error.toStdString();
}

// Design §6 and review focus 3: stems count in the cache quota, and the
// tracks of a Smart Mix playlist, pinned as its offline set, are never
// evicted; an unpinned track and its stems are.
TEST_F(StemJobsTest, StemsCountInTheCacheQuotaAndPinnedSmartMixPlaylistTracksAreNeverEvicted) {
    makeJobs(false);
    m_serverHasStems = true;
    m_playlists = QByteArrayLiteral(
            R"({"revision":3,"playlists":[{"playlistId":7,"enabled":true,"revision":3}]})");
    m_pJobs->startServer(kServer);
    // Its tracks are downloaded after the size check, pinned, and get the
    // server's stems
    ASSERT_TRUE(serveUntil([&] {
        return m_pJobs->trackState(kServer, kUidA) == StemJobs::TrackState::Ready &&
                m_pJobs->trackState(kServer, kUidB) == StemJobs::TrackState::Ready;
    }));
    ASSERT_EQ(1, m_confirmed.size());
    EXPECT_EQ(2, m_confirmed[0].first);
    EXPECT_EQ(2 * track(kUidA).hints.byteLength.value(), m_confirmed[0].second);
    const auto state = m_pJobs->playlistState(kServer, kPlaylist);
    ASSERT_TRUE(state);
    EXPECT_EQ(2, state->tracks);
    EXPECT_EQ(2, state->ready);

    const qint64 pinned = sizeOnDisk(kUidA) + sizeOnDisk(kUidB);
    EXPECT_GT(pinned, 2 * m_wav.size());
    EXPECT_EQ(pinned, m_pCache->usage().readyBytes);
    EXPECT_EQ(pinned, m_pCache->usage().pinnedBytes);

    // An unpinned track with stems of its own
    cache(kUidC);
    m_pJobs->requestStems(track(kUidC));
    ASSERT_TRUE(serveUntil([&] {
        return m_pJobs->trackState(kServer, kUidC) == StemJobs::TrackState::Ready;
    }));
    const QString cachedC = *m_pCache->cachedPath(kServer, kUidC);
    EXPECT_EQ(pinned + sizeOnDisk(kUidC), m_pCache->usage().readyBytes);

    // A limit below what is cached: the unpinned track and its stems go
    m_pCache->setLimitBytes(pinned + m_wav.size());
    EXPECT_FALSE(m_pCache->cachedPath(kServer, kUidC).has_value());
    ASSERT_TRUE(QTest::qWaitFor(
            [&] { return !QFileInfo::exists(StemSidecar::pathFor(cachedC)); }, kWaitMs));
    for (const QString& uid : {kUidA, kUidB}) {
        ASSERT_TRUE(m_pCache->cachedPath(kServer, uid).has_value());
        EXPECT_TRUE(QFileInfo::exists(StemSidecar::pathFor(*m_pCache->cachedPath(kServer, uid))));
    }
    // Even with no room at all
    m_pCache->setLimitBytes(1);
    EXPECT_TRUE(m_pCache->cachedPath(kServer, kUidA).has_value());
    EXPECT_TRUE(m_pCache->cachedPath(kServer, kUidB).has_value());
}

// STEM-D2: a Smart Mix playlist turned off on the server releases its
// tracks to the cache's LRU; their files and stems stay until then.
TEST_F(StemJobsTest, TurningAPlaylistOffOnTheServerUnpinsItsTracks) {
    makeJobs(false);
    m_serverHasStems = true;
    m_playlists = QByteArrayLiteral(
            R"({"revision":3,"playlists":[{"playlistId":7,"enabled":true,"revision":3}]})");
    m_pJobs->startServer(kServer);
    ASSERT_TRUE(serveUntil([&] {
        return m_pJobs->trackState(kServer, kUidB) == StemJobs::TrackState::Ready;
    }));
    const QList<crate::cache::PinnedSet> sets =
            m_pCache->pinnedSets(QStringLiteral("offline-set:"));
    ASSERT_EQ(1, sets.size());
    EXPECT_EQ(StemJobs::pinReasonOf(kPlaylist), sets[0].reason);
    EXPECT_EQ(2, sets[0].tracks);
    // The first read asks for everything
    ASSERT_EQ(1, m_playlistQueries.size());
    EXPECT_FALSE(m_playlistQueries[0].hasQueryItem(QStringLiteral("sinceRevision")));

    m_playlists = QByteArrayLiteral(
            R"({"revision":4,"playlists":[{"playlistId":7,"enabled":false,"revision":4}]})");
    m_pJobs->syncPlaylists(kServer);
    ASSERT_TRUE(serveUntil([&] { return m_playlistQueries.size() == 2; }));
    EXPECT_EQ(QStringLiteral("3"),
            m_playlistQueries[1].queryItemValue(QStringLiteral("sinceRevision")));
    ASSERT_TRUE(QTest::qWaitFor(
            [&] { return m_pCache->pinnedSets(QStringLiteral("offline-set:")).isEmpty(); },
            kWaitMs));
    EXPECT_FALSE(m_pJobs->playlistState(kServer, kPlaylist).has_value());
    EXPECT_TRUE(m_pCache->cachedPath(kServer, kUidA).has_value());
    EXPECT_EQ(0, m_pCache->usage().pinnedBytes);
}
