// The cache view of the Crate section (plan F4.C-PR4): the audio cache's use,
// limit, downloads and offline sets, over a real AudioCache with the network
// faked.

#include "crate/ui/cachestatusview.h"

#include <gtest/gtest.h>

#include <QLabel>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <memory>
#include <optional>

#include "crate/cache/audiocache.h"
#include "crate/client/crateclient.h"
#include "crate/ui/cachestatusmodel.h"
#include "crate/ui/delegates/delegateroles.h"
#include "crate/ui/dialogs/levelgauge.h"
#include "test/crate/fakecratenetwork.h"

namespace {

using crate::CacheDownloadsModel;
using crate::CacheOfflineSetsModel;
using crate::CacheStatusModel;
using crate::CacheStatusView;
using crate::CacheTrackTitle;
using crate::Client;
using crate::ClientOptions;
using crate::cache::AudioCache;
using crate::cache::CacheTicket;
using crate::cache::Priority;
using crate::cache::StorageInfo;
using crate::cache::TrackHints;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeReply;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");
const QString kUid1 = QStringLiteral("0f6b3d9e-5a1c-4e2b-8d7f-1a2b3c4d5e61");
const QString kUid2 = QStringLiteral("1a2b3c4d-5a1c-4e2b-8d7f-1a2b3c4d5e62");
const QString kUid3 = QStringLiteral("2b3c4d5e-5a1c-4e2b-8d7f-1a2b3c4d5e63");
const QString kUid4 = QStringLiteral("3c4d5e6f-5a1c-4e2b-8d7f-1a2b3c4d5e64");
const QString kUid5 = QStringLiteral("4d5e6f70-5a1c-4e2b-8d7f-1a2b3c4d5e65");
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

TrackHints flacHints(qint64 byteLength) {
    TrackHints hints;
    hints.format = QStringLiteral("flac");
    hints.byteLength = byteLength;
    return hints;
}

class CacheStatusViewTest : public testing::Test {
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
        cacheOptions.limitBytes = 1000;
        cacheOptions.pStorageInfo = std::make_shared<FakeStorageInfo>();
        cacheOptions.nowMs = [this] { return ++m_nowMs; };
        m_pCache = std::make_unique<AudioCache>(std::move(cacheOptions));
        ASSERT_TRUE(QTest::qWaitFor([&] { return m_pCache->isLoaded(); }, kWaitMs));
    }

    void TearDown() override {
        m_pView.reset();
        m_pModel.reset();
        m_pCache.reset();
    }

    /// The view over the cache, with the library's titles of the tracks.
    void makeView() {
        CacheStatusModel::Options options;
        options.pCache = m_pCache.get();
        options.lookUpTitles = [this](const QList<crate::cache::TrackKey>& tracks,
                                       const CacheStatusModel::TitlesReady& ready) {
            QHash<QString, CacheTrackTitle> titles;
            for (const auto& track : tracks) {
                ++m_lookedUp;
                if (track.entityUid == kUid2) {
                    titles.insert(kServer + QLatin1Char('/') + kUid2,
                            {QStringLiteral("Second Light"), QStringLiteral("Mirae")});
                }
            }
            ready(titles);
        };
        options.offlineSetName = [](const QString& serverId, const QString& reason) {
            return serverId == kServer && reason == QLatin1String("offline-set:playlist:7")
                    ? QStringLiteral("Warm-up")
                    : QString();
        };
        options.refreshDelay = std::chrono::milliseconds(1);
        m_pModel = std::make_unique<CacheStatusModel>(std::move(options));
        m_pView = std::make_unique<CacheStatusView>(m_pModel.get(), nullptr);
        m_pView->resize(900, 600);
    }

    FakeReply* waitForStream(const QString& uid) {
        const QString path = QStringLiteral("/api/tracks/by-entity/%1/stream").arg(uid);
        FakeReply* pReply = nullptr;
        const bool sent = QTest::qWaitFor(
                [&] {
                    for (const auto& sent : std::as_const(m_network.sent)) {
                        if (sent.request.url().path() == path && sent.pReply &&
                                !sent.pReply->isFinished()) {
                            pReply = sent.pReply.data();
                        }
                    }
                    return pReply != nullptr;
                },
                kWaitMs);
        return sent ? pReply : nullptr;
    }

    /// Downloads a whole track of `size` bytes.
    void download(const QString& uid, qint64 size) {
        CacheTicket* pTicket = m_pCache->ensure(kServer, uid, Priority::Prefetch, flacHints(size));
        QSignalSpy ready(pTicket, &CacheTicket::ready);
        FakeReply* pReply = waitForStream(uid);
        ASSERT_NE(nullptr, pReply);
        pReply->respond(200, QByteArray(size, 'x'), audioHeaders(size));
        ASSERT_TRUE(ready.wait(kWaitMs));
    }

    QString cell(QTableView* pTable, int row, int column) const {
        return pTable->model()->index(row, column).data().toString();
    }

    QTemporaryDir m_dir;
    FakeNetworkAccessManager m_network;
    FakeScheduler m_scheduler;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<AudioCache> m_pCache;
    std::unique_ptr<CacheStatusModel> m_pModel;
    std::unique_ptr<CacheStatusView> m_pView;
    qint64 m_nowMs = 1000;
    int m_lookedUp = 0;
};

TEST_F(CacheStatusViewTest, ShowsUsageAndQueue) {
    // A complete track kept for an offline set, then three on their way:
    // two downloading (two per server) and one waiting
    download(kUid1, 400);
    m_pCache->pin(kServer, kUid1, QStringLiteral("offline-set:playlist:7"));
    m_pCache->pin(kServer, kUid5, QStringLiteral("offline-set:playlist:7"));
    makeView();
    m_pCache->ensure(kServer, kUid2, Priority::Deck, flacHints(100));
    m_pCache->ensure(kServer, kUid4, Priority::AutoDj, flacHints(100));
    m_pCache->ensure(kServer, kUid3, Priority::Prefetch, flacHints(100));
    FakeReply* pDeck = waitForStream(kUid2);
    ASSERT_NE(nullptr, pDeck);
    pDeck->respondHeaders(200, audioHeaders(100));
    pDeck->sendBody(QByteArray(25, 'x'));

    QTableView* pDownloads = m_pView->downloadsTable();
    const auto progressOf = [pDownloads](int row) {
        return pDownloads->model()
                ->index(row, CacheDownloadsModel::Progress)
                .data(crate::delegates::CacheProgressRole)
                .toDouble();
    };
    ASSERT_TRUE(QTest::qWaitFor(
            [&] { return pDownloads->model()->rowCount() == 3 && progressOf(0) == 0.25; },
            kWaitMs));
    // Use and limit, as the Crate page writes them
    EXPECT_EQ(400, m_pModel->usedBytes());
    EXPECT_EQ(1000, m_pModel->limitBytes());
    EXPECT_NEAR(0.4, m_pView->gauge()->value(), 1e-9);
    EXPECT_EQ(QStringLiteral("0.0"), m_pView->usageLabel()->text());
    EXPECT_EQ(QStringLiteral("1 tracks · 3 downloading · 400 B kept offline"),
            m_pView->captionLabel()->text());

    // On their way first, the deck's before Auto DJ's; then the prefetch
    // waiting for its turn. Titles come from the library; a track it does
    // not know shows its uid.
    EXPECT_EQ(QStringLiteral("Second Light"), cell(pDownloads, 0, CacheDownloadsModel::Title));
    EXPECT_EQ(QStringLiteral("Mirae"), cell(pDownloads, 0, CacheDownloadsModel::Artist));
    EXPECT_EQ(QStringLiteral("Deck"), cell(pDownloads, 0, CacheDownloadsModel::Purpose));
    EXPECT_EQ(QStringLiteral("25 B of 100 B"), cell(pDownloads, 0, CacheDownloadsModel::Progress));
    EXPECT_EQ(QStringLiteral("Auto DJ"), cell(pDownloads, 1, CacheDownloadsModel::Purpose));
    EXPECT_EQ(kUid4, cell(pDownloads, 1, CacheDownloadsModel::Title));
    EXPECT_EQ(QStringLiteral("Prefetch"), cell(pDownloads, 2, CacheDownloadsModel::Purpose));
    EXPECT_EQ(QStringLiteral("Queued"), cell(pDownloads, 2, CacheDownloadsModel::Progress));
    // Each track is looked up once, not for every chunk
    pDeck->sendBody(QByteArray(25, 'x'));
    QTest::qWait(20);
    EXPECT_EQ(3, m_lookedUp);

    // The offline set by its name, with what of it is complete
    QTableView* pSets = m_pView->offlineSetsTable();
    ASSERT_EQ(1, pSets->model()->rowCount());
    EXPECT_EQ(QStringLiteral("Warm-up"), cell(pSets, 0, CacheOfflineSetsModel::Name));
    EXPECT_EQ(QStringLiteral("1 of 2 tracks"), cell(pSets, 0, CacheOfflineSetsModel::Tracks));
    EXPECT_FALSE(pSets->isHidden());

    // The downloads end: the list empties and the use grows
    pDeck->sendBody(QByteArray(50, 'x'));
    pDeck->finishBody();
    for (const QString& uid : {kUid3, kUid4}) {
        FakeReply* pReply = waitForStream(uid);
        ASSERT_NE(nullptr, pReply);
        pReply->respond(200, QByteArray(100, 'x'), audioHeaders(100));
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return pDownloads->model()->rowCount() == 0; }, kWaitMs));
    EXPECT_TRUE(pDownloads->isHidden());
    EXPECT_EQ(700, m_pModel->usedBytes());
    EXPECT_EQ(4, m_pModel->readyFiles());
}

TEST_F(CacheStatusViewTest, ChangingLimitTriggersEviction) {
    download(kUid1, 300);
    download(kUid2, 300);
    download(kUid3, 300);
    m_pCache->pin(kServer, kUid1, QStringLiteral("offline-set:playlist:7"));
    makeView();
    ASSERT_EQ(900, m_pModel->usedBytes());
    QSignalSpy evicted(m_pCache.get(), &AudioCache::evicted);

    // The least recently used file that is not pinned goes at once
    m_pModel->setLimitBytes(700);

    ASSERT_EQ(1, evicted.size());
    EXPECT_EQ(kUid2, evicted.first().at(1).toString());
    EXPECT_EQ(700, m_pCache->limitBytes());
    EXPECT_EQ(600, m_pModel->usedBytes());
    EXPECT_EQ(700, m_pModel->limitBytes());
    EXPECT_NEAR(600.0 / 700.0, m_pView->gauge()->value(), 1e-9);
    EXPECT_EQ(QStringLiteral("2 tracks · 0 downloading · 300 B kept offline"),
            m_pView->captionLabel()->text());

    // Below what is pinned, the pinned file stays
    m_pModel->setLimitBytes(100);
    EXPECT_FALSE(m_pCache->cachedPath(kServer, kUid3).has_value());
    EXPECT_TRUE(m_pCache->cachedPath(kServer, kUid1).has_value());
    EXPECT_EQ(300, m_pModel->usedBytes());
}

} // namespace
