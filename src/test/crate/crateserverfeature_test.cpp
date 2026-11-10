#include "library/crateserver/crateserverfeature.h"

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QIcon>
#include <QItemSelectionModel>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <iostream>
#include <memory>

#include "crate/crateservers.h"
#include "crate/db/crateserverprofiles.h"
#include "crate/db/crateserverschema.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarystore.h"
#include "crate/ui/delegates/delegateroles.h"
#include "library/crateserver/crateservertrackmodel.h"
#include "library/trackcollection.h"
#include "library/trackset/crate/crate.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "test/crate/fakecratenetwork.h"
#include "test/librarytest.h"

namespace {

using crate::CrateServers;
using crate::ServerProfile;
using crate::delegates::ProfileQuality;
using crate::library::CrateServerFeature;
using crate::library::CrateServerTrackModel;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeSecretStore;

const QString kServer = QStringLiteral("srv-1");

const QByteArray kCapabilities = QByteArrayLiteral(
        R"({"decks": {"available": true, "min_client_version": "0.1.0",
            "contract_version": 1, "features": ["library_cursor"]}})");

const QByteArray kTree = QByteArrayLiteral(R"({"library": {"track_count": 136512},
    "containers": [
    {"id": "28", "kind": "playlist", "name": "Peak hour", "parent_id": null,
     "track_count": 0, "etag": "p28"},
    {"id": "7", "kind": "playlist", "name": "Warm-up", "parent_id": null,
     "track_count": 52, "etag": "p7"},
    {"id": "4b1e7c2a-0000-4000-8000-000000000001", "kind": "crate",
     "name": "Disco edits", "parent_id": null, "etag": "c1"},
    {"id": "17", "kind": "genre", "name": "Melodic Techno", "parent_id": null,
     "track_count": 4312, "etag": "g1"},
    {"id": "me", "kind": "likes", "name": "Likes", "parent_id": null, "etag": "l1"}
]})");

crate::library::RemoteTrack remoteTrack(const QString& uid,
        const QString& title,
        double energy,
        const QString& quality) {
    crate::library::RemoteTrack track;
    track.entityUid = uid;
    track.title = title;
    track.artist = QStringLiteral("Ondine Varga");
    track.album = QStringLiteral("Night");
    track.durationMs = 372000;
    track.bpm = 124.0;
    track.camelot = QStringLiteral("8A");
    track.energy = energy;
    track.genre = QStringLiteral("Melodic Techno");
    track.format = QStringLiteral("flac");
    track.profileQuality = quality;
    track.playable = true;
    return track;
}

class CrateServerFeatureTest : public LibraryTest {
  protected:
    void SetUp() override {
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ServerProfile profile;
        profile.id = kServer;
        profile.baseUrl = QUrl(QStringLiteral("https://dev.crate.local"));
        ASSERT_TRUE(crate::db::saveServerProfile(dbConnection(), profile));
        ASSERT_TRUE(crate::db::saveLastCapabilities(dbConnection(), kServer, kCapabilities));
        m_store.secrets.insert(kServer + QStringLiteral(":refresh"), QStringLiteral("refresh-1"));

        CrateServers::Dependencies dependencies;
        dependencies.pConfig = config();
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.pSecretStore = &m_store;
        m_pServers = std::make_unique<CrateServers>(dependencies);
        m_pServers->start();
        // The session renews its access token at start
        ASSERT_FALSE(m_network.sent.isEmpty());
        m_network.last().pReply->respond(200,
                QByteArrayLiteral(R"({"id": 7, "email": "dj@crate.test", "role": "user",
                    "token": "access-1", "refresh_token": "refresh-2"})"));
        m_sentAtStart = m_network.sent.size();
    }

    void TearDown() override {
        m_pFeature.reset();
        m_pServers.reset();
    }

    CrateServerFeature* makeFeature() {
        CrateServerFeature::Dependencies dependencies;
        dependencies.pTrackCollectionManager = trackCollectionManager();
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pServers = m_pServers.get();
        m_pFeature = std::make_unique<CrateServerFeature>(nullptr, config(), dependencies);
        // The sidebar is read off the main thread
        QSignalSpy updated(m_pFeature.get(), &CrateServerFeature::sidebarUpdated);
        EXPECT_TRUE(updated.wait(5000));
        return m_pFeature.get();
    }

    void storeTree() {
        const auto tree = crate::library::parseContainerTree(kTree);
        ASSERT_TRUE(tree.ok());
        ASSERT_EQ(crate::library::WriteStatus::Ok,
                crate::library::storeContainerTree(dbConnection(), kServer, tree.value()));
    }

    qint64 containerId(const QString& kind, const QString& remoteId) const {
        QSqlQuery query(dbConnection());
        query.prepare(
                QStringLiteral("SELECT id FROM crateserver_containers WHERE "
                               "kind = :kind AND remote_id = :remote"));
        query.bindValue(QStringLiteral(":kind"), kind);
        query.bindValue(QStringLiteral(":remote"), remoteId);
        return query.exec() && query.next() ? query.value(0).toLongLong() : 0;
    }

    /// Stores `tracks` as the synced tracks of a container, in that order.
    void storeContainerTracks(qint64 container, const QList<crate::library::RemoteTrack>& tracks) {
        ASSERT_EQ(crate::library::WriteStatus::Ok,
                crate::library::storeTracks(dbConnection(),
                        kServer,
                        tracks,
                        QStringLiteral("2026-11-09T10:00:00Z")));
        QStringList uids;
        for (const auto& track : tracks) {
            uids.append(track.entityUid);
        }
        const auto stored = crate::library::loadContainer(dbConnection(), container);
        ASSERT_TRUE(stored.has_value());
        ASSERT_EQ(crate::library::WriteStatus::Ok,
                crate::library::replaceContainerTracks(dbConnection(),
                        container,
                        uids,
                        {{{QString(), QStringLiteral("\"t\""), static_cast<int>(uids.size())}},
                                stored->etag,
                                QStringLiteral("2026-11-09T10:00:00Z")}));
    }

    TreeItem* root() const {
        return m_pFeature->sidebarModel()->getRootItem();
    }

    QStringList labels(const TreeItem* pItem) const {
        QStringList result;
        for (const TreeItem* pChild : pItem->children()) {
            result.append(pChild->getLabel());
        }
        return result;
    }

    TreeItem* childLabelled(const TreeItem* pItem, const QString& prefix) const {
        for (TreeItem* pChild : pItem->children()) {
            if (pChild->getLabel().startsWith(prefix)) {
                return pChild;
            }
        }
        return nullptr;
    }

    QModelIndex indexOf(TreeItem* pItem) const {
        TreeItemModel* pModel = m_pFeature->sidebarModel();
        QList<int> rows;
        for (TreeItem* pCurrent = pItem; pCurrent && pCurrent->hasParent();
                pCurrent = pCurrent->parent()) {
            rows.prepend(pCurrent->parentRow());
        }
        QModelIndex index;
        for (const int row : rows) {
            index = pModel->index(row, 0, index);
        }
        return index;
    }

    int sentTo(const QString& path) const {
        int count = 0;
        for (const auto& sent : m_network.sent) {
            if (sent.request.url().path() == path) {
                ++count;
            }
        }
        return count;
    }

    FakeNetworkAccessManager::Sent* lastTo(const QString& path) {
        for (auto it = m_network.sent.rbegin(); it != m_network.sent.rend(); ++it) {
            if (it->request.url().path() == path) {
                return &*it;
            }
        }
        return nullptr;
    }

    FakeNetworkAccessManager m_network;
    FakeSecretStore m_store;
    std::unique_ptr<CrateServers> m_pServers;
    std::unique_ptr<CrateServerFeature> m_pFeature;
    qsizetype m_sentAtStart = 0;
};

TEST_F(CrateServerFeatureTest, SidebarShowsServerAndSections) {
    storeTree();
    storeContainerTracks(
            containerId(QStringLiteral("likes"), QStringLiteral("me")),
            {remoteTrack(QStringLiteral("e1"),
                     QStringLiteral("Lattice"),
                     0.7,
                     QStringLiteral("full")),
                    remoteTrack(QStringLiteral("e2"),
                            QStringLiteral("Cassiopeia"),
                            0.8,
                            QStringLiteral("full"))});
    CrateServerFeature* pFeature = makeFeature();

    EXPECT_EQ(QStringLiteral("Crate · dev.crate.local"), pFeature->title().toString());
    EXPECT_EQ(QStringLiteral("crateserver"), pFeature->iconName());
    // The sections Crate serves, with their counts; no moods or recent from
    // this server, and nothing of later phases. Library counts the whole
    // library, as the server's tree gives it, not the tracks synced so far
    EXPECT_EQ(QStringList({QStringLiteral("Library (136512)"),
                      QStringLiteral("Playlists (2)"),
                      QStringLiteral("Crates (1)"),
                      QStringLiteral("Genres"),
                      QStringLiteral("Likes (2)")}),
            labels(root()));
    // Their Solar icons, by name, for the CrateDecks sidebar
    EXPECT_EQ(QStringLiteral("crate-library"), root()->child(0)->getIcon().name());
    EXPECT_EQ(QStringLiteral("crate-likes"), root()->child(4)->getIcon().name());
    // The server's order, and its count when there is one
    EXPECT_EQ(QStringList({QStringLiteral("Peak hour"), QStringLiteral("Warm-up (52)")}),
            labels(root()->child(1)));
    EXPECT_EQ(QStringList({QStringLiteral("Disco edits")}), labels(root()->child(2)));
    // Likes is the one container of its section
    EXPECT_FALSE(root()->child(4)->hasChildren());

    // Painted from the local tables: nothing asked yet
    EXPECT_EQ(m_sentAtStart, m_network.sent.size());
}

TEST_F(CrateServerFeatureTest, LibraryCountsTheSyncedTracksWithoutTheServersCount) {
    // A tree from a server older than the library count
    const auto tree = crate::library::parseContainerTree(kTree);
    ASSERT_TRUE(tree.ok());
    ASSERT_EQ(crate::library::WriteStatus::Ok,
            crate::library::storeContainerTree(
                    dbConnection(), kServer, tree.value().containers));
    storeContainerTracks(containerId(QStringLiteral("likes"), QStringLiteral("me")),
            {remoteTrack(QStringLiteral("e1"), QStringLiteral("Lattice"), 0.7, QString()),
                    remoteTrack(QStringLiteral("e2"),
                            QStringLiteral("Cassiopeia"),
                            0.8,
                            QString())});
    makeFeature();

    EXPECT_EQ(QStringLiteral("Library (2)"), root()->child(0)->getLabel());
}

TEST_F(CrateServerFeatureTest, ActivatingPlaylistShowsTracksInOrder) {
    storeTree();
    const qint64 warmUp = containerId(QStringLiteral("playlist"), QStringLiteral("7"));
    storeContainerTracks(warmUp,
            {remoteTrack(QStringLiteral("e3"),
                     QStringLiteral("Glass Orchard"),
                     0.6,
                     QStringLiteral("partial")),
                    remoteTrack(QStringLiteral("e1"),
                            QStringLiteral("Lattice"),
                            0.7,
                            QStringLiteral("full")),
                    remoteTrack(QStringLiteral("e2"),
                            QStringLiteral("Northbound Static"),
                            0.04,
                            QString())});
    CrateServerFeature* pFeature = makeFeature();
    QSignalSpy shown(pFeature, &LibraryFeature::showTrackModel);

    TreeItem* pWarmUp = childLabelled(childLabelled(root(), QStringLiteral("Playlists")),
            QStringLiteral("Warm-up"));
    ASSERT_NE(nullptr, pWarmUp);
    pFeature->activateChild(indexOf(pWarmUp));

    ASSERT_EQ(1, shown.size());
    auto* pModel = dynamic_cast<CrateServerTrackModel*>(
            shown.first().first().value<QAbstractItemModel*>());
    ASSERT_NE(nullptr, pModel);
    ASSERT_TRUE(QTest::qWaitFor([&] { return pModel->rowCount() == 3; }, 5000));

    const auto cell = [pModel](int row, const QString& field, int role = Qt::DisplayRole) {
        return pModel->index(row, pModel->fieldIndex(field)).data(role);
    };
    EXPECT_EQ(QStringLiteral("Glass Orchard"), cell(0, QStringLiteral("title")).toString());
    EXPECT_EQ(QStringLiteral("Lattice"), cell(1, QStringLiteral("title")).toString());
    EXPECT_EQ(QStringLiteral("Northbound Static"), cell(2, QStringLiteral("title")).toString());
    EXPECT_EQ(1, cell(0, QStringLiteral("position")).toInt());
    EXPECT_EQ(3, cell(2, QStringLiteral("position")).toInt());
    EXPECT_EQ(QStringLiteral("6:12"), cell(0, QStringLiteral("duration")).toString());
    EXPECT_EQ(QStringLiteral("8A"),
            cell(0, QStringLiteral("key"), crate::delegates::CamelotRole).toString());
    // Energy as the mockups' level from 1 to 10
    EXPECT_EQ(QStringLiteral("6"), cell(0, QStringLiteral("crate_energy")).toString());
    EXPECT_EQ(QStringLiteral("1"), cell(2, QStringLiteral("crate_energy")).toString());
    EXPECT_EQ(static_cast<int>(ProfileQuality::Partial),
            cell(0, QStringLiteral("crate_profile"), crate::delegates::ProfileQualityRole).toInt());
    EXPECT_EQ(static_cast<int>(ProfileQuality::Full),
            cell(1, QStringLiteral("crate_profile"), crate::delegates::ProfileQualityRole).toInt());
    EXPECT_EQ(static_cast<int>(ProfileQuality::Missing),
            cell(2, QStringLiteral("crate_profile"), crate::delegates::ProfileQualityRole).toInt());
    EXPECT_EQ(QStringLiteral("crate_cache"),
            pModel->headerData(pModel->fieldIndex(QStringLiteral("crate_cache")),
                          Qt::Horizontal,
                          TrackModel::kHeaderNameRole)
                    .toString());
    // Loading into decks is F4
    EXPECT_EQ(nullptr, pModel->getTrack(pModel->index(0, 0)));
    EXPECT_FALSE(pModel->getTrackId(pModel->index(0, 0)).isValid());
    EXPECT_FALSE(pModel->hasCapabilities(TrackModel::Capability::LoadToDeck));

    // From the local tables: the server is not asked before its capabilities
    // are known
    EXPECT_EQ(0, sentTo(QStringLiteral("/api/decks/containers/playlist/7/tracks")));
}

TEST_F(CrateServerFeatureTest, SortingKeepsTheSelection) {
    storeTree();
    const qint64 warmUp = containerId(QStringLiteral("playlist"), QStringLiteral("7"));
    storeContainerTracks(warmUp,
            {remoteTrack(QStringLiteral("e3"), QStringLiteral("Glass Orchard"), 0.6, QString()),
                    remoteTrack(QStringLiteral("e1"), QStringLiteral("Lattice"), 0.7, QString()),
                    remoteTrack(QStringLiteral("e2"),
                            QStringLiteral("Northbound Static"),
                            0.5,
                            QString())});
    CrateServerFeature* pFeature = makeFeature();
    QSignalSpy shown(pFeature, &LibraryFeature::showTrackModel);
    pFeature->activateChild(indexOf(childLabelled(
            childLabelled(root(), QStringLiteral("Playlists")), QStringLiteral("Warm-up"))));
    ASSERT_EQ(1, shown.size());
    auto* pModel = dynamic_cast<CrateServerTrackModel*>(
            shown.first().first().value<QAbstractItemModel*>());
    ASSERT_NE(nullptr, pModel);
    ASSERT_TRUE(QTest::qWaitFor([&] { return pModel->rowCount() == 3; }, 5000));
    QItemSelectionModel selection(pModel);
    // Lattice
    selection.select(pModel->index(1, CrateServerTrackModel::kTitleColumn),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);

    QSignalSpy loaded(pModel, &CrateServerTrackModel::loaded);
    pModel->sort(CrateServerTrackModel::kTitleColumn, Qt::DescendingOrder);
    ASSERT_TRUE(loaded.wait(5000));

    EXPECT_EQ(QStringLiteral("Northbound Static"),
            pModel->index(0, CrateServerTrackModel::kTitleColumn).data().toString());
    const QModelIndexList selected = selection.selectedRows();
    ASSERT_EQ(1, selected.size());
    EXPECT_EQ(QStringLiteral("Lattice"),
            pModel->index(selected.first().row(), CrateServerTrackModel::kTitleColumn)
                    .data()
                    .toString());
}

TEST_F(CrateServerFeatureTest, ActivatingAnUnsyncedContainerFetchesIt) {
    storeTree();
    CrateServerFeature* pFeature = makeFeature();
    QSignalSpy shown(pFeature, &LibraryFeature::showTrackModel);
    TreeItem* pPeak = childLabelled(childLabelled(root(), QStringLiteral("Playlists")),
            QStringLiteral("Peak hour"));
    ASSERT_NE(nullptr, pPeak);

    pFeature->activateChild(indexOf(pPeak));
    ASSERT_EQ(1, shown.size());
    auto* pModel = dynamic_cast<CrateServerTrackModel*>(
            shown.first().first().value<QAbstractItemModel*>());
    ASSERT_NE(nullptr, pModel);

    // The capabilities are checked once per run before anything is synced
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(QStringLiteral("/api/capabilities")) == 1; },
            5000));
    lastTo(QStringLiteral("/api/capabilities"))->pReply->respond(200, kCapabilities);
    const QString path = QStringLiteral("/api/decks/containers/playlist/28/tracks");
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(path) == 1; }, 5000));
    lastTo(path)->pReply->respond(200,
            QByteArrayLiteral(R"({"container": {"kind": "playlist", "id": "28"},
                "tracks": [{"entity_uid": "e9", "title": "Saltwater Hymn", "artist": "Mirae",
                            "bpm": 122.0, "camelot": "3A", "energy": 0.5, "playable": true}],
                "next_cursor": null})"));

    ASSERT_TRUE(QTest::qWaitFor([&] { return pModel->rowCount() == 1; }, 5000));
    EXPECT_EQ(QStringLiteral("Saltwater Hymn"),
            pModel->index(0, pModel->fieldIndex(QStringLiteral("title"))).data().toString());
    // The sidebar counts what was synced
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                return pPeak->getLabel() == QStringLiteral("Peak hour (1)");
            },
            5000));
    // And the server's invalidations are listened to
    EXPECT_EQ(1, sentTo(QStringLiteral("/api/cache/events")));
}

TEST_F(CrateServerFeatureTest, ActivatingASyncedContainerRevalidatesItOncePerRun) {
    storeTree();
    const qint64 warmUp = containerId(QStringLiteral("playlist"), QStringLiteral("7"));
    storeContainerTracks(warmUp,
            {remoteTrack(QStringLiteral("e3"),
                     QStringLiteral("Glass Orchard"),
                     0.6,
                     QStringLiteral("partial")),
                    remoteTrack(QStringLiteral("e1"),
                            QStringLiteral("Lattice"),
                            0.7,
                            QStringLiteral("full"))});
    const auto fetchedAt = [this, warmUp] {
        const auto stored = crate::library::loadContainer(dbConnection(), warmUp);
        return stored ? stored->fetchedAt : QString();
    };
    const QString syncedAt = fetchedAt();
    CrateServerFeature* pFeature = makeFeature();
    TreeItem* pWarmUp = childLabelled(childLabelled(root(), QStringLiteral("Playlists")),
            QStringLiteral("Warm-up"));
    ASSERT_NE(nullptr, pWarmUp);

    // Synced in an earlier run, with the same tree ETag. That ETag only
    // follows membership and order, and a mood's never changes, so the
    // pages are asked once anyway, each with the ETag it had (S3 contract)
    pFeature->activateChild(indexOf(pWarmUp));
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(QStringLiteral("/api/capabilities")) == 1; },
            5000));
    lastTo(QStringLiteral("/api/capabilities"))->pReply->respond(200, kCapabilities);
    const QString path = QStringLiteral("/api/decks/containers/playlist/7/tracks");
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(path) == 1; }, 5000));
    EXPECT_EQ(QByteArrayLiteral("\"t\""), lastTo(path)->request.rawHeader("If-None-Match"));

    // A revalidation that failed is tried again when the container is shown
    lastTo(path)->pReply->respond(404);
    QTest::qWait(50);
    pFeature->activateChild(indexOf(pWarmUp));
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(path) == 2; }, 5000));
    lastTo(path)->pReply->respond(304,
            QByteArray(),
            {{QByteArrayLiteral("ETag"), QByteArrayLiteral("\"t\"")}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return fetchedAt() != syncedAt; }, 5000));

    // Revalidated: not asked again in this run
    pFeature->activateChild(indexOf(pWarmUp));
    QTest::qWait(50);
    EXPECT_EQ(2, sentTo(path));
}

TEST_F(CrateServerFeatureTest, LazyExpansionFetchesOnce) {
    CrateServerFeature* pFeature = makeFeature();
    // Nothing synced yet: the sections Crate always serves
    EXPECT_EQ(QStringList({QStringLiteral("Library"),
                      QStringLiteral("Playlists"),
                      QStringLiteral("Crates")}),
            labels(root()));

    TreeItem* pPlaylists = childLabelled(root(), QStringLiteral("Playlists"));
    pFeature->onLazyChildExpandation(indexOf(pPlaylists));
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(QStringLiteral("/api/capabilities")) == 1; },
            5000));
    lastTo(QStringLiteral("/api/capabilities"))->pReply->respond(200, kCapabilities);
    const QString tree = QStringLiteral("/api/decks/containers");
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(tree) == 1; }, 5000));
    lastTo(tree)->pReply->respond(200, kTree);
    ASSERT_TRUE(QTest::qWaitFor([&] { return labels(root()).size() == 5; }, 5000));
    EXPECT_EQ(QStringLiteral("Library (136512)"), root()->child(0)->getLabel());
    pPlaylists = childLabelled(root(), QStringLiteral("Playlists"));
    EXPECT_EQ(QStringLiteral("Playlists (2)"), pPlaylists->getLabel());
    EXPECT_EQ(2, pPlaylists->childRows());

    pFeature->onLazyChildExpandation(indexOf(pPlaylists));
    pFeature->onLazyChildExpandation(indexOf(childLabelled(root(), QStringLiteral("Crates"))));
    pFeature->activate();
    QTest::qWait(50);
    EXPECT_EQ(1, sentTo(tree));
    EXPECT_EQ(1, sentTo(QStringLiteral("/api/capabilities")));
}

TEST_F(CrateServerFeatureTest, NameDoesNotCollideWithMixxxCrates) {
    Crate mixxxCrate;
    mixxxCrate.setName(QStringLiteral("Disco edits"));
    CrateId mixxxCrateId;
    ASSERT_TRUE(internalCollection()->insertCrate(mixxxCrate, &mixxxCrateId));
    const auto mixxxCrates = [this] {
        QSqlQuery query(dbConnection());
        QStringList rows;
        if (query.exec(QStringLiteral("SELECT id, name FROM crates ORDER BY id"))) {
            while (query.next()) {
                rows.append(query.value(0).toString() + QLatin1Char(':') +
                        query.value(1).toString());
            }
        }
        return rows;
    };
    const QStringList before = mixxxCrates();
    storeTree();
    CrateServerFeature* pFeature = makeFeature();
    QSignalSpy shown(pFeature, &LibraryFeature::showTrackModel);

    TreeItem* pCrate = childLabelled(childLabelled(root(), QStringLiteral("Crates")),
            QStringLiteral("Disco edits"));
    ASSERT_NE(nullptr, pCrate);
    pFeature->activateChild(indexOf(pCrate));
    ASSERT_EQ(1, shown.size());
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(QStringLiteral("/api/capabilities")) == 1; },
            5000));
    lastTo(QStringLiteral("/api/capabilities"))->pReply->respond(200, kCapabilities);
    const QString path = QStringLiteral(
            "/api/decks/containers/crate/4b1e7c2a-0000-4000-8000-000000000001/tracks");
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(path) == 1; }, 5000));
    lastTo(path)->pReply->respond(200,
            QByteArrayLiteral(R"({"tracks": [{"entity_uid": "e1", "title": "Lattice"}],
                "next_cursor": null})"));
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                return pCrate->getLabel() == QStringLiteral("Disco edits (1)");
            },
            5000));

    // Mixxx's crate of the same name is untouched, and has no tracks
    EXPECT_EQ(before, mixxxCrates());
    QSqlQuery crateTracks(dbConnection());
    ASSERT_TRUE(crateTracks.exec(QStringLiteral("SELECT COUNT(*) FROM crate_tracks")));
    ASSERT_TRUE(crateTracks.next());
    EXPECT_EQ(0, crateTracks.value(0).toInt());
}

} // namespace

namespace {

// F3.C-PR3: in the Crate section the library's search box asks the server's
// DJ search, and its results replace the tracks on screen until the search
// is cleared.
TEST_F(CrateServerFeatureTest, SearchInTheCrateSectionAsksTheServer) {
    storeTree();
    storeContainerTracks(containerId(QStringLiteral("playlist"), QStringLiteral("7")),
            {remoteTrack(QStringLiteral("e1"),
                    QStringLiteral("Lattice"),
                    0.7,
                    QStringLiteral("full"))});
    CrateServerFeature* pFeature = makeFeature();
    QSignalSpy shown(pFeature, &LibraryFeature::showTrackModel);
    TreeItem* pWarmUp = childLabelled(childLabelled(root(), QStringLiteral("Playlists")),
            QStringLiteral("Warm-up"));
    ASSERT_NE(nullptr, pWarmUp);
    pFeature->activateChild(indexOf(pWarmUp));
    ASSERT_EQ(1, shown.size());
    auto* pBrowsed = dynamic_cast<CrateServerTrackModel*>(
            shown.last().first().value<QAbstractItemModel*>());
    ASSERT_NE(nullptr, pBrowsed);
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(QStringLiteral("/api/capabilities")) == 1; },
            5000));
    lastTo(QStringLiteral("/api/capabilities"))->pReply->respond(200, kCapabilities);

    // What the user types goes to the server, verbatim, after the debounce
    pBrowsed->search(QStringLiteral("bpm:120-128 key:8A"));
    const QString search = QStringLiteral("/api/search");
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(search) == 1; }, 5000));
    const QUrlQuery query(lastTo(search)->request.url());
    EXPECT_EQ(QStringLiteral("dj"), query.queryItemValue(QStringLiteral("fields")));
    EXPECT_EQ(QStringLiteral("bpm:120-128 key:8A"),
            query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded));
    lastTo(search)->pReply->respond(200,
            QByteArrayLiteral(R"({"artists": [], "albums": [], "tracks": [
                {"entity_uid": "s2", "title": "Second Light", "artist": "Mirae",
                 "album": "Tides", "duration": 301.0, "bpm": 126.0, "camelot": "8A",
                 "energy": 0.8, "playable": true},
                {"entity_uid": "s1", "title": "First Light", "artist": "Mirae",
                 "album": "Tides", "duration": 280.0, "bpm": 121.0, "camelot": "8A",
                 "energy": 0.6, "playable": true}]})"));

    // The results, in the server's order, with the query in the search box
    ASSERT_TRUE(QTest::qWaitFor([&] { return shown.size() == 2; }, 5000));
    auto* pResults = dynamic_cast<CrateServerTrackModel*>(
            shown.last().first().value<QAbstractItemModel*>());
    ASSERT_NE(nullptr, pResults);
    ASSERT_TRUE(QTest::qWaitFor([&] { return pResults->rowCount() == 2; }, 5000));
    const int title = pResults->fieldIndex(QStringLiteral("title"));
    EXPECT_EQ(QStringLiteral("Second Light"), pResults->index(0, title).data().toString());
    EXPECT_EQ(QStringLiteral("First Light"), pResults->index(1, title).data().toString());
    EXPECT_EQ(QStringLiteral("bpm:120-128 key:8A"), pResults->currentSearch());
    // Not a sidebar node
    EXPECT_EQ(2, labels(childLabelled(root(), QStringLiteral("Playlists"))).size());
    EXPECT_EQ(5, labels(root()).size());

    // Clearing the search shows the playlist again, unfiltered
    pResults->search(QString());
    ASSERT_TRUE(QTest::qWaitFor([&] { return shown.size() == 3; }, 5000));
    EXPECT_EQ(pBrowsed, shown.last().first().value<QAbstractItemModel*>());
    EXPECT_TRUE(pBrowsed->currentSearch().isEmpty());
    ASSERT_TRUE(QTest::qWaitFor([&] { return pBrowsed->rowCount() == 1; }, 5000));
    EXPECT_EQ(1, sentTo(search));
}

} // namespace

namespace {

/// The longest gap between ticks of a 1 ms timer on the main thread: how long
/// it was blocked, at most.
class MainThreadStalls {
  public:
    MainThreadStalls() {
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(1);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] {
            if (m_clock.isValid()) {
                m_longestMs = std::max(m_longestMs, m_clock.restart());
            } else {
                m_clock.start();
            }
        });
        m_timer.start();
    }
    qint64 longestMs() const {
        return m_longestMs;
    }

  private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_longestMs = 0;
};

// The manual check of F3.C-PR3 (a 2,000-track playlist and the DJ search,
// against the budgets of design §9), offline: the tables and the network are
// local fakes, so it measures the client's own work only. Opt-in, since
// timings vary with the machine: CRATE_DECKS_MEASURE=1.
// TEST_GAP: the dev stack's latency is not in it (G3 session).
TEST_F(CrateServerFeatureTest, MeasureLargePlaylistAndSearch) {
    if (qEnvironmentVariableIntValue("CRATE_DECKS_MEASURE") != 1) {
        GTEST_SKIP() << "Measurement; run with CRATE_DECKS_MEASURE=1";
    }
    storeTree();
    QList<crate::library::RemoteTrack> tracks;
    for (int i = 0; i < 2000; ++i) {
        tracks.append(remoteTrack(QStringLiteral("p%1").arg(i),
                QStringLiteral("Track %1").arg(i),
                0.5,
                QStringLiteral("full")));
    }
    storeContainerTracks(containerId(QStringLiteral("playlist"), QStringLiteral("7")), tracks);
    CrateServerFeature* pFeature = makeFeature();
    QSignalSpy shown(pFeature, &LibraryFeature::showTrackModel);
    TreeItem* pWarmUp = childLabelled(childLabelled(root(), QStringLiteral("Playlists")),
            QStringLiteral("Warm-up"));
    ASSERT_NE(nullptr, pWarmUp);

    MainThreadStalls stalls;
    QElapsedTimer clock;
    clock.start();
    pFeature->activateChild(indexOf(pWarmUp));
    auto* pPlaylist = dynamic_cast<CrateServerTrackModel*>(
            shown.last().first().value<QAbstractItemModel*>());
    ASSERT_NE(nullptr, pPlaylist);
    ASSERT_TRUE(QTest::qWaitFor([&] { return pPlaylist->rowCount() == 2000; }, 10000));
    const qint64 openMs = clock.elapsed();
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(QStringLiteral("/api/capabilities")) == 1; },
            5000));
    lastTo(QStringLiteral("/api/capabilities"))->pReply->respond(200, kCapabilities);

    QByteArray body = QByteArrayLiteral(R"({"artists": [], "albums": [], "tracks": [)");
    for (int i = 0; i < 50; ++i) {
        if (i > 0) {
            body += ',';
        }
        body += QStringLiteral(R"({"entity_uid": "s%1", "title": "Result %1", "artist": "Mirae",
            "album": "Tides", "duration": 300.0, "bpm": 124.0, "camelot": "8A",
            "energy": 0.7, "playable": true})")
                        .arg(i)
                        .toUtf8();
    }
    body += QByteArrayLiteral("]}");
    pPlaylist->search(QStringLiteral("bpm:120-128 key:8A"));
    const QString search = QStringLiteral("/api/search");
    ASSERT_TRUE(QTest::qWaitFor([&] { return sentTo(search) == 1; }, 5000));
    clock.restart();
    lastTo(search)->pReply->respond(200, body);
    ASSERT_TRUE(QTest::qWaitFor([&] { return shown.size() == 2; }, 5000));
    auto* pResults = dynamic_cast<CrateServerTrackModel*>(
            shown.last().first().value<QAbstractItemModel*>());
    ASSERT_TRUE(QTest::qWaitFor([&] { return pResults->rowCount() == 50; }, 5000));
    const qint64 resultsMs = clock.elapsed();

    std::cout << "[ MEASURE  ] open 2000-track playlist: " << openMs
              << " ms; search answer to 50 rows: " << resultsMs
              << " ms; longest main-thread stall: " << stalls.longestMs() << " ms\n";
    // Design §9: a synced 500-track container in under 150 ms; the main
    // thread never blocked more than 16 ms
    EXPECT_LT(openMs, 600);
    EXPECT_LT(stalls.longestMs(), 16);
}

} // namespace
