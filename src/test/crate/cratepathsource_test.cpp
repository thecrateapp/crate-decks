#include "crate/recs/cratepathsource.h"

#include <gtest/gtest.h>

#include <QDialogButtonBox>
#include <QFile>
#include <QFrame>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStringList>
#include <QTest>
#include <memory>

#include "crate/recs/recommendationqueue.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxtest.h"

// Music Paths as a source of Auto DJ (plan F5.C-PR3): a path from one track
// to another goes, in order, into Auto DJ's queue or into a new playlist.

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::recs::CratePathSource;
using crate::recs::MusicPath;
using crate::recs::PathRequest;
using crate::recs::RecommendedTrack;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");
/// The ends of the real path, from the dev stack: Depeche Mode to Adult.
const QString kFrom = QStringLiteral("afe794ee-23ea-53d3-8280-ff633310da60");
const QString kTo = QStringLiteral("ed59e42b-4f23-5b71-b652-558e5f5b87b9");

class FakeQueue : public crate::recs::RecommendationQueue {
  public:
    QList<QList<RecommendedTrack>> enqueued;
    QList<std::pair<QString, QList<RecommendedTrack>>> playlists;

    void enqueue(const QString& serverId, const QList<RecommendedTrack>& tracks) override {
        EXPECT_QSTRING_EQ(kServer, serverId);
        enqueued.append(tracks);
    }

    void readQueue(
            const std::function<void(const QList<crate::cache::TrackKey>&)>& done) override {
        done({});
    }

    QString saveAsPlaylist(const QString& name,
            const QString& serverId,
            const QList<RecommendedTrack>& tracks) override {
        EXPECT_QSTRING_EQ(kServer, serverId);
        playlists.append({name, tracks});
        return name + QStringLiteral(" (2)");
    }
};

class CratePathSourceTest : public MixxxTest {
  protected:
    void SetUp() override {
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(options));
        m_pClient->setTokenSource(&m_tokens);
        m_pPaths = std::make_unique<CratePathSource>(&m_queue);
    }

    void TearDown() override {
        m_pPaths.reset();
        m_pClient.reset();
    }

    QByteArray fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/recs/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return file.readAll();
    }

    bool waitForRequests(int count) {
        return QTest::qWaitFor(
                [&] {
                    return m_network.sent.size() >= count;
                },
                2000);
    }

    static PathRequest pathRequest(PathRequest::Destination destination) {
        PathRequest request;
        request.serverId = kServer;
        request.fromEntityUid = kFrom;
        request.toEntityUid = kTo;
        request.steps = 6;
        request.destination = destination;
        return request;
    }

    /// The uids of the real path, in step order.
    QStringList realPathUids() const {
        const auto path = crate::recs::parseMusicPath(fixture(QStringLiteral(
                "real-path-preview.json")));
        EXPECT_TRUE(path.ok());
        QStringList uids;
        if (path.ok()) {
            for (const RecommendedTrack& track : path.value().tracks) {
                uids.append(track.entityUid);
            }
        }
        return uids;
    }

    static QStringList uidsOf(const QList<RecommendedTrack>& tracks) {
        QStringList uids;
        for (const RecommendedTrack& track : tracks) {
            uids.append(track.entityUid);
        }
        return uids;
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    FakeQueue m_queue;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<CratePathSource> m_pPaths;
};

TEST_F(CratePathSourceTest, EnqueuesPathInOrder) {
    QSignalSpy finished(m_pPaths.get(), &CratePathSource::finished);
    m_pPaths->request(m_pClient.get(), pathRequest(PathRequest::Destination::AutoDjQueue));
    ASSERT_TRUE(waitForRequests(1));
    const auto sent = m_network.last();
    EXPECT_EQ(QNetworkAccessManager::PostOperation, sent.operation);
    EXPECT_EQ(QStringLiteral("/api/paths/preview"), sent.request.url().path());
    const QJsonObject body = QJsonDocument::fromJson(sent.body).object();
    EXPECT_EQ((QJsonObject{{QStringLiteral("type"), QStringLiteral("track")},
                      {QStringLiteral("value"), kFrom}}),
            body.value(QStringLiteral("origin")).toObject());
    EXPECT_EQ((QJsonObject{{QStringLiteral("type"), QStringLiteral("track")},
                      {QStringLiteral("value"), kTo}}),
            body.value(QStringLiteral("destination")).toObject());
    EXPECT_EQ(6, body.value(QStringLiteral("step_count")).toInt());

    sent.pReply->respond(200, fixture(QStringLiteral("real-path-preview.json")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !finished.isEmpty(); }, 2000));
    // The whole path, both ends included, in its steps' order
    ASSERT_EQ(1, m_queue.enqueued.size());
    const QList<RecommendedTrack>& tracks = m_queue.enqueued.first();
    ASSERT_EQ(6, tracks.size());
    EXPECT_EQ(realPathUids(), uidsOf(tracks));
    EXPECT_QSTRING_EQ(kFrom, tracks.first().entityUid);
    EXPECT_QSTRING_EQ(kTo, tracks.last().entityUid);
    EXPECT_QSTRING_EQ(QStringLiteral("Once You Say (People Theatre Remix)"), tracks[1].title);
    EXPECT_QSTRING_EQ(QStringLiteral("flac"), tracks[1].format);
    EXPECT_EQ(264626, tracks[1].durationMs.value_or(0));
    EXPECT_TRUE(m_queue.playlists.isEmpty());
    EXPECT_TRUE(finished.first().at(1).toString().isEmpty());
}

TEST_F(CratePathSourceTest, SavesPathAsPlaylist) {
    QSignalSpy finished(m_pPaths.get(), &CratePathSource::finished);
    m_pPaths->request(m_pClient.get(), pathRequest(PathRequest::Destination::Playlist));
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(200, fixture(QStringLiteral("real-path-preview.json")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !finished.isEmpty(); }, 2000));
    EXPECT_TRUE(m_queue.enqueued.isEmpty());
    ASSERT_EQ(1, m_queue.playlists.size());
    // Named after the path, as the server names it
    EXPECT_QSTRING_EQ(QStringLiteral("I Feel Loved (Danny Tenaglia's Labor Of Love Dub Edit) — "
                                     "Depeche Mode -> Lovely Love — Adult."),
            m_queue.playlists.first().first);
    EXPECT_EQ(realPathUids(), uidsOf(m_queue.playlists.first().second));
    // The name the playlist got
    EXPECT_QSTRING_EQ(m_queue.playlists.first().first + QStringLiteral(" (2)"),
            finished.first().at(1).toString());

    // A name of the DJ's own wins
    PathRequest named = pathRequest(PathRequest::Destination::Playlist);
    named.playlistName = QStringLiteral("Into the night");
    m_pPaths->request(m_pClient.get(), named);
    ASSERT_TRUE(waitForRequests(2));
    m_network.last().pReply->respond(200, fixture(QStringLiteral("real-path-preview.json")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return m_queue.playlists.size() == 2; }, 2000));
    EXPECT_QSTRING_EQ(QStringLiteral("Into the night"), m_queue.playlists.last().first);
}

TEST_F(CratePathSourceTest, ReportsAPathTheServerCannotWalk) {
    QSignalSpy failed(m_pPaths.get(), &CratePathSource::failed);
    PathRequest request = pathRequest(PathRequest::Destination::AutoDjQueue);
    // Out of the server's range: sent as its nearest
    request.steps = 400;
    m_pPaths->request(m_pClient.get(), request);
    ASSERT_TRUE(waitForRequests(1));
    EXPECT_EQ(PathRequest::kMaxSteps,
            QJsonDocument::fromJson(m_network.last().body)
                    .object()
                    .value(QStringLiteral("step_count"))
                    .toInt());
    m_network.last().pReply->respond(422,
            QByteArrayLiteral(
                    R"({"error":"Could not compute path — endpoints may lack bliss vectors"})"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !failed.isEmpty(); }, 2000));
    EXPECT_TRUE(m_queue.enqueued.isEmpty());
}

TEST_F(CratePathSourceTest, DialogAsksForStepsAndDestination) {
    crate::recs::CratePathDialog dialog(QStringLiteral("Depeche Mode - I Feel Loved"),
            QStringLiteral("Adult. - Lovely Love"));
    EXPECT_EQ(20, dialog.steps());
    EXPECT_EQ(PathRequest::kMinSteps, dialog.stepsBox()->minimum());
    EXPECT_EQ(PathRequest::kMaxSteps, dialog.stepsBox()->maximum());
    EXPECT_EQ(PathRequest::Destination::AutoDjQueue, dialog.destination());
    dialog.stepsBox()->setValue(12);
    dialog.toPlaylist()->click();
    EXPECT_EQ(12, dialog.steps());
    EXPECT_EQ(PathRequest::Destination::Playlist, dialog.destination());
}

// V3.PR3: a dialog of Decks' own, with the tracks and the destination as
// sections and its action named, instead of Qt's form and button box
TEST_F(CratePathSourceTest, DialogHasTheCrateDialogsSections) {
    crate::recs::CratePathDialog dialog(QStringLiteral("Depeche Mode - I Feel Loved"),
            QStringLiteral("Adult. - Lovely Love"));
    EXPECT_TRUE(dialog.findChildren<QDialogButtonBox*>().isEmpty());
    EXPECT_TRUE(dialog.findChildren<QGroupBox*>().isEmpty());
    QStringList sections;
    const QList<QFrame*> frames =
            dialog.findChildren<QFrame*>(QStringLiteral("CrateDialogSection"));
    for (const QFrame* pSection : frames) {
        sections.append(pSection->findChild<QLabel*>(QStringLiteral("MonoDialogSection"))->text());
    }
    EXPECT_EQ(QStringList({QStringLiteral("Tracks"), QStringLiteral("Destination")}), sections);
    EXPECT_EQ(frames.value(0), dialog.stepsBox()->parentWidget());
    EXPECT_EQ(frames.value(1), dialog.toPlaylist()->parentWidget());
    auto* pMake = dialog.findChild<QPushButton*>(QStringLiteral("crateConfirm"));
    ASSERT_NE(nullptr, pMake);
    EXPECT_QSTRING_EQ(QStringLiteral("Make the path"), pMake->text());
    EXPECT_QSTRING_EQ(QStringLiteral("primary"), pMake->property("crateButton").toString());
    pMake->click();
    EXPECT_EQ(QDialog::Accepted, dialog.result());
}

} // namespace
