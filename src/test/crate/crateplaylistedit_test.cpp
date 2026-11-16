#include "crate/writeback/crateplaylistedit.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <memory>

#include "crate/client/crateclient.h"
#include "test/crate/fakecratenetwork.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::cache::TrackKey;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeTokenSource;
using crate::writeback::CratePlaylistEdit;
using Row = CratePlaylistEdit::Row;
using Outcome = CratePlaylistEdit::Outcome;

const QString kServer = QStringLiteral("srv-1");
constexpr qint64 kPlaylist = 7;
const QString kPlaylistPath = QStringLiteral("/api/playlists/7");

/// One row of `GET /api/playlists/{id}`: its id and track (empty for one
/// without a local file).
struct ServerRow {
    int id;
    QString uid;
};

QByteArray playlistAnswer(const QList<ServerRow>& rows, bool canEdit = true) {
    QJsonArray tracks;
    int position = 0;
    for (const ServerRow& row : rows) {
        QJsonObject track{{QStringLiteral("id"), row.id},
                {QStringLiteral("playlist_id"), kPlaylist},
                {QStringLiteral("position"), ++position},
                {QStringLiteral("title"), QStringLiteral("Track %1").arg(row.id)}};
        track.insert(QStringLiteral("track_entity_uid"),
                row.uid.isEmpty() ? QJsonValue() : QJsonValue(row.uid));
        tracks.append(track);
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("id"), kPlaylist},
                                 {QStringLiteral("name"), QStringLiteral("Warm-up")},
                                 {QStringLiteral("can_edit"), canEdit},
                                 {QStringLiteral("tracks"), tracks}})
            .toJson(QJsonDocument::Compact);
}

QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
    return QJsonDocument::fromJson(sent.body).object();
}

QString verbOf(const FakeNetworkAccessManager::Sent& sent) {
    switch (sent.operation) {
    case QNetworkAccessManager::GetOperation:
        return QStringLiteral("GET");
    case QNetworkAccessManager::PostOperation:
        return QStringLiteral("POST");
    case QNetworkAccessManager::DeleteOperation:
        return QStringLiteral("DELETE");
    default:
        return QString::fromLatin1(
                sent.request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray());
    }
}

class CratePlaylistEditTest : public testing::Test {
  protected:
    void SetUp() override {
        ClientOptions options;
        options.deviceFingerprint = QStringLiteral("install-1");
        options.maxAttempts = 1;
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(options));
        m_pClient->setTokenSource(&m_tokens);

        CratePlaylistEdit::Options editOptions;
        editOptions.clientFor = [this](const QString&) {
            return m_pClient.get();
        };
        editOptions.readStored = [this](const QString& serverId,
                                         qint64 playlistId,
                                         const auto& done) {
            EXPECT_EQ(kServer, serverId);
            EXPECT_EQ(kPlaylist, playlistId);
            done(m_stored);
        };
        editOptions.resync = [this](const QString& serverId, qint64 playlistId, const auto& done) {
            EXPECT_EQ(kServer, serverId);
            m_resyncs.append(playlistId);
            if (playlistId == kPlaylist && m_afterResync) {
                m_stored = *m_afterResync;
            }
            done(true);
        };
        m_pEdit = std::make_unique<CratePlaylistEdit>(std::move(editOptions));
    }

    /// The last request, which must be `verb path`.
    FakeNetworkAccessManager::Sent expectLast(const QString& verb, const QString& path) {
        const auto sent = m_network.last();
        EXPECT_EQ(verb, verbOf(sent));
        EXPECT_EQ(path, sent.request.url().path());
        return sent;
    }

    void answerAll() {
        const QList<crate::test::FakeReply*> replies = m_network.openReplies();
        for (auto* pReply : std::as_const(replies)) {
            pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
        }
    }

    CratePlaylistEdit::Done keep(std::optional<Outcome>* pOutcome) {
        return [pOutcome](const Outcome& outcome) {
            *pOutcome = outcome;
        };
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<CratePlaylistEdit> m_pEdit;
    std::optional<QStringList> m_stored;
    /// What the stored copy becomes when the playlist is synced again.
    std::optional<QStringList> m_afterResync;
    QList<qint64> m_resyncs;
};

TEST_F(CratePlaylistEditTest, AddRemoveReorder) {
    // Row 14 is a track without a local file, which Decks does not list
    const QList<ServerRow> rows = {{11, QStringLiteral("a")},
            {12, QStringLiteral("b")},
            {14, QString()},
            {13, QStringLiteral("c")}};
    m_stored = QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};

    // Add, at the end
    std::optional<Outcome> added;
    m_pEdit->add(kServer, kPlaylist, {QStringLiteral("d"), QStringLiteral("e")}, keep(&added));
    expectLast(QStringLiteral("GET"), kPlaylistPath).pReply->respond(200, playlistAnswer(rows));
    const auto add = expectLast(QStringLiteral("POST"), kPlaylistPath + QStringLiteral("/tracks"));
    EXPECT_EQ((QJsonObject{{QStringLiteral("tracks"),
                      QJsonArray{QJsonObject{{QStringLiteral("entity_uid"), QStringLiteral("d")}},
                              QJsonObject{{QStringLiteral("entity_uid"), QStringLiteral("e")}}}}}),
            bodyOf(add));
    EXPECT_FALSE(added.has_value());
    add.pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "added": 2})"));
    ASSERT_TRUE(added.has_value());
    EXPECT_TRUE(added->ok);
    EXPECT_EQ(kPlaylist, added->playlistId);
    EXPECT_EQ(QList<qint64>{kPlaylist}, m_resyncs);

    // Remove b and c: by their positions on the server, the last first
    std::optional<Outcome> removed;
    m_pEdit->remove(kServer,
            kPlaylist,
            {Row{1, QStringLiteral("b")}, Row{2, QStringLiteral("c")}},
            keep(&removed));
    expectLast(QStringLiteral("GET"), kPlaylistPath).pReply->respond(200, playlistAnswer(rows));
    expectLast(QStringLiteral("DELETE"), kPlaylistPath + QStringLiteral("/tracks/4"))
            .pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    expectLast(QStringLiteral("DELETE"), kPlaylistPath + QStringLiteral("/tracks/2"))
            .pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(removed->ok);

    // Move c before a: every row id goes, row 14 keeps its place
    std::optional<Outcome> moved;
    m_pEdit->move(kServer,
            kPlaylist,
            {Row{2, QStringLiteral("c")}},
            Row{0, QStringLiteral("a")},
            keep(&moved));
    expectLast(QStringLiteral("GET"), kPlaylistPath).pReply->respond(200, playlistAnswer(rows));
    const auto reorder =
            expectLast(QStringLiteral("POST"), kPlaylistPath + QStringLiteral("/reorder"));
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_ids"), QJsonArray{13, 11, 14, 12}}}),
            bodyOf(reorder));
    reorder.pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    ASSERT_TRUE(moved.has_value());
    EXPECT_TRUE(moved->ok);

    // To the end
    m_pEdit->move(kServer, kPlaylist, {Row{0, QStringLiteral("a")}}, std::nullopt);
    expectLast(QStringLiteral("GET"), kPlaylistPath).pReply->respond(200, playlistAnswer(rows));
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_ids"), QJsonArray{12, 13, 14, 11}}}),
            bodyOf(expectLast(QStringLiteral("POST"), kPlaylistPath + QStringLiteral("/reorder"))));
    answerAll();

    // A whole new order, as a drag in the track table leaves it
    std::optional<Outcome> reordered;
    m_pEdit->reorder(kServer,
            kPlaylist,
            {QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("a")},
            keep(&reordered));
    expectLast(QStringLiteral("GET"), kPlaylistPath).pReply->respond(200, playlistAnswer(rows));
    const auto order = expectLast(
            QStringLiteral("POST"), kPlaylistPath + QStringLiteral("/reorder"));
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_ids"), QJsonArray{13, 12, 14, 11}}}),
            bodyOf(order));
    order.pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    ASSERT_TRUE(reordered.has_value());
    EXPECT_TRUE(reordered->ok);
}

TEST_F(CratePlaylistEditTest, RejectsNonCrateTracks) {
    const auto trackOfFile = [](const QString& location) -> std::optional<TrackKey> {
        if (location.startsWith(QStringLiteral("/cache/srv-1/"))) {
            return TrackKey{kServer, location.section(QLatin1Char('/'), 3, 3)};
        }
        if (location.startsWith(QStringLiteral("/cache/srv-2/"))) {
            return TrackKey{QStringLiteral("srv-2"), location.section(QLatin1Char('/'), 3, 3)};
        }
        return std::nullopt;
    };
    const CratePlaylistEdit::CrateTracks tracks = CratePlaylistEdit::crateTracksOf(kServer,
            {QStringLiteral("/cache/srv-1/a"),
                    QStringLiteral("/music/local.flac"),
                    QStringLiteral("/cache/srv-2/x"),
                    QStringLiteral("/cache/srv-1/b")},
            trackOfFile);
    EXPECT_EQ((QStringList{QStringLiteral("a"), QStringLiteral("b")}), tracks.entityUids);
    EXPECT_EQ(2, tracks.rejected);

    // Nothing of Crate: nothing is sent
    QSignalSpy warnings(m_pEdit.get(), &CratePlaylistEdit::warning);
    std::optional<Outcome> none;
    m_pEdit->exportTracks(kServer, QStringLiteral("Local set"), {}, 3, keep(&none));
    std::optional<Outcome> noneAdded;
    m_pEdit->add(kServer, kPlaylist, {}, keep(&noneAdded));
    EXPECT_TRUE(m_network.sent.isEmpty());
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->ok);
    EXPECT_EQ(3, none->rejected);
    ASSERT_TRUE(noneAdded.has_value());
    EXPECT_FALSE(noneAdded->ok);
    EXPECT_EQ(2, warnings.size());

    // The Crate tracks of a Decks playlist become a Crate playlist
    std::optional<Outcome> exported;
    m_pEdit->exportTracks(kServer,
            QStringLiteral("Friday set"),
            tracks.entityUids,
            tracks.rejected,
            keep(&exported));
    const auto create = expectLast(QStringLiteral("POST"), QStringLiteral("/api/playlists"));
    EXPECT_EQ(QStringLiteral("Friday set"),
            bodyOf(create).value(QStringLiteral("name")).toString());
    create.pReply->respond(200, QByteArrayLiteral(R"({"id": 99})"));
    const auto add = expectLast(QStringLiteral("POST"), QStringLiteral("/api/playlists/99/tracks"));
    EXPECT_EQ(2, bodyOf(add).value(QStringLiteral("tracks")).toArray().size());
    add.pReply->respond(200, QByteArrayLiteral(R"({"ok": true, "added": 2})"));
    ASSERT_TRUE(exported.has_value());
    EXPECT_TRUE(exported->ok);
    EXPECT_EQ(99, exported->playlistId);
    EXPECT_EQ(2, exported->rejected);
    // The tree shows the new playlist
    EXPECT_TRUE(m_resyncs.contains(0));
}

TEST_F(CratePlaylistEditTest, RefetchesOnConflict) {
    QSignalSpy warnings(m_pEdit.get(), &CratePlaylistEdit::warning);
    m_stored = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    // Someone added x on the server in the meantime
    const QList<ServerRow> changed = {{11, QStringLiteral("a")},
            {15, QStringLiteral("x")},
            {12, QStringLiteral("b")}};
    m_afterResync = QStringList{QStringLiteral("a"), QStringLiteral("x"), QStringLiteral("b")};

    std::optional<Outcome> removed;
    m_pEdit->remove(kServer, kPlaylist, {Row{1, QStringLiteral("b")}}, keep(&removed));
    expectLast(QStringLiteral("GET"), kPlaylistPath).pReply->respond(200, playlistAnswer(changed));
    // Synced again, read again, and b is found at its new place
    EXPECT_EQ(QList<qint64>{kPlaylist}, m_resyncs);
    expectLast(QStringLiteral("GET"), kPlaylistPath).pReply->respond(200, playlistAnswer(changed));
    expectLast(QStringLiteral("DELETE"), kPlaylistPath + QStringLiteral("/tracks/3"))
            .pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(removed->ok);
    EXPECT_TRUE(warnings.isEmpty());

    // A new order made on the old copy: its tracks take it among their places
    m_stored = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    std::optional<Outcome> reordered;
    m_pEdit->reorder(kServer,
            kPlaylist,
            {QStringLiteral("b"), QStringLiteral("a")},
            keep(&reordered));
    expectLast(QStringLiteral("GET"), kPlaylistPath).pReply->respond(200, playlistAnswer(changed));
    expectLast(QStringLiteral("GET"), kPlaylistPath).pReply->respond(200, playlistAnswer(changed));
    const auto order = expectLast(
            QStringLiteral("POST"), kPlaylistPath + QStringLiteral("/reorder"));
    EXPECT_EQ((QJsonObject{{QStringLiteral("track_ids"), QJsonArray{12, 15, 11}}}),
            bodyOf(order));
    order.pReply->respond(200, QByteArrayLiteral(R"({"ok": true})"));
    ASSERT_TRUE(reordered.has_value());
    EXPECT_TRUE(reordered->ok);

    // It keeps changing: one retry, then the user is told and nothing is written
    m_stored = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    m_afterResync = QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("y")};
    m_resyncs.clear();
    const int sentBefore = static_cast<int>(m_network.sent.size());
    std::optional<Outcome> moved;
    m_pEdit->move(kServer,
            kPlaylist,
            {Row{1, QStringLiteral("b")}},
            Row{0, QStringLiteral("a")},
            keep(&moved));
    expectLast(QStringLiteral("GET"), kPlaylistPath)
            .pReply->respond(200,
                    playlistAnswer({{11, QStringLiteral("a")},
                            {12, QStringLiteral("b")},
                            {16, QStringLiteral("y")}}));
    expectLast(QStringLiteral("GET"), kPlaylistPath)
            .pReply->respond(200,
                    playlistAnswer({{11, QStringLiteral("a")},
                            {12, QStringLiteral("b")},
                            {16, QStringLiteral("y")},
                            {17, QStringLiteral("z")}}));
    EXPECT_EQ(sentBefore + 2, m_network.sent.size());
    ASSERT_TRUE(moved.has_value());
    EXPECT_FALSE(moved->ok);
    EXPECT_TRUE(moved->conflict);
    // Synced before the retry, and after it so that Decks shows the server's
    EXPECT_EQ((QList<qint64>{kPlaylist, kPlaylist}), m_resyncs);
    ASSERT_EQ(1, warnings.size());

    // A server error on the write warns too
    m_stored = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    m_afterResync.reset();
    std::optional<Outcome> failed;
    m_pEdit->move(kServer,
            kPlaylist,
            {Row{1, QStringLiteral("b")}},
            Row{0, QStringLiteral("a")},
            keep(&failed));
    expectLast(QStringLiteral("GET"), kPlaylistPath)
            .pReply->respond(200,
                    playlistAnswer({{11, QStringLiteral("a")}, {12, QStringLiteral("b")}}));
    expectLast(QStringLiteral("POST"), kPlaylistPath + QStringLiteral("/reorder"))
            .pReply->respond(500, QByteArrayLiteral("Internal Server Error"));
    ASSERT_TRUE(failed.has_value());
    EXPECT_FALSE(failed->ok);
    EXPECT_FALSE(failed->conflict);
    EXPECT_EQ(2, warnings.size());

    // And so does a playlist the user cannot edit
    std::optional<Outcome> refused;
    m_pEdit->add(kServer, kPlaylist, {QStringLiteral("d")}, keep(&refused));
    expectLast(QStringLiteral("GET"), kPlaylistPath)
            .pReply->respond(200,
                    playlistAnswer({{11, QStringLiteral("a")}, {12, QStringLiteral("b")}}, false));
    ASSERT_TRUE(refused.has_value());
    EXPECT_FALSE(refused->ok);
    EXPECT_EQ(3, warnings.size());
    EXPECT_EQ(QStringLiteral("GET"), verbOf(m_network.last()));
}

} // namespace
