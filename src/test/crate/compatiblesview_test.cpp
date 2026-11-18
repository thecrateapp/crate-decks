#include "crate/recs/compatiblesview.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QHash>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <QUrlQuery>
#include <memory>

#include "crate/db/crateserverschema.h"
#include "crate/library/cratelibrarystore.h"
#include "crate/library/crateloadcoordinator.h"
#include "crate/recs/compatiblesmodel.h"
#include "crate/ui/widgets/camelotwheel.h"
#include "crate/ui/widgets/camelotwheelmodel.h"
#include "crate/ui/widgets/instrumentpainter.h"
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

TEST(CompatiblesViewLayoutTest, BandFitsThePaneOrGivesWay) {
    const struct {
        int available;
        int wheelMaximum;
        int content;
        bool compact;
        int wheel;
        int height;
    } cases[] = {
            // Maximised at 1920 x 1080: the mockup's 470 px wheel
            {750, 470, 390, false, 470, 502},
            // Less room: the wheel shrinks with the pane
            {436, 462, 386, false, 404, 436},
            // Columns taller than the wheel: the band is as tall as they are
            {600, 300, 386, false, 300, 418},
            // Short of what the columns need: the strip
            {410, 470, 386, true, 235, 418},
            // Short of the wheel's minimum
            {200, 470, 150, true, 235, 267},
            // Too narrow for the wheel's minimum
            {900, 200, 300, true, 235, 332},
    };
    for (const auto& item : cases) {
        const auto layout = CompatiblesView::bandLayout(
                item.available, 32, 235, item.wheelMaximum, item.content);
        EXPECT_EQ(item.compact, layout.compact) << item.available << " " << item.wheelMaximum;
        EXPECT_EQ(item.wheel, layout.wheel) << item.available << " " << item.wheelMaximum;
        EXPECT_EQ(item.height, layout.height) << item.available << " " << item.wheelMaximum;
        if (!layout.compact) {
            EXPECT_LE(layout.height, item.available);
        }
    }
}

TEST(CompatiblesViewLayoutTest, LoadsIntoTheMasterDecksPartner) {
    const auto none = [](int) {
        return false;
    };
    const auto deck = [](int playing) {
        return [playing](int index) {
            return index == playing;
        };
    };
    EXPECT_EQ(1, CompatiblesView::targetDeck(0, 2, none));
    EXPECT_EQ(0, CompatiblesView::targetDeck(1, 2, none));
    EXPECT_EQ(-1, CompatiblesView::targetDeck(0, 2, deck(1)));
    EXPECT_EQ(3, CompatiblesView::targetDeck(2, 4, none));
    // The partner plays: the first other deck that does not
    EXPECT_EQ(0, CompatiblesView::targetDeck(2, 4, deck(3)));
    EXPECT_EQ(2, CompatiblesView::targetDeck(1, 4, deck(0)));
    // No master deck: the first deck that is stopped
    EXPECT_EQ(0, CompatiblesView::targetDeck(-1, 2, none));
    EXPECT_EQ(1, CompatiblesView::targetDeck(-1, 2, deck(0)));
}

TEST(CompatiblesViewLayoutTest, SaysHowOldTheResultsAre) {
    EXPECT_EQ(QStringLiteral("0.4 s ago"), CompatiblesView::ageText(400));
    EXPECT_EQ(QStringLiteral("0.0 s ago"), CompatiblesView::ageText(-5));
    EXPECT_EQ(QStringLiteral("12 s ago"), CompatiblesView::ageText(12'400));
    EXPECT_EQ(QStringLiteral("3 min ago"), CompatiblesView::ageText(180'000));
    EXPECT_EQ(QStringLiteral("2 h ago"), CompatiblesView::ageText(7'300'000));
}

TEST_F(CrateCompatiblesViewTest, ChipsPickWhatToFollowAndTheContext) {
    Compatibles* pCompatibles = makeCompatibles();
    CompatiblesView view(pCompatibles, new QTableView(), nullptr);
    ASSERT_NE(nullptr, view.followChip(Compatibles::Follow::Selection));
    EXPECT_TRUE(view.followChip(Compatibles::Follow::MasterDeck)->isChecked());
    EXPECT_TRUE(view.contextChip(crate::recs::CompatibleContext::Auto)->isChecked());
    view.followChip(Compatibles::Follow::Selection)->click();
    EXPECT_EQ(Compatibles::Follow::Selection, pCompatibles->follow());
    EXPECT_TRUE(view.followChip(Compatibles::Follow::Selection)->isChecked());
    EXPECT_FALSE(view.followChip(Compatibles::Follow::MasterDeck)->isChecked());
    view.contextChip(crate::recs::CompatibleContext::Smooth)->click();
    EXPECT_EQ(crate::recs::CompatibleContext::Smooth, pCompatibles->context());
    EXPECT_TRUE(view.contextChip(crate::recs::CompatibleContext::Smooth)->isChecked());
}

TEST_F(CrateCompatiblesViewTest, ShowsTheBandOrTheStripAndNeverGrows) {
    // The followed track in the catalogue, for the band's summary
    crate::library::RemoteTrack seed;
    seed.entityUid = kSeed;
    seed.title = QStringLiteral("Lattice");
    seed.artist = QStringLiteral("Ondine Varga");
    seed.bpm = 124.0;
    seed.camelot = QStringLiteral("8A");
    ASSERT_EQ(crate::library::WriteStatus::Ok,
            crate::library::storeTracks(
                    dbConnection(), kServer, {seed}, QStringLiteral("2026-11-18")));
    Compatibles* pCompatibles = makeCompatibles();
    const auto style = crate::widgets::InstrumentStyle::installed();
    crate::widgets::CamelotWheelModel wheelModel(style.opacityContext);
    auto* pTable = new QTableView();
    pTable->setModel(m_pModel.get());
    CompatiblesView view(pCompatibles, pTable, &wheelModel);
    EXPECT_EQ(QSize(0, 0), view.minimumSizeHint());
    pCompatibles->setMasterDeck(QStringLiteral("[Channel1]"));
    pCompatibles->setMasterTrack(crateTrack(kSeed));
    ASSERT_TRUE(waitForRequests(1));
    answerLast();

    // The summary names the key and counts the results
    EXPECT_QSTRING_EQ(QStringLiteral("8A · A minor"), view.summaryTitle()->text());
    EXPECT_TRUE(view.headMeta()->text().contains(QStringLiteral("Lattice")));
    EXPECT_TRUE(view.headMeta()->text().startsWith(QStringLiteral("Deck A")));
    EXPECT_TRUE(view.countLabel()->text().startsWith(QStringLiteral("8 tracks")));
    EXPECT_EQ(crate::widgets::CamelotWheelModel::keyOfCamelot(QStringLiteral("8A")),
            wheelModel.masterKey());
    EXPECT_QSTRING_EQ(QStringLiteral("A minor"), view.wheel()->subtitle());
    EXPECT_EQ(5, view.prewarmRings().size());
    EXPECT_QSTRING_EQ(QStringLiteral("0 / 5 ready"), view.prewarmLabel()->text());

    // A tall pane: the band, with the mockup's wheel, the list under it
    view.resize(1680, 1000);
    view.show();
    QCoreApplication::processEvents();
    EXPECT_FALSE(view.isCompact());
    EXPECT_TRUE(view.band()->isVisible());
    EXPECT_FALSE(view.strip()->isVisible());
    EXPECT_EQ(style.camelotWheelSize, view.wheel()->width());
    EXPECT_GE(pTable->geometry().top(), view.band()->geometry().bottom());
    EXPECT_EQ(view.height(), pTable->geometry().bottom() + 1);

    // A short one: the strip, and the list takes the pane; the toggle shows
    // the wheel anyway
    view.resize(1680, 400);
    QCoreApplication::processEvents();
    EXPECT_TRUE(view.isCompact());
    EXPECT_FALSE(view.band()->isVisible());
    EXPECT_TRUE(view.strip()->isVisible());
    EXPECT_TRUE(view.stripCounts()->text().contains(QStringLiteral("Relative")));
    EXPECT_EQ(view.strip()->geometry().bottom() + 1, pTable->geometry().top());
    view.wheelToggle()->click();
    EXPECT_TRUE(view.band()->isVisible());
    EXPECT_GE(view.wheel()->width(), 235);
    EXPECT_EQ(QSize(0, 0), view.minimumSizeHint());

    // The selected track, ready to load into deck B or to plan
    pTable->selectRow(0);
    EXPECT_QSTRING_EQ(QStringLiteral("Selected · 1 of 8"), view.selectedLabel()->text());
    EXPECT_QSTRING_EQ(QStringLiteral("Load in deck B"), view.loadButton()->text());
    EXPECT_FALSE(view.badges().first()->isHidden());
    // At 1366 x 768 with the library maximised: the badges that do not fit
    // the side column drop from the end, none is cut
    view.resize(1126, 616);
    QCoreApplication::processEvents();
    ASSERT_TRUE(view.band()->isVisible());
    EXPECT_FALSE(view.badges().first()->isHidden());
    for (const QLabel* pBadge : view.badges()) {
        if (!pBadge->isHidden()) {
            EXPECT_LE(pBadge->geometry().right(), pBadge->parentWidget()->width())
                    << pBadge->text().toStdString();
        }
    }
    QSignalSpy plan(&view, &CompatiblesView::planTransitionRequested);
    view.planButton()->click();
    ASSERT_EQ(1, plan.size());
    EXPECT_QSTRING_EQ(QStringLiteral("[Channel2]"), plan.first().first().toString());
}

} // namespace
