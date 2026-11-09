#include "crate/library/cratelibrarysync.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTest>
#include <QThread>
#include <QUrlQuery>
#include <atomic>
#include <memory>

#include "crate/db/crateserverschema.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarystore.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::library::LibrarySync;
using crate::library::PageState;
using crate::library::RemoteContainer;
using crate::library::RemoteTrack;
using crate::library::StoredContainer;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeReply;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");

// The tree of GET /api/decks/containers as the S3 contract serves it: a flat
// list in the server's order, string ids, track_count only for playlists and
// genres, and one likes node with id "me". The server has no folders, but
// parent_id is read anyway. A kind the client does not know, like "folder",
// is left out. Beside the containers, the whole library's track count.
const QByteArray kTree = QByteArrayLiteral(R"({"library": {"track_count": 136512},
    "containers": [
    {"id": "7", "kind": "playlist", "name": "Warm-up", "parent_id": null,
     "track_count": 3, "etag": "p7-1"},
    {"id": "9", "kind": "playlist", "name": "Peak", "parent_id": "8",
     "track_count": 0, "etag": "p9-1"},
    {"id": "8", "kind": "playlist", "name": "Sets", "parent_id": null,
     "track_count": 0, "etag": "p8-1"},
    {"id": "4b1e7c2a-0000-4000-8000-000000000001", "kind": "crate",
     "name": "Disco edits", "parent_id": null, "etag": "c1"},
    {"id": "17", "kind": "genre", "name": "Melodic Techno", "parent_id": null,
     "track_count": 4312, "etag": "g1"},
    {"id": "f1", "kind": "folder", "name": "Not for Decks", "parent_id": null,
     "etag": "f1"},
    {"id": "me", "kind": "likes", "name": "Likes", "parent_id": null, "etag": "l1"}
]})");

QByteArray track(const QString& uid, const QString& title) {
    return QStringLiteral(
            R"({"entity_uid": "%1", "global_track_uid": "g-%1", "title": "%2",
                "artist": "Kessler & Moor", "album": "Night", "duration_ms": 372000,
                "bpm": 124.0, "camelot": "8A", "energy": 0.7, "genre": "Melodic Techno",
                "format": "flac", "bitrate": 1411, "sample_rate": 44100, "bit_depth": 16,
                "byte_length": 41943040, "source_revision": "3b0c4429", "profile_revision": "c7d2",
                "profile_quality": "full", "playable": true, "cover_url": null})")
            .arg(uid, title)
            .toUtf8();
}

QByteArray page(const QList<QByteArray>& tracks, const QString& nextCursor = QString()) {
    QByteArray body = QByteArrayLiteral(
            R"({"container": {"kind": "playlist", "id": "7"}, "tracks": [)");
    for (int i = 0; i < tracks.size(); ++i) {
        if (i > 0) {
            body += ',';
        }
        body += tracks[i];
    }
    body += QByteArrayLiteral(R"(], "next_cursor": )");
    body += nextCursor.isEmpty() ? QByteArrayLiteral("null")
                                 : QByteArray('"' + nextCursor.toUtf8() + '"');
    body += '}';
    return body;
}

class CrateLibrarySyncTest : public MixxxDbTest {
  protected:
    CrateLibrarySyncTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection()));
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(options));
        m_pClient->setTokenSource(&m_tokens);
    }

    void TearDown() override {
        m_pSync.reset();
        m_pClient.reset();
    }

    LibrarySync* makeSync(LibrarySync::Options options = LibrarySync::Options()) {
        m_pSync = std::make_unique<LibrarySync>(
                kServer, m_pClient.get(), dbConnectionPooler(), std::move(options));
        return m_pSync.get();
    }

    /// Stores the tree as if a sync had written it.
    void storeTree() {
        auto tree = crate::library::parseContainerTree(kTree);
        ASSERT_TRUE(tree.ok());
        ASSERT_EQ(crate::library::WriteStatus::Ok,
                crate::library::storeContainerTree(dbConnection(), kServer, tree.value()));
    }

    qint64 containerId(const QString& kind, const QString& remoteId) const {
        QSqlQuery query(dbConnection());
        query.prepare(QStringLiteral(
                "SELECT id FROM crateserver_containers "
                "WHERE server_id = :server AND kind = :kind AND remote_id = :remote"));
        query.bindValue(QStringLiteral(":server"), kServer);
        query.bindValue(QStringLiteral(":kind"), kind);
        query.bindValue(QStringLiteral(":remote"), remoteId);
        if (!query.exec() || !query.next()) {
            return 0;
        }
        return query.value(0).toLongLong();
    }

    /// The titles of a container, in order.
    QStringList titles(qint64 container) const {
        QSqlQuery query(dbConnection());
        query.prepare(QStringLiteral(
                "SELECT t.title FROM crateserver_container_tracks ct "
                "JOIN crateserver_tracks t ON t.server_id = :server "
                "AND t.entity_uid = ct.entity_uid "
                "WHERE ct.container_id = :container ORDER BY ct.position"));
        query.bindValue(QStringLiteral(":server"), kServer);
        query.bindValue(QStringLiteral(":container"), container);
        QStringList result;
        if (query.exec()) {
            while (query.next()) {
                result.append(query.value(0).toString());
            }
        }
        return result;
    }

    StoredContainer stored(qint64 container) const {
        return crate::library::loadContainer(dbConnection(), container).value_or(StoredContainer());
    }

    bool waitForRequests(int count) {
        return QTest::qWaitFor([&] { return m_network.sent.size() >= count; }, 5000);
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<LibrarySync> m_pSync;
};

TEST_F(CrateLibrarySyncTest, SyncsTreeIntoTables) {
    LibrarySync* pSync = makeSync();
    QSignalSpy updated(pSync, &LibrarySync::containersUpdated);

    pSync->syncContainers();
    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_EQ(QUrl(QStringLiteral("https://crate.test/api/decks/containers")),
            m_network.last().request.url());
    m_network.last().pReply->respond(200, kTree);
    ASSERT_TRUE(updated.wait(5000));

    const QList<StoredContainer> containers =
            crate::library::loadContainers(dbConnection(), kServer);
    ASSERT_EQ(6, containers.size());
    EXPECT_EQ(0, containerId(QStringLiteral("folder"), QStringLiteral("f1")));
    const StoredContainer warmUp = stored(
            containerId(QStringLiteral("playlist"), QStringLiteral("7")));
    EXPECT_EQ(QStringLiteral("Warm-up"), warmUp.name);
    EXPECT_EQ(3, warmUp.trackCount.value_or(-1));
    EXPECT_EQ(QStringLiteral("p7-1"), warmUp.etag);
    EXPECT_EQ(0, warmUp.parentId);
    // A child listed before its parent still finds it
    const StoredContainer peak = stored(
            containerId(QStringLiteral("playlist"), QStringLiteral("9")));
    EXPECT_EQ(containerId(QStringLiteral("playlist"), QStringLiteral("8")), peak.parentId);
    EXPECT_LT(warmUp.position, peak.position);
    EXPECT_EQ(4312,
            stored(containerId(QStringLiteral("genre"), QStringLiteral("17")))
                    .trackCount.value_or(-1));
    EXPECT_FALSE(stored(containerId(QStringLiteral("crate"),
                                QStringLiteral("4b1e7c2a-0000-4000-8000-000000000001")))
                    .trackCount.has_value());
    EXPECT_FALSE(stored(containerId(QStringLiteral("likes"), QStringLiteral("me")))
                    .trackCount.has_value());
    EXPECT_EQ(containers.last().id, containerId(QStringLiteral("likes"), QStringLiteral("me")));
    EXPECT_EQ(136512, crate::library::loadLibraryTrackCount(dbConnection(), kServer).value_or(-1));

    // A container the server no longer lists goes, with its tracks
    const qint64 sets = containerId(QStringLiteral("playlist"), QStringLiteral("8"));
    QSqlQuery insert(dbConnection());
    ASSERT_TRUE(insert.exec(QStringLiteral(
            "INSERT INTO crateserver_container_tracks VALUES (%1, 0, 'e1')")
                    .arg(sets)));
    pSync->syncContainers();
    ASSERT_TRUE(waitForRequests(2));
    m_network.last().pReply->respond(200,
            QByteArrayLiteral(R"({"containers": [{"id": "7", "kind": "playlist",
                "name": "Warm-up 2", "parent_id": null, "etag": "p7-2"}]})"));
    ASSERT_TRUE(updated.wait(5000));
    ASSERT_EQ(1, crate::library::loadContainers(dbConnection(), kServer).size());
    EXPECT_EQ(warmUp.id, containerId(QStringLiteral("playlist"), QStringLiteral("7")));
    EXPECT_EQ(QStringLiteral("Warm-up 2"), stored(warmUp.id).name);
    QSqlQuery orphans(dbConnection());
    ASSERT_TRUE(orphans.exec(QStringLiteral("SELECT COUNT(*) FROM crateserver_container_tracks")));
    ASSERT_TRUE(orphans.next());
    EXPECT_EQ(0, orphans.value(0).toInt());
}

TEST_F(CrateLibrarySyncTest, AnOlderServerLeavesTheLibraryCountUnknown) {
    storeTree();
    ASSERT_TRUE(crate::library::loadLibraryTrackCount(dbConnection(), kServer).has_value());
    LibrarySync* pSync = makeSync();
    QSignalSpy updated(pSync, &LibrarySync::containersUpdated);

    // A server from before the library count: its tree has no "library"
    pSync->syncContainers();
    ASSERT_EQ(1, m_network.sent.size());
    m_network.last().pReply->respond(200,
            QByteArrayLiteral(R"({"containers": [{"id": "7", "kind": "playlist",
                "name": "Warm-up", "parent_id": null, "etag": "p7-1"}]})"));
    ASSERT_TRUE(updated.wait(5000));

    // Not the count of the server it was before: the sidebar counts the
    // synced tracks instead
    EXPECT_FALSE(crate::library::loadLibraryTrackCount(dbConnection(), kServer).has_value());
}

TEST_F(CrateLibrarySyncTest, PagesUntilCursorExhausted) {
    storeTree();
    const qint64 warmUp = containerId(QStringLiteral("playlist"), QStringLiteral("7"));
    LibrarySync* pSync = makeSync();
    QSignalSpy updated(pSync, &LibrarySync::containerUpdated);

    pSync->syncContainerTracks(warmUp);
    ASSERT_TRUE(waitForRequests(1));
    ASSERT_EQ(1, m_network.sent.size());
    const QUrl first = m_network.sent[0].request.url();
    EXPECT_EQ(QStringLiteral("/api/decks/containers/playlist/7/tracks"), first.path());
    EXPECT_EQ(QStringLiteral("500"), QUrlQuery(first).queryItemValue(QStringLiteral("limit")));
    EXPECT_FALSE(QUrlQuery(first).hasQueryItem(QStringLiteral("cursor")));
    m_network.sent[0].pReply->respond(200,
            page({track(QStringLiteral("e1"), QStringLiteral("Lattice")),
                         track(QStringLiteral("e2"), QStringLiteral("Northbound Static"))},
                    QStringLiteral("Y3Vyc29yLTI=")),
            {{QByteArrayLiteral("ETag"), QByteArrayLiteral("\"t1\"")}});

    ASSERT_TRUE(waitForRequests(2));
    const QUrl second = m_network.sent[1].request.url();
    EXPECT_EQ(QStringLiteral("Y3Vyc29yLTI="),
            QUrlQuery(second).queryItemValue(QStringLiteral("cursor"), QUrl::FullyDecoded));
    EXPECT_FALSE(m_network.sent[1].request.hasRawHeader("If-None-Match"));
    EXPECT_TRUE(updated.isEmpty());
    m_network.sent[1].pReply->respond(200,
            page({track(QStringLiteral("e3"), QStringLiteral("Glass Orchard"))}));

    ASSERT_TRUE(updated.wait(5000));
    EXPECT_EQ(warmUp, updated.first().first().toLongLong());
    EXPECT_EQ(QStringList({QStringLiteral("Lattice"),
                      QStringLiteral("Northbound Static"),
                      QStringLiteral("Glass Orchard")}),
            titles(warmUp));
    const StoredContainer synced = stored(warmUp);
    EXPECT_EQ(QList<PageState>({{QString(), QStringLiteral("\"t1\""), 2},
                      {QStringLiteral("Y3Vyc29yLTI="), QString(), 1}}),
            synced.pages);
    EXPECT_EQ(QStringLiteral("p7-1"), synced.syncedEtag);
    EXPECT_FALSE(synced.needsSync());
    EXPECT_EQ(3, synced.syncedTrackCount().value_or(0));
    // The server's count stays: it also counts entries without a local track
    EXPECT_EQ(3, synced.trackCount.value_or(0));

    // The DJ fields reach the catalogue
    QSqlQuery query(dbConnection());
    ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT bpm, camelot, energy, profile_quality, playable, byte_length, "
            "duration_ms, global_track_uid FROM crateserver_tracks WHERE entity_uid = 'e1'")));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(124.0, query.value(0).toDouble());
    EXPECT_EQ(QStringLiteral("8A"), query.value(1).toString());
    EXPECT_EQ(0.7, query.value(2).toDouble());
    EXPECT_EQ(QStringLiteral("full"), query.value(3).toString());
    EXPECT_TRUE(query.value(4).toBool());
    EXPECT_EQ(41943040, query.value(5).toLongLong());
    EXPECT_EQ(372000, query.value(6).toLongLong());
    EXPECT_EQ(QStringLiteral("g-e1"), query.value(7).toString());
}

TEST_F(CrateLibrarySyncTest, NotModifiedKeepsRows) {
    storeTree();
    const qint64 warmUp = containerId(QStringLiteral("playlist"), QStringLiteral("7"));
    LibrarySync* pSync = makeSync();
    QSignalSpy updated(pSync, &LibrarySync::containerUpdated);
    pSync->syncContainerTracks(warmUp);
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(200,
            page({track(QStringLiteral("e1"), QStringLiteral("Lattice"))}),
            {{QByteArrayLiteral("ETag"), QByteArrayLiteral("\"t1\"")}});
    ASSERT_TRUE(updated.wait(5000));
    QSignalSpy dirty(pSync, &LibrarySync::containersDirty);
    pSync->markDirty({crate::library::Invalidation::Scope::Container,
            QStringLiteral("playlist"),
            QStringLiteral("7")});
    ASSERT_TRUE(dirty.wait(5000));
    ASSERT_TRUE(stored(warmUp).needsSync());

    pSync->syncContainerTracks(warmUp);
    ASSERT_TRUE(waitForRequests(2));
    EXPECT_EQ(QByteArrayLiteral("\"t1\""), m_network.last().request.rawHeader("If-None-Match"));
    m_network.last().pReply->respond(304, QByteArray());

    ASSERT_TRUE(QTest::qWaitFor([&] { return !pSync->isSyncing(warmUp); }, 5000));
    EXPECT_EQ(QStringList({QStringLiteral("Lattice")}), titles(warmUp));
    EXPECT_FALSE(stored(warmUp).needsSync());
    EXPECT_EQ(1, updated.count());
}

// The tree's ETag only changes with membership, and a page's ETag covers that
// page: a 304 on the first page says nothing of the next ones.
TEST_F(CrateLibrarySyncTest, RevalidatesEveryPage) {
    storeTree();
    const qint64 warmUp = containerId(QStringLiteral("playlist"), QStringLiteral("7"));
    LibrarySync* pSync = makeSync();
    QSignalSpy updated(pSync, &LibrarySync::containerUpdated);
    pSync->syncContainerTracks(warmUp);
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(200,
            page({track(QStringLiteral("e1"), QStringLiteral("Lattice")),
                         track(QStringLiteral("e2"), QStringLiteral("Northbound Static"))},
                    QStringLiteral("c2")),
            {{QByteArrayLiteral("ETag"), QByteArrayLiteral("\"t1\"")}});
    ASSERT_TRUE(waitForRequests(2));
    m_network.last().pReply->respond(200,
            page({track(QStringLiteral("e3"), QStringLiteral("Glass Orchard"))}),
            {{QByteArrayLiteral("ETag"), QByteArrayLiteral("\"t2\"")}});
    ASSERT_TRUE(updated.wait(5000));

    // A track appended to the second page
    pSync->markDirty({crate::library::Invalidation::Scope::Container,
            QStringLiteral("playlist"),
            QStringLiteral("7")});
    pSync->syncContainerTracks(warmUp);
    ASSERT_TRUE(waitForRequests(3));
    EXPECT_EQ(QByteArrayLiteral("\"t1\""), m_network.last().request.rawHeader("If-None-Match"));
    m_network.last().pReply->respond(304, QByteArray());
    ASSERT_TRUE(waitForRequests(4));
    const QUrl second = m_network.last().request.url();
    EXPECT_EQ(QStringLiteral("c2"),
            QUrlQuery(second).queryItemValue(QStringLiteral("cursor"), QUrl::FullyDecoded));
    EXPECT_EQ(QByteArrayLiteral("\"t2\""), m_network.last().request.rawHeader("If-None-Match"));
    m_network.last().pReply->respond(200,
            page({track(QStringLiteral("e3"), QStringLiteral("Glass Orchard")),
                    track(QStringLiteral("e4"), QStringLiteral("Tidal"))}),
            {{QByteArrayLiteral("ETag"), QByteArrayLiteral("\"t2b\"")}});
    ASSERT_TRUE(updated.wait(5000));
    EXPECT_EQ(QStringList({QStringLiteral("Lattice"),
                      QStringLiteral("Northbound Static"),
                      QStringLiteral("Glass Orchard"),
                      QStringLiteral("Tidal")}),
            titles(warmUp));
    EXPECT_EQ(QList<PageState>({{QString(), QStringLiteral("\"t1\""), 2},
                      {QStringLiteral("c2"), QStringLiteral("\"t2b\""), 2}}),
            stored(warmUp).pages);

    // Nothing changed on either page: no update, the rows stay
    pSync->syncContainerTracks(warmUp);
    ASSERT_TRUE(waitForRequests(5));
    m_network.last().pReply->respond(304, QByteArray());
    ASSERT_TRUE(waitForRequests(6));
    EXPECT_EQ(QByteArrayLiteral("\"t2b\""), m_network.last().request.rawHeader("If-None-Match"));
    m_network.last().pReply->respond(304, QByteArray());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pSync->isSyncing(warmUp); }, 5000));
    EXPECT_EQ(2, updated.count());
    EXPECT_EQ(4, titles(warmUp).size());
    EXPECT_FALSE(stored(warmUp).needsSync());
}

TEST_F(CrateLibrarySyncTest, ReplacesContainerTracksAtomically) {
    storeTree();
    const qint64 warmUp = containerId(QStringLiteral("playlist"), QStringLiteral("7"));
    LibrarySync* pSync = makeSync();
    QSignalSpy updated(pSync, &LibrarySync::containerUpdated);
    QSignalSpy failed(pSync, &LibrarySync::syncFailed);
    pSync->syncContainerTracks(warmUp);
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(200,
            page({track(QStringLiteral("a"), QStringLiteral("A")),
                    track(QStringLiteral("b"), QStringLiteral("B"))}));
    ASSERT_TRUE(updated.wait(5000));
    pSync->markDirty({crate::library::Invalidation::Scope::Container,
            QStringLiteral("playlist"),
            QStringLiteral("7")});

    // A new sync whose second page fails leaves the old tracks whole
    pSync->syncContainerTracks(warmUp);
    ASSERT_TRUE(waitForRequests(2));
    m_network.last().pReply->respond(200,
            page({track(QStringLiteral("x"), QStringLiteral("X"))}, QStringLiteral("next")));
    ASSERT_TRUE(waitForRequests(3));
    EXPECT_EQ(QStringList({QStringLiteral("A"), QStringLiteral("B")}), titles(warmUp));
    m_network.last().pReply->respond(503, QByteArray());
    // The Client retries a GET on 503: let the retries fail too
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                const QList<FakeReply*> open = m_network.openReplies();
                for (FakeReply* pReply : open) {
                    pReply->failNetwork();
                }
                return !failed.isEmpty();
            },
            10000));
    EXPECT_EQ(QStringList({QStringLiteral("A"), QStringLiteral("B")}), titles(warmUp));
    EXPECT_TRUE(stored(warmUp).needsSync());

    pSync->syncContainerTracks(warmUp);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !m_network.openReplies().isEmpty(); }, 5000));
    m_network.openReplies().first()->respond(200,
            page({track(QStringLiteral("x"), QStringLiteral("X")),
                    track(QStringLiteral("y"), QStringLiteral("Y")),
                    track(QStringLiteral("z"), QStringLiteral("Z"))}));
    ASSERT_TRUE(updated.wait(5000));
    EXPECT_EQ(QStringList({QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")}),
            titles(warmUp));
}

TEST_F(CrateLibrarySyncTest, ParseRunsOffMainThread) {
    storeTree();
    const qint64 likes = containerId(QStringLiteral("likes"), QStringLiteral("me"));
    std::atomic<QThread*> parseThread = nullptr;
    std::atomic<int> parses = 0;
    LibrarySync::Options options;
    options.onParse = [&] {
        parseThread = QThread::currentThread();
        ++parses;
    };
    LibrarySync* pSync = makeSync(std::move(options));
    QSignalSpy updated(pSync, &LibrarySync::containerUpdated);
    QSignalSpy treeUpdated(pSync, &LibrarySync::containersUpdated);

    pSync->syncContainerTracks(likes);
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(200,
            page({track(QStringLiteral("e1"), QStringLiteral("Lattice"))}));
    ASSERT_TRUE(updated.wait(5000));
    pSync->syncContainers();
    m_network.last().pReply->respond(200, kTree);
    ASSERT_TRUE(treeUpdated.wait(5000));

    EXPECT_EQ(2, parses.load());
    ASSERT_NE(nullptr, parseThread.load());
    EXPECT_NE(QCoreApplication::instance()->thread(), parseThread.load());
}

TEST(CrateLibraryJsonTest, ReadsTheContractTree) {
    const auto tree = crate::library::parseContainerTree(kTree);
    ASSERT_TRUE(tree.ok());
    ASSERT_EQ(6, tree.value().containers.size());
    EXPECT_EQ(136512, tree.value().libraryTrackCount.value_or(-1));
    const RemoteContainer& warmUp = tree.value().containers.first();
    EXPECT_EQ(QStringLiteral("playlist"), warmUp.kind);
    EXPECT_EQ(QStringLiteral("7"), warmUp.remoteId);
    EXPECT_EQ(QStringLiteral("Warm-up"), warmUp.name);
    EXPECT_TRUE(warmUp.parentRemoteId.isEmpty());
    EXPECT_EQ(3, warmUp.trackCount.value_or(0));
    EXPECT_EQ(QStringLiteral("p7-1"), warmUp.etag);
    EXPECT_EQ(QStringLiteral("8"), tree.value().containers[1].parentRemoteId);
    EXPECT_EQ(QStringLiteral("likes"), tree.value().containers.last().kind);
    EXPECT_EQ(QStringLiteral("me"), tree.value().containers.last().remoteId);

    // An integer id is read as text
    const auto integer = crate::library::parseContainerTree(QByteArrayLiteral(
            R"({"containers": [{"id": 28, "kind": "playlist", "name": "A", "etag": "e"}]})"));
    ASSERT_TRUE(integer.ok());
    EXPECT_EQ(QStringLiteral("28"), integer.value().containers.first().remoteId);

    EXPECT_FALSE(crate::library::parseContainerTree(
            QByteArrayLiteral(R"({"containers": [{"kind": "playlist", "name": "No id"}]})"))
                    .ok());
    EXPECT_FALSE(crate::library::parseContainerTree(
            QByteArrayLiteral(R"({"containers": [{"id": "1", "name": "No kind"}]})"))
                    .ok());
    EXPECT_FALSE(crate::library::parseContainerTree(
            QByteArrayLiteral(R"({"playlists": []})"))
                    .ok());
    EXPECT_FALSE(crate::library::parseContainerTree(QByteArrayLiteral("[]")).ok());
}

TEST(CrateLibraryJsonTest, ReadsTheLibraryTrackCount) {
    const auto count = [](const char* json) {
        return crate::library::parseContainerTree(QByteArray(json));
    };
    EXPECT_EQ(0,
            count(R"({"library": {"track_count": 0}, "containers": []})")
                    .value()
                    .libraryTrackCount.value_or(-1));
    // A server older than the count, or one that has none to give
    EXPECT_FALSE(count(R"({"containers": []})").value().libraryTrackCount.has_value());
    EXPECT_FALSE(count(R"({"library": null, "containers": []})")
                    .value()
                    .libraryTrackCount.has_value());
    EXPECT_FALSE(
            count(R"({"library": {}, "containers": []})").value().libraryTrackCount.has_value());
    // Not a count
    EXPECT_FALSE(count(R"({"library": 136512, "containers": []})").ok());
    EXPECT_FALSE(count(R"({"library": {"track_count": "many"}, "containers": []})").ok());
    EXPECT_FALSE(count(R"({"library": {"track_count": -1}, "containers": []})").ok());
}

TEST(CrateLibraryJsonTest, KnowsTheSectionOfEachKind) {
    using crate::library::Section;
    EXPECT_EQ(Section::Playlists, crate::library::sectionOf(QStringLiteral("playlist")));
    EXPECT_EQ(Section::Crates, crate::library::sectionOf(QStringLiteral("crate")));
    EXPECT_EQ(Section::Likes, crate::library::sectionOf(QStringLiteral("likes")));
    EXPECT_EQ(Section::Genres, crate::library::sectionOf(QStringLiteral("genre")));
    EXPECT_EQ(Section::Moods, crate::library::sectionOf(QStringLiteral("mood")));
    EXPECT_EQ(Section::Recent, crate::library::sectionOf(QStringLiteral("recent")));
    EXPECT_FALSE(crate::library::sectionOf(QStringLiteral("folder")).has_value());
    // Not the server's spelling: the tracks path would not take it
    EXPECT_FALSE(crate::library::sectionOf(QStringLiteral("genres")).has_value());
}

TEST(CrateLibraryJsonTest, TrackPageNeedsEntityUids) {
    const auto parsed = crate::library::parseTrackPage(
            page({track(QStringLiteral("e1"), QStringLiteral("A"))}, QStringLiteral("WyJw")));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(1, parsed.value().tracks.size());
    const RemoteTrack& first = parsed.value().tracks.first();
    EXPECT_EQ(QStringLiteral("c7d2"), first.profileRevision);
    EXPECT_EQ(QStringLiteral("3b0c4429"), first.sourceRevision);
    EXPECT_EQ(0.7, first.energy.value_or(0));
    EXPECT_TRUE(first.playable.value_or(false));
    EXPECT_TRUE(first.coverUrl.isEmpty());
    EXPECT_EQ(QStringLiteral("WyJw"), parsed.value().nextCursor);
    EXPECT_TRUE(crate::library::parseTrackPage(page({})).value().nextCursor.isEmpty());

    // No profile and no audio: the nulls of the contract
    const auto bare = crate::library::parseTrackPage(QByteArrayLiteral(
            R"({"tracks": [{"entity_uid": "e", "title": "T", "artist": "A", "album": null,
                "bpm": null, "camelot": null, "energy": null, "source_revision": null,
                "profile_revision": null, "profile_quality": null, "playable": false}],
                "next_cursor": null})"));
    ASSERT_TRUE(bare.ok());
    EXPECT_FALSE(bare.value().tracks.first().bpm.has_value());
    EXPECT_TRUE(bare.value().tracks.first().profileQuality.isEmpty());
    EXPECT_FALSE(bare.value().tracks.first().playable.value_or(true));

    EXPECT_FALSE(crate::library::parseTrackPage(
            QByteArrayLiteral(R"({"tracks": [{"title": "x"}]})"))
                    .ok());
    EXPECT_FALSE(crate::library::parseTrackPage(
            QByteArrayLiteral(
                    R"({"tracks": [{"entity_uid": "e", "bpm": "fast"}]})"))
                    .ok());
    EXPECT_EQ(QStringLiteral("/api/decks/containers/likes/me/tracks"),
            crate::library::containerTracksPath(QStringLiteral("likes"), QStringLiteral("me")));
    EXPECT_EQ(QStringLiteral("/api/decks/containers/crate/a%2Fb/tracks"),
            crate::library::containerTracksPath(QStringLiteral("crate"), QStringLiteral("a/b")));
}

} // namespace
