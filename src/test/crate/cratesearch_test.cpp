#include "crate/library/cratesearch.h"

#include <gtest/gtest.h>

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

// The DJ search of the Crate section (plan F3.C-PR3): debounced, the query
// verbatim to GET /api/search?fields=dj, the results as the server's
// ephemeral container of kind "search".

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::library::Search;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");

/// A track of GET /api/search?fields=dj as the server serves it
/// (SearchTrackResultResponse): duration in seconds, no format, bitrate or
/// profile quality, and `playable` (S9).
QByteArray searchTrack(const QString& uid, const QString& title, double bpm) {
    return QStringLiteral(
            R"({"id": 812, "entity_uid": "%1", "slug": "t", "title": "%2",
                "artist": "Kessler & Moor", "album_id": 41, "album": "Night",
                "has_cover": true, "duration": 372.5, "year": "2024",
                "genre": "Melodic Techno", "bpm": %3, "audio_key": "A",
                "audio_scale": "minor", "camelot": "8A", "energy": 0.7,
                "analysis_required": false, "cover_url": "/api/vdj/albums/41/cover?size=512",
                "playable": true})")
            .arg(uid, title, QString::number(bpm))
            .toUtf8();
}

QByteArray results(const QList<QByteArray>& tracks) {
    QByteArray body = QByteArrayLiteral(R"({"artists": [], "albums": [], "tracks": [)");
    for (int i = 0; i < tracks.size(); ++i) {
        if (i > 0) {
            body += ',';
        }
        body += tracks[i];
    }
    return body + QByteArrayLiteral("]}");
}

class CrateSearchTest : public MixxxDbTest {
  protected:
    CrateSearchTest()
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
        m_pSearch.reset();
        m_pClient.reset();
    }

    Search* makeSearch(Search::Options options = Search::Options()) {
        m_pSearch = std::make_unique<Search>(
                kServer, m_pClient.get(), dbConnectionPooler(), std::move(options));
        return m_pSearch.get();
    }

    /// Waits past the debounce for the request.
    void waitForRequests(int count) {
        for (int i = 0; i < 40 && m_network.sent.size() < count; ++i) {
            QTest::qWait(25);
        }
        ASSERT_EQ(count, m_network.sent.size());
    }

    static QString query(const QNetworkRequest& request, const QString& key) {
        return QUrlQuery(request.url()).queryItemValue(key, QUrl::FullyDecoded);
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<Search> m_pSearch;
};

TEST_F(CrateSearchTest, DebouncesAndCancelsPrevious) {
    Search::Options options;
    options.debounce = std::chrono::milliseconds(200);
    Search* pSearch = makeSearch(options);
    QSignalSpy ready(pSearch, &Search::resultsReady);

    // Typing: only the last query goes, once the user stops
    pSearch->search(QStringLiteral("Kes"));
    QTest::qWait(80);
    pSearch->search(QStringLiteral("Kessler"));
    QTest::qWait(120);
    EXPECT_TRUE(m_network.sent.isEmpty()) << "Asked before the user stopped typing";
    waitForRequests(1);
    EXPECT_QSTRING_EQ(QStringLiteral("Kessler"), query(m_network.last().request, "q"));
    QPointer<crate::test::FakeReply> pFirst = m_network.last().pReply;

    // A new query cancels the one in flight at once, not when it is sent
    pSearch->search(QStringLiteral("Kessler & Moor"));
    ASSERT_TRUE(pFirst);
    EXPECT_TRUE(pFirst->aborted());
    waitForRequests(2);
    EXPECT_QSTRING_EQ(QStringLiteral("Kessler & Moor"), query(m_network.last().request, "q"));

    m_network.last().pReply->respond(200,
            results({searchTrack(QStringLiteral("t-1"), QStringLiteral("Night Drive"), 124)}));
    ASSERT_TRUE(ready.wait(2000));
    ASSERT_EQ(1, ready.size());
    EXPECT_QSTRING_EQ(QStringLiteral("Kessler & Moor"), ready.first().at(1).toString());

    // Clearing the query drops what is pending: nothing more is asked
    pSearch->search(QStringLiteral("Kessler & Moor live"));
    pSearch->search(QString());
    QTest::qWait(350);
    EXPECT_EQ(2, m_network.sent.size());
    EXPECT_FALSE(pSearch->isBusy());
}

TEST_F(CrateSearchTest, PassesDjFiltersVerbatim) {
    Search::Options options;
    options.debounce = std::chrono::milliseconds(10);
    Search* pSearch = makeSearch(options);

    pSearch->search(QStringLiteral("bpm:120-128 key:8A"));
    waitForRequests(1);
    const QNetworkRequest request = m_network.last().request;
    EXPECT_EQ(QNetworkAccessManager::GetOperation, m_network.last().operation);
    EXPECT_QSTRING_EQ(QStringLiteral("/api/search"), request.url().path());
    EXPECT_QSTRING_EQ(QStringLiteral("dj"), query(request, "fields"));
    EXPECT_QSTRING_EQ(QStringLiteral("bpm:120-128 key:8A"), query(request, "q"));
    EXPECT_QSTRING_EQ(QStringLiteral("50"), query(request, "limit"));
    EXPECT_FALSE(request.url().toString().contains(QStringLiteral("token")));
    EXPECT_TRUE(request.rawHeader("Authorization").startsWith("Bearer "));

    // Quotes, colons and spaces of the DJ grammar reach the server as typed
    pSearch->search(QStringLiteral("artist:\"Kessler & Moor\" energy:0.6-0.8 analyzed:yes"));
    waitForRequests(2);
    EXPECT_QSTRING_EQ(QStringLiteral("artist:\"Kessler & Moor\" energy:0.6-0.8 analyzed:yes"),
            query(m_network.last().request, "q"));

    // The server does not search fewer than two characters: nothing goes
    pSearch->search(QStringLiteral(" a "));
    QTest::qWait(100);
    EXPECT_EQ(2, m_network.sent.size());
}

TEST_F(CrateSearchTest, ShowsResultsAsEphemeralContainer) {
    namespace lib = crate::library;
    // A track the catalogue already has from a container page, with the
    // fields a search result lacks
    lib::RemoteTrack known;
    known.entityUid = QStringLiteral("t-2");
    known.title = QStringLiteral("Old title");
    known.format = QStringLiteral("flac");
    known.bitrate = 1411;
    known.profileQuality = QStringLiteral("full");
    ASSERT_EQ(lib::WriteStatus::Ok,
            lib::storeTracks(dbConnection(), kServer, {known}, QStringLiteral("2026-11-10")));

    Search::Options options;
    options.debounce = std::chrono::milliseconds(10);
    std::atomic<bool> parsedOffMainThread = false;
    QThread* pMainThread = QThread::currentThread();
    options.onParse = [&] {
        parsedOffMainThread = QThread::currentThread() != pMainThread;
    };
    Search* pSearch = makeSearch(options);
    QSignalSpy ready(pSearch, &Search::resultsReady);

    pSearch->search(QStringLiteral("night"));
    waitForRequests(1);
    m_network.last().pReply->respond(200,
            results({searchTrack(QStringLiteral("t-2"), QStringLiteral("Night Drive"), 124),
                    searchTrack(QStringLiteral("t-1"), QStringLiteral("Night Call"), 122.5)}));
    ASSERT_TRUE(ready.wait(2000));
    EXPECT_TRUE(parsedOffMainThread);
    const qint64 containerId = ready.first().at(0).toLongLong();
    ASSERT_GT(containerId, 0);

    // The results, in the server's order, as the container of kind search
    const auto container = lib::loadContainer(dbConnection(), containerId);
    ASSERT_TRUE(container.has_value());
    EXPECT_QSTRING_EQ(Search::kKind, container->kind);
    EXPECT_QSTRING_EQ(kServer, container->serverId);
    EXPECT_QSTRING_EQ(QStringLiteral("night"), container->name);
    EXPECT_EQ((QStringList{QStringLiteral("t-2"), QStringLiteral("t-1")}),
            lib::loadContainerTrackUids(dbConnection(), containerId, 0, 10));
    // Not a sidebar section
    EXPECT_FALSE(lib::sectionOf(Search::kKind).has_value());

    // The search's fields update the catalogue; the ones it lacks stay
    QSqlQuery track(dbConnection());
    ASSERT_TRUE(track.exec(QStringLiteral(
            "SELECT title, format, bitrate, profile_quality, duration_ms, bpm, camelot, "
            "playable FROM crateserver_tracks WHERE entity_uid = 't-2'")));
    ASSERT_TRUE(track.next());
    EXPECT_QSTRING_EQ(QStringLiteral("Night Drive"), track.value(0).toString());
    EXPECT_QSTRING_EQ(QStringLiteral("flac"), track.value(1).toString());
    EXPECT_EQ(1411, track.value(2).toInt());
    EXPECT_QSTRING_EQ(QStringLiteral("full"), track.value(3).toString());
    EXPECT_EQ(372500, track.value(4).toLongLong());
    EXPECT_DOUBLE_EQ(124.0, track.value(5).toDouble());
    EXPECT_QSTRING_EQ(QStringLiteral("8A"), track.value(6).toString());
    EXPECT_TRUE(track.value(7).toBool());
    track.finish();

    // A sync of the server's tree, which never lists it, keeps it
    ASSERT_EQ(lib::WriteStatus::Ok,
            lib::storeContainerTree(dbConnection(), kServer, QList<lib::RemoteContainer>{}));
    EXPECT_EQ((QStringList{QStringLiteral("t-2"), QStringLiteral("t-1")}),
            lib::loadContainerTrackUids(dbConnection(), containerId, 0, 10));

    // The next search replaces the results in the same container
    pSearch->search(QStringLiteral("bpm:122"));
    waitForRequests(2);
    m_network.last().pReply->respond(200,
            results({searchTrack(QStringLiteral("t-1"), QStringLiteral("Night Call"), 122.5)}));
    ASSERT_TRUE(ready.wait(2000));
    EXPECT_EQ(containerId, ready.last().at(0).toLongLong());
    EXPECT_EQ(QStringList{QStringLiteral("t-1")},
            lib::loadContainerTrackUids(dbConnection(), containerId, 0, 10));
    EXPECT_QSTRING_EQ(QStringLiteral("bpm:122"),
            lib::loadContainer(dbConnection(), containerId)->name);
}

TEST_F(CrateSearchTest, ReportsAnInvalidDjQuery) {
    Search::Options options;
    options.debounce = std::chrono::milliseconds(10);
    Search* pSearch = makeSearch(options);
    QSignalSpy failed(pSearch, &Search::searchFailed);
    QSignalSpy ready(pSearch, &Search::resultsReady);

    pSearch->search(QStringLiteral("key:13Z"));
    waitForRequests(1);
    m_network.last().pReply->respond(400,
            QByteArrayLiteral(R"({"detail": "key must be a Camelot key such as 8A"})"));
    // The error may come at once, before the spy waits
    ASSERT_TRUE(!failed.isEmpty() || failed.wait(2000));
    EXPECT_QSTRING_EQ(QStringLiteral("key:13Z"), failed.first().at(0).toString());
    EXPECT_TRUE(ready.isEmpty());
}

} // namespace
