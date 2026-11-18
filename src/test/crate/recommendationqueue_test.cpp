#include "crate/recs/recommendationqueue.h"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTest>
#include <memory>

#include "crate/library/crateloadcoordinator.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "test/librarytest.h"

// Auto DJ's queue and the library's playlists as Radio and Music Paths fill
// them (plan F5.C-PR2 and F5.C-PR3), uncached tracks included.

namespace {

using crate::cache::TrackKey;
using crate::recs::AutoDjRecommendationQueue;
using crate::recs::RecommendedTrack;

const QString kServer = QStringLiteral("srv-1");
constexpr int kWaitMs = 5000;

class CrateRecommendationQueueTest : public LibraryTest {
  protected:
    void SetUp() override {
        PlaylistDAO& playlists = internalCollection()->getPlaylistDAO();
        m_autoDj = playlists.getPlaylistIdFromName(AUTODJ_TABLE);
        if (m_autoDj < 0) {
            m_autoDj = playlists.createPlaylist(AUTODJ_TABLE, PlaylistDAO::PLHT_AUTO_DJ);
        }
        ASSERT_GE(m_autoDj, 0);

        AutoDjRecommendationQueue::Options options;
        options.pPlaylistDao = &playlists;
        options.pDbConnectionPool = dbConnectionPooler();
        // As the coordinator does it: a library track for the file the
        // download will write, <cache>/<server>/<uid>.<format>
        options.trackIds = [this](const QList<crate::library::CrateQueuedTrack>& tracks) {
            QStringList paths;
            for (const auto& queued : tracks) {
                m_tags.append(queued.tags.title);
                paths.append(pathOf(queued.track.entityUid, queued.track.hints.format));
            }
            return crate::library::CrateLoadCoordinator::trackIdsOfFilesIn(
                    trackCollectionManager())(paths);
        };
        options.trackOfFile =
                [this](const QString& location) -> std::optional<TrackKey> {
            const QString prefix = m_cacheDir.path() + QLatin1Char('/') +
                    kServer + QLatin1Char('/');
            if (!location.startsWith(prefix)) {
                return std::nullopt;
            }
            return TrackKey{kServer, location.mid(prefix.size()).section(QLatin1Char('.'), 0, 0)};
        };
        options.settleTime = std::chrono::milliseconds(10);
        m_pQueue = std::make_unique<AutoDjRecommendationQueue>(std::move(options));
    }

    void TearDown() override {
        m_pQueue.reset();
    }

    QString pathOf(const QString& uid, const QString& format) const {
        return m_cacheDir.path() + QLatin1Char('/') + kServer + QLatin1Char('/') + uid +
                QLatin1Char('.') + format;
    }

    static RecommendedTrack track(const QString& uid, const QString& title) {
        RecommendedTrack track;
        track.entityUid = uid;
        track.trackId = 1;
        track.title = title;
        track.artist = QStringLiteral("Artist");
        track.durationMs = 300000;
        track.format = QStringLiteral("flac");
        return track;
    }

    QList<TrackKey> readQueue() {
        std::optional<QList<TrackKey>> read;
        m_pQueue->readQueue([&read](const QList<TrackKey>& queued) {
            read = queued;
        });
        EXPECT_TRUE(QTest::qWaitFor([&] { return read.has_value(); }, kWaitMs));
        return read.value_or(QList<TrackKey>());
    }

    static QStringList uidsOf(const QList<TrackKey>& tracks) {
        QStringList uids;
        for (const TrackKey& track : tracks) {
            uids.append(track.entityUid);
        }
        return uids;
    }

    QTemporaryDir m_cacheDir;
    int m_autoDj = -1;
    QStringList m_tags;
    std::unique_ptr<AutoDjRecommendationQueue> m_pQueue;
};

TEST_F(CrateRecommendationQueueTest, EnqueuesAtTheEndAndReadsTheQueue) {
    QSignalSpy changed(m_pQueue.get(), &crate::recs::RecommendationQueue::queueChanged);
    m_pQueue->enqueue(kServer, {track(QStringLiteral("e1"), QStringLiteral("One"))});
    m_pQueue->enqueue(kServer,
            {track(QStringLiteral("e2"), QStringLiteral("Two")),
                    track(QStringLiteral("e3"), QStringLiteral("Three"))});
    // The changes that come together are told once
    ASSERT_TRUE(changed.wait(kWaitMs));
    QTest::qWait(50);
    EXPECT_EQ(1, changed.size());
    EXPECT_EQ((QStringList{QStringLiteral("One"), QStringLiteral("Two"), QStringLiteral("Three")}),
            m_tags);
    EXPECT_EQ(3, internalCollection()->getPlaylistDAO().tracksInPlaylist(m_autoDj));
    EXPECT_EQ((QStringList{QStringLiteral("e1"), QStringLiteral("e2"), QStringLiteral("e3")}),
            uidsOf(readQueue()));
}

TEST_F(CrateRecommendationQueueTest, SavesAPlaylistWithAUniqueName) {
    PlaylistDAO& playlists = internalCollection()->getPlaylistDAO();
    const QString name = QStringLiteral("Depeche Mode -> Adult.");
    ASSERT_GE(playlists.createPlaylist(name), 0);
    const QString saved = m_pQueue->saveAsPlaylist(name,
            kServer,
            {track(QStringLiteral("e1"), QStringLiteral("One")),
                    track(QStringLiteral("e2"), QStringLiteral("Two"))});
    EXPECT_FALSE(saved.isEmpty());
    EXPECT_NE(name, saved);
    const int playlistId = playlists.getPlaylistIdFromName(saved);
    ASSERT_GE(playlistId, 0);
    EXPECT_EQ(2, playlists.tracksInPlaylist(playlistId));
    // Auto DJ's queue stays as it was
    EXPECT_EQ(0, playlists.tracksInPlaylist(m_autoDj));
}

} // namespace
