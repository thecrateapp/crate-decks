#include "crate/recs/compatiblesview.h"

#include <gtest/gtest.h>

#include <QComboBox>
#include <QFile>
#include <QHash>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <QUrlQuery>
#include <memory>

#include "crate/db/crateserverschema.h"
#include "crate/library/crateloadcoordinator.h"
#include "crate/recs/compatiblesmodel.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"
#include "track/track.h"

// The Compatibles view of the Crate section (plan F5.C-PR1): it follows the
// master deck's track, or the selected one, and asks the server for the
// tracks compatible with it.

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::recs::Compatibles;
using crate::recs::CompatiblesModel;
using crate::recs::CompatiblesView;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");
const QString kSeed = QStringLiteral("afe794ee-23ea-53d3-8280-ff633310da60");
const QString kOther = QStringLiteral("ed59e42b-4f23-5b71-b652-558e5f5b87b9");
const QString kThird = QStringLiteral("9e8c5fc0-0728-50b5-8237-cbb24efce49e");
constexpr std::chrono::milliseconds kDebounce{60};

class CrateCompatiblesViewTest : public MixxxDbTest {
  protected:
    CrateCompatiblesViewTest()
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
        m_pModel = std::make_unique<CompatiblesModel>(dbConnection(), dbConnectionPooler());
    }

    void TearDown() override {
        m_pCompatibles.reset();
        m_pModel.reset();
        m_pClient.reset();
    }

    Compatibles* makeCompatibles() {
        Compatibles::Options options;
        options.debounce = kDebounce;
        options.clientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pClient.get() : nullptr;
        };
        options.trackOfFile =
                [](const QString& location) -> std::optional<crate::cache::TrackKey> {
            // The cache's files are <cache>/<server>/<uid>.<ext>
            const QString prefix = QStringLiteral("/cache/") + kServer + QLatin1Char('/');
            if (!location.startsWith(prefix)) {
                return std::nullopt;
            }
            return crate::cache::TrackKey{
                    kServer, location.mid(prefix.size()).section(QLatin1Char('.'), 0, 0)};
        };
        options.pDbConnectionPool = dbConnectionPooler();
        options.prewarmEnabled = [this] {
            return m_prewarmEnabled;
        };
        options.prewarm = [this](const QList<crate::library::CrateTrackRef>& tracks) {
            for (const auto& track : tracks) {
                m_prewarmed.append(track.entityUid);
            }
        };
        m_pCompatibles = std::make_unique<Compatibles>(m_pModel.get(), std::move(options));
        return m_pCompatibles.get();
    }

    static TrackPointer crateTrack(const QString& uid) {
        return Track::newTemporary(QStringLiteral("/cache/") + kServer + QLatin1Char('/') + uid +
                QStringLiteral(".flac"));
    }

    QByteArray fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/recs/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return file.readAll();
    }

    /// Waits past the debounce for `count` requests in all.
    bool waitForRequests(int count) {
        return QTest::qWaitFor(
                [&] {
                    return m_network.sent.size() >= count;
                },
                2000);
    }

    /// The uid the request asks compatibles of.
    static QString seedOf(const FakeNetworkAccessManager::Sent& sent) {
        return sent.request.url().path().section(QLatin1Char('/'), 4, 4);
    }

    /// Answers the last request with the real answer and waits for the model.
    void answerLast() {
        QSignalSpy ready(m_pCompatibles.get(), &Compatibles::stateChanged);
        m_network.last().pReply->respond(200, fixture(QStringLiteral("real-compatibles.json")));
        ASSERT_TRUE(QTest::qWaitFor(
                [&] {
                    return m_pCompatibles->state() == Compatibles::State::Ready;
                },
                5000));
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<CompatiblesModel> m_pModel;
    std::unique_ptr<Compatibles> m_pCompatibles;
    bool m_prewarmEnabled = false;
    QStringList m_prewarmed;
};

TEST_F(CrateCompatiblesViewTest, FollowsMasterDeck) {
    Compatibles* pCompatibles = makeCompatibles();
    pCompatibles->setMasterTrack(crateTrack(kSeed));
    ASSERT_TRUE(waitForRequests(1));
    const auto sent = m_network.last();
    EXPECT_EQ(QStringLiteral("/api/tracks/by-entity/%1/compatible").arg(kSeed),
            sent.request.url().path());
    const QUrlQuery query(sent.request.url());
    EXPECT_QSTRING_EQ(QStringLiteral("auto"), query.queryItemValue(QStringLiteral("context")));
    EXPECT_QSTRING_EQ(QStringLiteral("50"), query.queryItemValue(QStringLiteral("limit")));
    EXPECT_EQ(Compatibles::State::Loading, pCompatibles->state());
    answerLast();
    EXPECT_EQ(8, m_pModel->rowCount());
    EXPECT_QSTRING_EQ(kSeed, m_pModel->seedEntityUid());

    // Another deck goes on air
    pCompatibles->setMasterTrack(crateTrack(kOther));
    ASSERT_TRUE(waitForRequests(2));
    EXPECT_QSTRING_EQ(kOther, seedOf(m_network.last()));

    // Following the selection: the master deck no longer counts, the
    // selection does, and back
    m_network.last().pReply->respond(200, fixture(QStringLiteral("real-compatibles.json")));
    pCompatibles->setFollow(Compatibles::Follow::Selection);
    pCompatibles->setMasterTrack(crateTrack(kThird));
    QTest::qWait(static_cast<int>(kDebounce.count()) * 3);
    EXPECT_EQ(2, m_network.sent.size());
    pCompatibles->setSelectedCrateTrack(crate::cache::TrackKey{kServer, kThird});
    ASSERT_TRUE(waitForRequests(3));
    EXPECT_QSTRING_EQ(kThird, seedOf(m_network.last()));
    // The master deck has kThird too: nothing new to ask
    pCompatibles->setFollow(Compatibles::Follow::MasterDeck);
    QTest::qWait(static_cast<int>(kDebounce.count()) * 3);
    EXPECT_EQ(3, m_network.sent.size());

    // Another context asks again
    m_network.last().pReply->respond(200, fixture(QStringLiteral("real-compatibles.json")));
    pCompatibles->setContext(crate::recs::CompatibleContext::Beatmatch);
    ASSERT_TRUE(waitForRequests(4));
    EXPECT_QSTRING_EQ(QStringLiteral("beatmatch"),
            QUrlQuery(m_network.last().request.url())
                    .queryItemValue(QStringLiteral("context")));
}

TEST_F(CrateCompatiblesViewTest, DebouncesRapidChanges) {
    Compatibles* pCompatibles = makeCompatibles();
    // A DJ flicking through decks: only the last one is asked, once
    pCompatibles->setMasterTrack(crateTrack(kSeed));
    QTest::qWait(static_cast<int>(kDebounce.count()) / 3);
    pCompatibles->setMasterTrack(crateTrack(kOther));
    QTest::qWait(static_cast<int>(kDebounce.count()) / 3);
    pCompatibles->setMasterTrack(crateTrack(kThird));
    EXPECT_TRUE(m_network.sent.isEmpty()) << "Asked before the track stayed";
    ASSERT_TRUE(waitForRequests(1));
    QTest::qWait(static_cast<int>(kDebounce.count()) * 3);
    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_QSTRING_EQ(kThird, seedOf(m_network.last()));
}

TEST_F(CrateCompatiblesViewTest, CancelsStaleRequest) {
    Compatibles* pCompatibles = makeCompatibles();
    pCompatibles->setMasterTrack(crateTrack(kSeed));
    ASSERT_TRUE(waitForRequests(1));
    QPointer<crate::test::FakeReply> pStale = m_network.last().pReply;

    // The master deck changes while the server is answering: that request
    // goes, and its answer never shows
    pCompatibles->setMasterTrack(crateTrack(kOther));
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                return !pStale || pStale->aborted();
            },
            2000));
    EXPECT_EQ(0, m_pModel->rowCount());
    ASSERT_TRUE(waitForRequests(2));
    EXPECT_QSTRING_EQ(kOther, seedOf(m_network.last()));
    answerLast();
    EXPECT_QSTRING_EQ(kOther, m_pModel->seedEntityUid());
}

TEST_F(CrateCompatiblesViewTest, PrewarmsTopFiveWhenEnabled) {
    Compatibles* pCompatibles = makeCompatibles();
    // Off: nothing is downloaded ahead
    pCompatibles->setMasterTrack(crateTrack(kSeed));
    ASSERT_TRUE(waitForRequests(1));
    answerLast();
    EXPECT_TRUE(m_prewarmed.isEmpty());

    // On: the five best, best first
    m_prewarmEnabled = true;
    pCompatibles->setMasterTrack(crateTrack(kOther));
    ASSERT_TRUE(waitForRequests(2));
    answerLast();
    ASSERT_EQ(5, m_prewarmed.size());
    for (int row = 0; row < 5; ++row) {
        EXPECT_QSTRING_EQ(m_pModel->entityUid(row), m_prewarmed[row]);
    }
}

TEST_F(CrateCompatiblesViewTest, NonCrateTrackShowsHint) {
    Compatibles* pCompatibles = makeCompatibles();
    // The view takes the table as its child
    CompatiblesView view(pCompatibles, new QTableView(), nullptr);
    EXPECT_EQ(Compatibles::State::NoCrateTrack, pCompatibles->state());
    EXPECT_FALSE(view.hint()->isHidden());
    EXPECT_QSTRING_EQ(CompatiblesView::hintFor(Compatibles::State::NoCrateTrack),
            view.hint()->text());

    pCompatibles->setMasterTrack(crateTrack(kSeed));
    ASSERT_TRUE(waitForRequests(1));
    answerLast();
    EXPECT_TRUE(view.hint()->isHidden());
    EXPECT_EQ(8, m_pModel->rowCount());

    // A local track on air: nothing to ask, and the list empties
    pCompatibles->setMasterTrack(Track::newTemporary(QStringLiteral("/music/local.flac")));
    QTest::qWait(static_cast<int>(kDebounce.count()) * 3);
    EXPECT_EQ(1, m_network.sent.size());
    EXPECT_EQ(Compatibles::State::NoCrateTrack, pCompatibles->state());
    EXPECT_EQ(0, m_pModel->rowCount());
    EXPECT_FALSE(view.hint()->isHidden());
    EXPECT_QSTRING_EQ(CompatiblesView::hintFor(Compatibles::State::NoCrateTrack),
            view.hint()->text());
    // An empty deck the same
    pCompatibles->setMasterTrack(TrackPointer());
    EXPECT_EQ(Compatibles::State::NoCrateTrack, pCompatibles->state());
}

TEST_F(CrateCompatiblesViewTest, TrackWithoutProfileSaysSo) {
    Compatibles* pCompatibles = makeCompatibles();
    pCompatibles->setMasterTrack(crateTrack(kSeed));
    ASSERT_TRUE(waitForRequests(1));
    // The server's answer for a track without a usable Smart Mix profile
    m_network.last().pReply->respond(
            404, QByteArrayLiteral(R"({"detail":"Track or Smart Mix profile not found"})"));
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                return pCompatibles->state() == Compatibles::State::NoProfile;
            },
            2000));
    EXPECT_EQ(0, m_pModel->rowCount());
    EXPECT_FALSE(CompatiblesView::hintFor(Compatibles::State::NoProfile).isEmpty());
}

} // namespace
