// F7 and F8, live: Decks' write-back (plan F7) and its cue and correction
// sync (plan F8) against a Crate dev stack that serves S6 and S7, with the
// real client classes: CrateServers signed in through AddServerFlow, its
// Outbox, CrateLikes, CrateCueSync and CrateCorrections, and a
// HistoryReporter, CrateRatings and CratePlaylistEdit (with the real
// LibrarySync and library store behind it) wired as the application wires
// them. Each effect is read back from the server's API.
//
// Skipped unless CRATE_DECKS_LIVE_URL is set, so CI never runs it:
//   CRATE_DECKS_LIVE_URL         a dev stack (loopback or private address
//                                only), e.g. http://127.0.0.1:8585
//   CRATE_DECKS_LIVE_EMAIL,
//   CRATE_DECKS_LIVE_PASSWORD    an account of that dev stack; never
//                                production
//   CRATE_DECKS_LIVE_COMPOSE     optional: the dev stack's compose file. With
//                                it the tests also read, never write, what the
//                                API does not show: the play event's columns
//                                (psql) and the now playing state (Redis)
//
// The tracks are taken from the first playlist of the server's tree with at
// least kTracksNeeded playable tracks, each test its own. Every test removes
// what it created and puts back what it changed: the temporary playlist is
// deleted, likes and ratings go back to what they were, and the cue set and
// the correction are emptied (S7 keeps their rows and revisions). A play
// event cannot be removed through the API: the history test leaves one.

#include <gtest/gtest.h>

#include <QDir>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <iostream>
#include <memory>
#include <optional>

#include "crate/client/crateclient.h"
#include "crate/crateaddserverflow.h"
#include "crate/crateservers.h"
#include "crate/library/cratelibrarystore.h"
#include "crate/library/cratelibrarysync.h"
#include "crate/session/cratesecretstore.h"
#include "crate/session/cratesession.h"
#include "crate/writeback/cratecorrections.h"
#include "crate/writeback/cratecuesync.h"
#include "crate/writeback/cratelikes.h"
#include "crate/writeback/crateplaylistedit.h"
#include "crate/writeback/crateratings.h"
#include "crate/writeback/cuemapping.h"
#include "crate/writeback/historyreporter.h"
#include "crate/writeback/outbox.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

using namespace std::chrono_literals;
using crate::cache::TrackKey;
using crate::test::FakeScheduler;
using crate::writeback::CrateCorrections;
using crate::writeback::CrateCueSync;
using crate::writeback::CratePlaylistEdit;
using crate::writeback::CrateRatings;
using crate::writeback::HistoryReporter;
using crate::writeback::Outbox;
using crate::writeback::SyncedCues;
namespace outboxkind = crate::writeback::outboxkind;

constexpr int kNetworkWaitMs = 60000;
/// The tracks the tests take, each its own.
constexpr int kTracksNeeded = 16;
constexpr int kSampleRate = 44100;
const QString kOtherMachine = QStringLiteral("decks-live-other-machine");

void report(const std::string& line) {
    std::cout << "[ LIVE     ] " << line << std::endl;
}

std::string text(const QJsonValue& value) {
    if (value.isObject()) {
        return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact).toStdString();
    }
    if (value.isArray()) {
        return QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact).toStdString();
    }
    return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).toStdString();
}

bool isDevStack(const QUrl& url) {
    const QString host = url.host();
    if (host == QLatin1String("localhost")) {
        return true;
    }
    const QHostAddress address(host);
    if (address.isNull()) {
        return false;
    }
    if (address.isLoopback() || address.isLinkLocal()) {
        return true;
    }
    for (const char* subnet : {"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "fc00::/7"}) {
        if (address.isInSubnet(QHostAddress::parseSubnet(QString::fromLatin1(subnet)))) {
            return true;
        }
    }
    return false;
}

double framesOf(double ms) {
    return ms * kSampleRate / 1000.0;
}

/// An answer of the server to the test's own requests.
struct Answer {
    bool ok = false;
    int status = 0;
    QJsonDocument json;
    QString error;

    QJsonObject object() const {
        return json.object();
    }
};

/// The entries the outbox delivered (or refused) since the spy began.
struct OutboxWatch {
    explicit OutboxWatch(Outbox* pOutbox)
            : delivered(pOutbox, &Outbox::delivered),
              conflicted(pOutbox, &Outbox::conflicted),
              dropped(pOutbox, &Outbox::dropped) {
    }

    /// Waits for the next delivered entry of `kind` after the first
    /// `seen`; its payload and answer.
    std::optional<std::pair<QJsonObject, QJsonObject>> waitDelivered(
            const QString& kind, int seen = 0) {
        std::optional<std::pair<QJsonObject, QJsonObject>> found;
        // A timeout leaves `found` empty, which the caller asserts
        static_cast<void>(QTest::qWaitFor(
                [&] {
                    int matching = 0;
                    for (const QList<QVariant>& args : std::as_const(delivered)) {
                        if (args.at(1).toString() != kind) {
                            continue;
                        }
                        if (matching++ == seen) {
                            found = std::make_pair(args.at(2).toJsonObject(),
                                    args.at(3).value<QJsonDocument>().object());
                            return true;
                        }
                    }
                    return !dropped.isEmpty();
                },
                kNetworkWaitMs));
        for (const QList<QVariant>& args : std::as_const(dropped)) {
            ADD_FAILURE() << "The outbox dropped a " << args.at(1).toString().toStdString()
                          << ": " << args.at(2).toString().toStdString();
        }
        return found;
    }

    QSignalSpy delivered;
    QSignalSpy conflicted;
    QSignalSpy dropped;
};

/// Another computer of the same user: its own outbox, which only sends while
/// `online`, and its own syncs, whose timers the test runs.
struct OtherMachine {
    FakeScheduler scheduler;
    bool online = true;
    std::unique_ptr<Outbox> pOutbox;
    std::unique_ptr<CrateCueSync> pCues;
    std::unique_ptr<CrateCorrections> pCorrections;

    ~OtherMachine() {
        for (crate::writeback::TrackDataSync* pSync :
                {static_cast<crate::writeback::TrackDataSync*>(pCues.get()),
                        static_cast<crate::writeback::TrackDataSync*>(pCorrections.get())}) {
            if (pSync) {
                pSync->waitForStorage();
            }
        }
        pCues.reset();
        pCorrections.reset();
        if (pOutbox) {
            pOutbox->waitForStorage();
        }
        pOutbox.reset();
    }
};

class CrateWritebackLive : public MixxxDbTest {
  public:
    CrateWritebackLive()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        const QString url = qEnvironmentVariable("CRATE_DECKS_LIVE_URL");
        if (url.isEmpty()) {
            GTEST_SKIP() << "Live check against a Crate dev stack (F7 and F8): set "
                            "CRATE_DECKS_LIVE_URL, CRATE_DECKS_LIVE_EMAIL and "
                            "CRATE_DECKS_LIVE_PASSWORD";
        }
        ASSERT_TRUE(isDevStack(QUrl(url))) << "Only a dev stack: " << url.toStdString();
        const QString email = qEnvironmentVariable("CRATE_DECKS_LIVE_EMAIL");
        const QString password = qEnvironmentVariable("CRATE_DECKS_LIVE_PASSWORD");
        ASSERT_FALSE(email.isEmpty() || password.isEmpty())
                << "Set CRATE_DECKS_LIVE_EMAIL and CRATE_DECKS_LIVE_PASSWORD";
        m_composeFile = qEnvironmentVariable("CRATE_DECKS_LIVE_COMPOSE");
        m_email = email;

        crate::CrateServers::Dependencies dependencies;
        dependencies.pConfig = config();
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.pSecretStore = &m_secrets;
        // Read when the reply finishes: the client deletes it right after
        QObject::connect(&m_network,
                &QNetworkAccessManager::finished,
                &m_network,
                [this](QNetworkReply* pReply) {
                    m_requests.append({pReply->url().path(),
                            pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()});
                });
        m_pServers = std::make_unique<crate::CrateServers>(dependencies);
        m_pServers->start();

        using Step = crate::AddServerFlow::Step;
        crate::AddServerFlow flow(m_pServers.get());
        flow.setAddress(url);
        if (flow.step() == Step::ConfirmLocalNetwork) {
            flow.confirmLocalNetwork();
        }
        ASSERT_TRUE(QTest::qWaitFor(
                [&flow] {
                    return flow.step() == Step::SignIn || flow.step() == Step::EnterAddress;
                },
                kNetworkWaitMs));
        ASSERT_EQ(Step::SignIn, flow.step()) << flow.error().toStdString();
        flow.loginWithPassword(email, password);
        ASSERT_TRUE(QTest::qWaitFor(
                [&flow] {
                    return flow.step() != Step::SigningIn;
                },
                kNetworkWaitMs));
        ASSERT_EQ(Step::Done, flow.step()) << flow.error().toStdString();
        ASSERT_EQ(1, m_pServers->sessions().size());
        m_serverId = m_pServers->sessions().constFirst()->profile().id;
        m_pClient = m_pServers->client(m_serverId);
        ASSERT_NE(nullptr, m_pClient);
        ASSERT_TRUE(QTest::qWaitFor(
                [this] {
                    return m_pServers->outbox()->isLoaded() &&
                            m_pServers->cueSync()->isLoaded() &&
                            m_pServers->corrections()->isLoaded();
                },
                kNetworkWaitMs));
        // S7 is there
        ASSERT_TRUE(m_pServers->serverHas(m_serverId, crate::DecksFeature::Cues));
        m_fingerprint = crate::deviceFingerprint(config());
        ASSERT_NO_FATAL_FAILURE(pickTracks());
    }

    void TearDown() override {
        if (m_pServers) {
            // What the tests sent has left
            EXPECT_TRUE(QTest::qWaitFor(
                    [this] {
                        return m_pServers->outbox()->pendingCount() == 0;
                    },
                    kNetworkWaitMs));
            // The test's session is revoked on the server
            if (!m_serverId.isEmpty()) {
                m_pServers->removeServer(m_serverId);
                QTest::qWait(1500);
            }
        }
        m_pServers.reset();
    }

  protected:
    Outbox* outbox() const {
        return m_pServers->outbox();
    }

    Answer call(QNetworkAccessManager::Operation verb,
            const QString& path,
            const QJsonObject& body = QJsonObject(),
            const QUrlQuery& query = QUrlQuery()) {
        crate::RequestOptions options;
        options.query = query;
        crate::Reply* pReply = nullptr;
        switch (verb) {
        case QNetworkAccessManager::GetOperation:
            pReply = m_pClient->get(path, options);
            break;
        case QNetworkAccessManager::PostOperation:
            pReply = m_pClient->post(path, QJsonDocument(body), options);
            break;
        case QNetworkAccessManager::PutOperation:
            pReply = m_pClient->put(path, QJsonDocument(body), options);
            break;
        case QNetworkAccessManager::DeleteOperation:
            pReply = body.isEmpty() ? m_pClient->del(path, options)
                                    : m_pClient->del(path, QJsonDocument(body), options);
            break;
        default:
            ADD_FAILURE() << "Unsupported verb";
            return Answer();
        }
        // Shared with the slot, which may outlive this call on a timeout
        auto pAnswer = std::make_shared<std::optional<Answer>>();
        QObject::connect(pReply,
                &crate::Reply::finished,
                [pAnswer, pReply](const crate::JsonResult& result) {
                    Answer answer;
                    answer.ok = result.ok();
                    answer.status = pReply->httpStatus();
                    if (result.ok()) {
                        answer.json = result.value();
                    } else {
                        answer.error = result.error().message;
                        answer.json = QJsonDocument::fromJson(pReply->body());
                    }
                    *pAnswer = answer;
                });
        EXPECT_TRUE(QTest::qWaitFor(
                [pAnswer] {
                    return pAnswer->has_value();
                },
                kNetworkWaitMs))
                << path.toStdString();
        return pAnswer->value_or(Answer());
    }

    Answer get(const QString& path, const QUrlQuery& query = QUrlQuery()) {
        return call(QNetworkAccessManager::GetOperation, path, QJsonObject(), query);
    }

    /// A read-only look at what the API does not show. Empty without
    /// CRATE_DECKS_LIVE_COMPOSE.
    std::optional<QString> inspect(const QStringList& serviceCommand) {
        if (m_composeFile.isEmpty()) {
            return std::nullopt;
        }
        QProcess process;
        QStringList arguments{QStringLiteral("compose"),
                QStringLiteral("-f"),
                m_composeFile,
                QStringLiteral("exec"),
                QStringLiteral("-T")};
        arguments.append(serviceCommand);
        process.start(QStringLiteral("docker"), arguments);
        if (!process.waitForFinished(30000) || process.exitCode() != 0) {
            ADD_FAILURE() << "docker compose exec failed: "
                          << process.readAllStandardError().toStdString();
            return std::nullopt;
        }
        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }

    /// One SELECT on the dev stack's database, its rows as `a|b` lines.
    std::optional<QString> selectRows(const QString& sql) {
        EXPECT_TRUE(sql.startsWith(QLatin1String("SELECT ")));
        return inspect({QStringLiteral("postgres"),
                QStringLiteral("psql"),
                QStringLiteral("-U"),
                QStringLiteral("crate"),
                QStringLiteral("-d"),
                QStringLiteral("crate"),
                QStringLiteral("-At"),
                QStringLiteral("-c"),
                sql});
    }

    /// GET of a key of the dev stack's cache.
    std::optional<QString> redisGet(const QString& key) {
        return inspect({QStringLiteral("redis"),
                QStringLiteral("sh"),
                QStringLiteral("-c"),
                QStringLiteral("redis-cli -a \"$REDIS_PASSWORD\" --no-auth-warning GET '%1'")
                        .arg(key)});
    }

    void pickTracks() {
        const Answer tree = get(QStringLiteral("/api/decks/containers"));
        ASSERT_TRUE(tree.ok) << tree.error.toStdString();
        const QJsonArray containers = tree.object().value(QStringLiteral("containers")).toArray();
        QStringList uids;
        for (const QJsonValue& value : containers) {
            const QJsonObject container = value.toObject();
            if (container.value(QStringLiteral("kind")).toString() != QLatin1String("playlist") ||
                    container.value(QStringLiteral("track_count")).toInt() < kTracksNeeded) {
                continue;
            }
            const Answer tracks = get(QStringLiteral("/api/decks/containers/playlist/%1/tracks")
                                              .arg(container.value(QStringLiteral("id"))
                                                              .toVariant()
                                                              .toString()),
                    QUrlQuery(QStringLiteral("limit=100")));
            ASSERT_TRUE(tracks.ok) << tracks.error.toStdString();
            uids.clear();
            const QJsonArray rows = tracks.object().value(QStringLiteral("tracks")).toArray();
            for (const QJsonValue& track : rows) {
                const QJsonObject object = track.toObject();
                const QString uid = object.value(QStringLiteral("entity_uid")).toString();
                if (object.value(QStringLiteral("playable")).toBool(true) &&
                        !uids.contains(uid)) {
                    uids.append(uid);
                    m_titles.insert(uid, object.value(QStringLiteral("title")).toString());
                }
            }
            if (uids.size() >= kTracksNeeded) {
                m_uids = uids;
                report("tracks from the playlist \"" +
                        container.value(QStringLiteral("name")).toString().toStdString() + "\"");
                return;
            }
        }
        FAIL() << "No playlist with " << kTracksNeeded << " playable tracks on the server";
    }

    QString location(const QString& uid) const {
        return QStringLiteral("/crate-decks-live/%1/%2.flac").arg(m_serverId, uid);
    }

    std::function<std::optional<TrackKey>(const QString&)> trackOfFile() const {
        const QString prefix = QStringLiteral("/crate-decks-live/%1/").arg(m_serverId);
        const QString serverId = m_serverId;
        return [prefix, serverId](const QString& location) -> std::optional<TrackKey> {
            if (!location.startsWith(prefix)) {
                return std::nullopt;
            }
            return TrackKey{serverId, location.mid(prefix.size()).section(QLatin1Char('.'), 0, 0)};
        };
    }

    /// A Crate track as the decks have it once its file is open.
    TrackPointer crateTrack(const QString& uid) const {
        TrackPointer pTrack = Track::newTemporary(location(uid));
        pTrack->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
                mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::Bitrate(320),
                mixxx::Duration::fromSeconds(240));
        pTrack->setTitle(m_titles.value(uid));
        pTrack->setArtist(QStringLiteral("Decks live check"));
        return pTrack;
    }

    /// The HTTP statuses of the requests to `path` answered so far.
    QList<int> answered(const QString& path) const {
        QList<int> statuses;
        for (const auto& [answeredPath, status] : m_requests) {
            if (answeredPath == path) {
                statuses.append(status);
            }
        }
        return statuses;
    }

    /// Waits until more than `seen` requests to `path` were answered.
    bool waitForRequest(const QString& path, int seen = 0) {
        return QTest::qWaitFor(
                [&] {
                    return answered(path).size() > seen;
                },
                kNetworkWaitMs);
    }

    std::unique_ptr<OtherMachine> otherMachine() {
        auto pMachine = std::make_unique<OtherMachine>();
        OtherMachine* const pRaw = pMachine.get();
        Outbox::Options outbox;
        outbox.pDbConnectionPool = dbConnectionPooler();
        outbox.clientFor = [this, pRaw](const QString&) -> crate::Client* {
            return pRaw->online ? m_pClient : nullptr;
        };
        pMachine->pOutbox = std::make_unique<Outbox>(std::move(outbox));
        pMachine->pOutbox->start();
        crate::writeback::TrackDataSync::Options sync;
        sync.pOutbox = pMachine->pOutbox.get();
        sync.pDbConnectionPool = dbConnectionPooler();
        sync.clientFor = [this](const QString&) {
            return m_pClient;
        };
        sync.enabledFor = [](const QString&) {
            return true;
        };
        sync.clientId = kOtherMachine;
        sync.schedule = pMachine->scheduler.function();
        pMachine->pCues = std::make_unique<CrateCueSync>(sync);
        pMachine->pCorrections = std::make_unique<CrateCorrections>(std::move(sync));
        pMachine->pCues->start();
        pMachine->pCorrections->start();
        EXPECT_TRUE(QTest::qWaitFor(
                [pRaw] {
                    return pRaw->pOutbox->isLoaded() && pRaw->pCues->isLoaded() &&
                            pRaw->pCorrections->isLoaded();
                },
                kNetworkWaitMs));
        return pMachine;
    }

    /// The server's cue set or correction of a track: the entry, or none
    /// when `missing`; and the answer's server_time.
    std::pair<std::optional<QJsonObject>, QString> queryUserData(
            const QString& kind, const QString& uid) {
        const Answer answer = call(QNetworkAccessManager::PostOperation,
                QStringLiteral("/api/decks/%1/query").arg(kind),
                QJsonObject{{QStringLiteral("entity_uids"), QJsonArray{uid}}});
        EXPECT_TRUE(answer.ok) << answer.error.toStdString();
        const QString serverTime = answer.object().value(QStringLiteral("server_time")).toString();
        const QJsonObject entries =
                answer.object()
                        .value(kind == QLatin1String("cues") ? QStringLiteral("cue_sets")
                                                             : QStringLiteral("corrections"))
                        .toObject();
        if (!entries.contains(uid.toLower())) {
            EXPECT_TRUE(answer.object()
                            .value(QStringLiteral("missing"))
                            .toArray()
                            .contains(QJsonValue(uid)));
            return {std::nullopt, serverTime};
        }
        return {entries.value(uid.toLower()).toObject(), serverTime};
    }

    QSet<QString> likedUids() {
        const Answer likes = get(QStringLiteral("/api/me/likes"),
                QUrlQuery(QStringLiteral("limit=1000")));
        EXPECT_TRUE(likes.ok) << likes.error.toStdString();
        QSet<QString> uids;
        const QJsonArray rows = likes.json.array();
        for (const QJsonValue& like : rows) {
            uids.insert(like.toObject()
                            .value(QStringLiteral("track_entity_uid"))
                            .toString()
                            .toLower());
        }
        return uids;
    }

    /// The tracks of a playlist on the server, in order.
    QStringList playlistOrder(qint64 playlistId) {
        const Answer playlist = get(QStringLiteral("/api/playlists/%1").arg(playlistId));
        EXPECT_TRUE(playlist.ok) << playlist.error.toStdString();
        QList<std::pair<int, QString>> rows;
        const QJsonArray tracks = playlist.object().value(QStringLiteral("tracks")).toArray();
        for (const QJsonValue& value : tracks) {
            const QJsonObject track = value.toObject();
            rows.append({track.value(QStringLiteral("position")).toInt(),
                    track.value(QStringLiteral("track_entity_uid")).toString().toLower()});
        }
        std::stable_sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        QStringList uids;
        for (const auto& row : std::as_const(rows)) {
            uids.append(row.second);
        }
        return uids;
    }

    QNetworkAccessManager m_network;
    crate::SessionOnlySecretStore m_secrets;
    std::unique_ptr<crate::CrateServers> m_pServers;
    QString m_serverId;
    QString m_email;
    QString m_fingerprint;
    QString m_composeFile;
    crate::Client* m_pClient = nullptr;
    /// Playable tracks of the server, in a playlist's order.
    QStringList m_uids;
    QHash<QString, QString> m_titles;
    /// Every request answered: its path and HTTP status.
    QList<std::pair<QString, int>> m_requests;
};

// 1. A play event of the HistoryReporter goes through the outbox, lands in
// the user's history labelled as Decks, with its client_event_id, and a retry
// with the same id does not count it twice.
TEST_F(CrateWritebackLive, PlayEventLandsInTheHistoryAsDecks) {
    const QString played = m_uids.at(0);
    const QString next = m_uids.at(1);
    // The reporter's clock runs ahead by hand: 45 s on air take no 45 s
    QDateTime now = QDateTime::currentDateTimeUtc().addSecs(-120);
    HistoryReporter::Options options;
    options.pOutbox = outbox();
    options.trackOfFile = trackOfFile();
    options.clientFor = [this](const QString& serverId) {
        return m_pServers->client(serverId);
    };
    options.now = [&now] {
        return now;
    };
    options.mixSessionId = QStringLiteral("decks-live-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto pReporter = std::make_unique<HistoryReporter>(std::move(options));
    OutboxWatch watch(outbox());

    const TrackPointer pPlayed = crateTrack(played);
    const TrackPointer pNext = crateTrack(next);
    pReporter->setPlayingDeck(0);
    pReporter->setPlayingTrack(pPlayed);
    now = now.addSecs(45);
    pReporter->setPlayingTrack(pNext);
    const auto first = watch.waitDelivered(outboxkind::kPlayEvent);
    pReporter->setPlayingTrack(TrackPointer());
    pReporter.reset();
    ASSERT_TRUE(first.has_value());
    const auto& [payload, answer] = *first;
    const qint64 eventId = answer.value(QStringLiteral("id")).toInteger();
    const QString clientEventId = payload.value(QStringLiteral("client_event_id")).toString();
    report("play event " + std::to_string(eventId) + " for " + played.toStdString() +
            ", client_event_id " + clientEventId.toStdString() + ", payload " + text(payload));
    EXPECT_GT(eventId, 0);
    EXPECT_FALSE(QUuid(clientEventId).isNull());
    EXPECT_DOUBLE_EQ(45.0, payload.value(QStringLiteral("played_seconds")).toDouble());

    // The user's history has it on top
    const Answer history = get(QStringLiteral("/api/me/history"),
            QUrlQuery(QStringLiteral("limit=50")));
    ASSERT_TRUE(history.ok) << history.error.toStdString();
    ASSERT_FALSE(history.json.array().isEmpty());
    const QJsonObject top = history.json.array().first().toObject();
    EXPECT_EQ(played.toLower(), top.value(QStringLiteral("track_entity_uid")).toString());
    int entries = 0;
    const QJsonArray historyRows = history.json.array();
    for (const QJsonValue& entry : historyRows) {
        entries += entry.toObject().value(QStringLiteral("track_entity_uid")).toString() ==
                        played.toLower()
                ? 1
                : 0;
    }
    report("history top: " + text(top));

    // The same event again, as a retry after a lost answer
    outbox()->enqueue(m_serverId, outboxkind::kPlayEvent, payload);
    const auto retry = watch.waitDelivered(outboxkind::kPlayEvent, 1);
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(eventId, retry->second.value(QStringLiteral("id")).toInteger())
            << "the retry got another event";
    const Answer again = get(QStringLiteral("/api/me/history"),
            QUrlQuery(QStringLiteral("limit=50")));
    int entriesAgain = 0;
    const QJsonArray againRows = again.json.array();
    for (const QJsonValue& entry : againRows) {
        entriesAgain += entry.toObject().value(QStringLiteral("track_entity_uid")).toString() ==
                        played.toLower()
                ? 1
                : 0;
    }
    EXPECT_EQ(entries, entriesAgain) << "the retry shows twice in the history";

    // Its columns, which the history does not show
    const auto columns = selectRows(
            QStringLiteral("SELECT client_event_id || '|' || device_type || '|' || "
                           "app_platform || '|' || play_source_type || '|' || "
                           "played_seconds || '|' || track_entity_uid FROM user_play_events "
                           "WHERE id = %1")
                    .arg(eventId));
    if (columns) {
        report("user_play_events row: " + columns->toStdString());
        EXPECT_EQ(QStringLiteral("%1|decks|decks|decks|45|%2").arg(clientEventId, played.toLower()),
                *columns);
        const auto count = selectRows(
                QStringLiteral("SELECT count(*) FROM user_play_events WHERE client_event_id = '%1'")
                        .arg(clientEventId));
        EXPECT_EQ(QStringLiteral("1"), count.value_or(QString()));
    } else {
        report("CRATE_DECKS_LIVE_COMPOSE not set: the play event's columns are not checked");
    }
}

// 2. Now playing carries the deck and the mix session; {"playing": false}
// clears it.
TEST_F(CrateWritebackLive, NowPlayingCarriesTheDeckAndTheMixSession) {
    const QString uid = m_uids.at(2);
    const QString mixSession =
            QStringLiteral("decks-live-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    HistoryReporter::Options options;
    options.pOutbox = outbox();
    options.trackOfFile = trackOfFile();
    options.clientFor = [this](const QString& serverId) {
        return m_pServers->client(serverId);
    };
    options.mixSessionId = mixSession;
    auto pReporter = std::make_unique<HistoryReporter>(std::move(options));
    const QString nowPlayingPath = QStringLiteral("/api/me/now-playing");
    const int sentBefore = static_cast<int>(answered(nowPlayingPath).size());
    const auto stopPlaying = qScopeGuard([&] {
        pReporter.reset();
    });

    const TrackPointer pTrack = crateTrack(uid);
    // The third deck
    pReporter->setPlayingDeck(2);
    pReporter->setPlayingTrack(pTrack);
    ASSERT_TRUE(waitForRequest(nowPlayingPath, sentBefore));
    EXPECT_EQ(200, answered(nowPlayingPath).constLast());

    const auto userId = selectRows(
            QStringLiteral("SELECT id FROM users WHERE email = '%1'").arg(m_email));
    std::optional<QString> state;
    if (userId) {
        const QString key = QStringLiteral("cache:now_playing:%1").arg(*userId);
        state = redisGet(key);
        ASSERT_TRUE(state.has_value());
        report("now playing state: " + state->toStdString());
        const QJsonObject nowPlaying = QJsonDocument::fromJson(state->toUtf8()).object();
        EXPECT_EQ(3, nowPlaying.value(QStringLiteral("deck")).toInt());
        EXPECT_EQ(mixSession, nowPlaying.value(QStringLiteral("mix_session_id")).toString());
        EXPECT_EQ(uid.toLower(), nowPlaying.value(QStringLiteral("track_entity_uid")).toString());
        EXPECT_EQ(QStringLiteral("decks"),
                nowPlaying.value(QStringLiteral("device_type")).toString());
        EXPECT_EQ(QStringLiteral("decks"),
                nowPlaying.value(QStringLiteral("app_platform")).toString());
    } else {
        report("CRATE_DECKS_LIVE_COMPOSE not set: deck and mix_session_id are not visible "
               "through the API");
    }
    // The API shows the Decks session as the one playing
    const Answer sessions = get(QStringLiteral("/api/auth/sessions"));
    ASSERT_TRUE(sessions.ok) << sessions.error.toStdString();
    bool decksActive = false;
    const QJsonArray sessionRows = sessions.json.array();
    for (const QJsonValue& session : sessionRows) {
        const QJsonObject object = session.toObject();
        decksActive |= object.value(QStringLiteral("app_id")).toString() ==
                        QLatin1String("crate-decks") &&
                object.value(QStringLiteral("is_active")).toBool();
    }
    EXPECT_TRUE(decksActive);

    // Off the air: {"playing": false}
    const int sent = static_cast<int>(answered(nowPlayingPath).size());
    pReporter->setPlayingTrack(TrackPointer());
    ASSERT_TRUE(waitForRequest(nowPlayingPath, sent));
    EXPECT_EQ(200, answered(nowPlayingPath).constLast());
    if (userId) {
        const auto cleared = redisGet(QStringLiteral("cache:now_playing:%1").arg(*userId));
        ASSERT_TRUE(cleared.has_value());
        EXPECT_TRUE(cleared->isEmpty()) << cleared->toStdString();
        report("now playing state after {\"playing\": false}: \"" + cleared->toStdString() + "\"");
    }
}

// 3. Like and unlike through the outbox (the unlike is a DELETE with a JSON
// body) show in /api/me/likes.
TEST_F(CrateWritebackLive, LikeAndUnlikeShowInTheLikes) {
    const QSet<QString> before = likedUids();
    QString uid;
    for (int i = 3; i < 6 && uid.isEmpty(); ++i) {
        if (!before.contains(m_uids.at(i).toLower())) {
            uid = m_uids.at(i);
        }
    }
    ASSERT_FALSE(uid.isEmpty()) << "Every candidate is liked already";
    crate::writeback::CrateLikes* pLikes = m_pServers->likes();
    const QList<TrackKey> tracks{TrackKey{m_serverId, uid}};
    OutboxWatch watch(outbox());
    // Back to not liked, whatever happens
    const auto restore = qScopeGuard([&] {
        if (likedUids().contains(uid.toLower())) {
            call(QNetworkAccessManager::DeleteOperation,
                    QStringLiteral("/api/me/likes"),
                    QJsonObject{{QStringLiteral("track_entity_uid"), uid}});
        }
    });

    pLikes->setLiked(tracks, true);
    EXPECT_TRUE(pLikes->isLiked(m_serverId, uid));
    const auto liked = watch.waitDelivered(outboxkind::kLike);
    ASSERT_TRUE(liked.has_value());
    report("like answer: " + text(liked->second));
    EXPECT_TRUE(liked->second.value(QStringLiteral("added")).toBool());
    EXPECT_TRUE(likedUids().contains(uid.toLower()));

    pLikes->setLiked(tracks, false);
    EXPECT_FALSE(pLikes->isLiked(m_serverId, uid));
    const auto unliked = watch.waitDelivered(outboxkind::kUnlike);
    ASSERT_TRUE(unliked.has_value());
    report("unlike answer: " + text(unliked->second));
    EXPECT_TRUE(unliked->second.value(QStringLiteral("removed")).toBool());
    EXPECT_FALSE(likedUids().contains(uid.toLower()));
    EXPECT_EQ(before, likedUids());
}

// 4. A rating change looks the numeric id up by entity, then rates it, and
// the server keeps it; it is put back after.
TEST_F(CrateWritebackLive, RatingReachesTheServerByItsNumericId) {
    const QString uid = m_uids.at(6);
    const QString infoPath = QStringLiteral("/api/tracks/by-entity/%1/info").arg(uid);
    const Answer info = get(infoPath);
    ASSERT_TRUE(info.ok) << info.error.toStdString();
    const qint64 trackId = info.object().value(QStringLiteral("id")).toInteger();
    const int original = info.object().value(QStringLiteral("rating")).toInt();
    ASSERT_GT(trackId, 0) << "The track's info has no numeric id";
    const int target = original == 4 ? 2 : 4;
    report("track " + uid.toStdString() + " is " + std::to_string(trackId) + ", rated " +
            std::to_string(original));

    const TrackId localId(QVariant(1));
    const TrackPointer pTrack = Track::newDummy(location(uid), localId);
    CrateRatings::Options options;
    options.pOutbox = outbox();
    options.pDbConnectionPool = dbConnectionPooler();
    options.trackOfFile = trackOfFile();
    options.trackById = [&](TrackId trackId) {
        return trackId == localId ? pTrack : TrackPointer();
    };
    CrateRatings ratings(std::move(options));
    ratings.start();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return ratings.isLoaded();
    }));
    OutboxWatch watch(outbox());
    const auto restore = qScopeGuard([&] {
        if (get(infoPath).object().value(QStringLiteral("rating")).toInt() != original) {
            call(QNetworkAccessManager::PostOperation,
                    QStringLiteral("/api/track/rate"),
                    QJsonObject{{QStringLiteral("track_id"), trackId},
                            {QStringLiteral("rating"), original}});
        }
        ratings.waitForStorage();
    });

    pTrack->setRating(target);
    ratings.onTracksChanged({localId});
    const auto rated = watch.waitDelivered(outboxkind::kRate);
    ASSERT_TRUE(rated.has_value());
    report("rate payload " + text(rated->first) + ", answer " + text(rated->second));
    EXPECT_EQ(trackId, rated->first.value(QStringLiteral("track_id")).toInteger());
    EXPECT_EQ(target, rated->second.value(QStringLiteral("rating")).toInt());
    EXPECT_EQ(target, get(infoPath).object().value(QStringLiteral("rating")).toInt());

    // Back to what it was, through Decks too
    pTrack->setRating(original);
    ratings.onTracksChanged({localId});
    const auto restored = watch.waitDelivered(outboxkind::kRate, 1);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(original, get(infoPath).object().value(QStringLiteral("rating")).toInt());
}

// 5. A temporary playlist exported, added to, removed from and reordered
// through CratePlaylistEdit, with the library sync's copy as Decks keeps it;
// then a change on the server meanwhile takes the re-read, compare and
// retry-once path.
TEST_F(CrateWritebackLive, PlaylistEditsReachTheServer) {
    const QStringList uids = [this] {
        QStringList lower;
        for (int i = 7; i < 12; ++i) {
            lower.append(m_uids.at(i).toLower());
        }
        return lower;
    }();
    crate::library::LibrarySync sync(m_serverId, m_pClient, dbConnectionPooler());
    QSignalSpy synced(&sync, &crate::library::LibrarySync::containerSynced);
    QSignalSpy treeUpdated(&sync, &crate::library::LibrarySync::containersUpdated);
    QSignalSpy failed(&sync, &crate::library::LibrarySync::syncFailed);

    const auto containerOf = [this](qint64 playlistId) -> qint64 {
        const QList<crate::library::StoredContainer> containers =
                crate::library::loadContainers(dbConnection(), m_serverId);
        for (const crate::library::StoredContainer& container : containers) {
            if (container.kind == QLatin1String("playlist") &&
                    container.remoteId == QString::number(playlistId)) {
                return container.id;
            }
        }
        return 0;
    };
    int resyncs = 0;
    // As CrateServerFeature wires it
    CratePlaylistEdit::Options options;
    options.clientFor = [this](const QString& serverId) {
        return m_pServers->client(serverId);
    };
    options.readStored = [&](const QString&,
                                 qint64 playlistId,
                                 const std::function<void(std::optional<QStringList>)>& done) {
        const qint64 containerId = containerOf(playlistId);
        if (containerId <= 0) {
            done(std::nullopt);
            return;
        }
        done(crate::library::loadContainerTrackUids(
                dbConnection(), containerId, 0, std::numeric_limits<int>::max()));
    };
    options.resync = [&](const QString&, qint64 playlistId, const std::function<void(bool)>& done) {
        ++resyncs;
        const qint64 containerId = playlistId > 0 ? containerOf(playlistId) : 0;
        if (containerId <= 0) {
            sync.syncContainers();
            done(playlistId <= 0);
            return;
        }
        auto pConnections = std::make_shared<std::pair<QMetaObject::Connection,
                QMetaObject::Connection>>();
        const auto finish = [pConnections, done](bool ok) {
            QObject::disconnect(pConnections->first);
            QObject::disconnect(pConnections->second);
            done(ok);
        };
        pConnections->first = QObject::connect(&sync,
                &crate::library::LibrarySync::containerSynced,
                [containerId, finish](qint64 done) {
                    if (done == containerId) {
                        finish(true);
                    }
                });
        pConnections->second = QObject::connect(&sync,
                &crate::library::LibrarySync::syncFailed,
                [containerId, finish](qint64 failed) {
                    if (failed == containerId) {
                        finish(false);
                    }
                });
        sync.syncContainerTracks(containerId);
    };
    CratePlaylistEdit edit(std::move(options));
    QSignalSpy warnings(&edit, &CratePlaylistEdit::warning);

    std::optional<CratePlaylistEdit::Outcome> outcome;
    const auto onDone = [&outcome](const CratePlaylistEdit::Outcome& done) {
        outcome = done;
    };
    const auto waitOutcome = [&]() -> CratePlaylistEdit::Outcome {
        EXPECT_TRUE(QTest::qWaitFor(
                [&] {
                    return outcome.has_value();
                },
                kNetworkWaitMs));
        const CratePlaylistEdit::Outcome done = outcome.value_or(CratePlaylistEdit::Outcome());
        outcome.reset();
        return done;
    };
    // Decks' copy of the playlist, synced until it is the server's
    const auto storedMatchesServer = [&](qint64 playlistId) {
        int rounds = 0;
        const bool matched = QTest::qWaitFor(
                [&] {
                    qint64 containerId = containerOf(playlistId);
                    if (containerId <= 0) {
                        const int trees = static_cast<int>(treeUpdated.size() + failed.size());
                        sync.syncContainers();
                        static_cast<void>(QTest::qWaitFor(
                                [&] {
                                    return treeUpdated.size() + failed.size() > trees;
                                },
                                kNetworkWaitMs));
                        containerId = containerOf(playlistId);
                        if (containerId <= 0) {
                            return false;
                        }
                    }
                    const QStringList stored = crate::library::loadContainerTrackUids(
                            dbConnection(), containerId, 0, std::numeric_limits<int>::max());
                    QStringList lower;
                    for (const QString& uid : stored) {
                        lower.append(uid.toLower());
                    }
                    if (sync.isSyncing(containerId)) {
                        // A sync of the edit is on its way: wait for it, and
                        // do not queue another one behind it
                        const int before = static_cast<int>(synced.size() + failed.size());
                        static_cast<void>(QTest::qWaitFor(
                                [&] {
                                    return synced.size() + failed.size() > before;
                                },
                                kNetworkWaitMs));
                        return false;
                    }
                    if (lower == playlistOrder(playlistId)) {
                        return true;
                    }
                    ++rounds;
                    const int before = static_cast<int>(synced.size() + failed.size());
                    sync.syncContainerTracks(containerId);
                    static_cast<void>(QTest::qWaitFor(
                            [&] {
                                return synced.size() + failed.size() > before;
                            },
                            kNetworkWaitMs));
                    return false;
                },
                kNetworkWaitMs);
        if (rounds > 1) {
            report("Decks' copy took " + std::to_string(rounds) + " syncs to match the server");
        }
        return matched;
    };

    // Export: a new playlist with three tracks
    const QString name = QStringLiteral("Decks live check %1")
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    edit.exportTracks(m_serverId, name, uids.mid(0, 3), 0, onDone);
    const CratePlaylistEdit::Outcome exported = waitOutcome();
    ASSERT_TRUE(exported.ok) << exported.message.toStdString();
    const qint64 playlistId = exported.playlistId;
    ASSERT_GT(playlistId, 0);
    report("temporary playlist " + std::to_string(playlistId) + " \"" + name.toStdString() + "\"");
    const auto deletePlaylist = qScopeGuard([&] {
        const Answer deleted = call(QNetworkAccessManager::DeleteOperation,
                QStringLiteral("/api/playlists/%1").arg(playlistId));
        EXPECT_TRUE(deleted.ok) << deleted.error.toStdString();
        const Answer gone = get(QStringLiteral("/api/playlists/%1").arg(playlistId));
        EXPECT_EQ(404, gone.status);
    });
    EXPECT_EQ(uids.mid(0, 3), playlistOrder(playlistId));
    ASSERT_TRUE(storedMatchesServer(playlistId));

    // Add one at the end
    edit.add(m_serverId, playlistId, {uids[3]}, onDone);
    CratePlaylistEdit::Outcome done = waitOutcome();
    ASSERT_TRUE(done.ok) << done.message.toStdString();
    EXPECT_EQ((QStringList{uids[0], uids[1], uids[2], uids[3]}), playlistOrder(playlistId));
    ASSERT_TRUE(storedMatchesServer(playlistId));

    // Remove the second
    edit.remove(m_serverId, playlistId, {CratePlaylistEdit::Row{1, uids[1]}}, onDone);
    done = waitOutcome();
    ASSERT_TRUE(done.ok) << done.message.toStdString();
    EXPECT_EQ((QStringList{uids[0], uids[2], uids[3]}), playlistOrder(playlistId));
    ASSERT_TRUE(storedMatchesServer(playlistId));

    // Move the last one first
    edit.move(m_serverId,
            playlistId,
            {CratePlaylistEdit::Row{2, uids[3]}},
            CratePlaylistEdit::Row{0, uids[0]},
            onDone);
    done = waitOutcome();
    ASSERT_TRUE(done.ok) << done.message.toStdString();
    EXPECT_EQ((QStringList{uids[3], uids[0], uids[2]}), playlistOrder(playlistId));
    ASSERT_TRUE(storedMatchesServer(playlistId));

    // A drag in the table: the whole new order
    edit.reorder(m_serverId, playlistId, {uids[2], uids[3], uids[0]}, onDone);
    done = waitOutcome();
    ASSERT_TRUE(done.ok) << done.message.toStdString();
    EXPECT_EQ((QStringList{uids[2], uids[3], uids[0]}), playlistOrder(playlistId));
    ASSERT_TRUE(storedMatchesServer(playlistId));
    EXPECT_TRUE(warnings.isEmpty());

    // Another client adds a track meanwhile: Decks' copy is behind
    const Answer added = call(QNetworkAccessManager::PostOperation,
            QStringLiteral("/api/playlists/%1/tracks").arg(playlistId),
            QJsonObject{{QStringLiteral("tracks"),
                    QJsonArray{QJsonObject{{QStringLiteral("entity_uid"), uids[4]}}}}});
    ASSERT_TRUE(added.ok) << added.error.toStdString();
    EXPECT_EQ((QStringList{uids[2], uids[3], uids[0], uids[4]}), playlistOrder(playlistId));
    const int resyncsBefore = resyncs;
    // Decks removes the first of its copy: read, compare, sync, retry
    edit.remove(m_serverId, playlistId, {CratePlaylistEdit::Row{0, uids[2]}}, onDone);
    done = waitOutcome();
    ASSERT_TRUE(done.ok) << done.message.toStdString();
    EXPECT_FALSE(done.conflict);
    // One resync before the retry, one after the write
    EXPECT_EQ(resyncsBefore + 2, resyncs);
    EXPECT_EQ((QStringList{uids[3], uids[0], uids[4]}), playlistOrder(playlistId));
    EXPECT_TRUE(warnings.isEmpty());
    ASSERT_TRUE(storedMatchesServer(playlistId));
}

// 6. Cue sync: a set through Decks' path, read back (revision, server_time,
// every type with colours and labels); another computer maps it back; a
// stale base_revision gets the 409 and the server wins on the server's
// clock; an identical retry gets 200.
TEST_F(CrateWritebackLive, CueSetsSyncAcrossComputers) {
    const QString uid = m_uids.at(12);
    const auto [originalSet, originalTime] = queryUserData(QStringLiteral("cues"), uid);
    ASSERT_FALSE(originalTime.isEmpty()) << "No server_time in the cue query";
    const qint64 originalRevision =
            originalSet ? originalSet->value(QStringLiteral("revision")).toInteger() : 0;
    ASSERT_TRUE(!originalSet || originalSet->value(QStringLiteral("cues")).toArray().isEmpty())
            << "The user has cues of " << uid.toStdString() << " already: "
            << text(*originalSet);
    const auto emptySet = qScopeGuard([&] {
        const auto [set, serverTime] = queryUserData(QStringLiteral("cues"), uid);
        if (set && !set->value(QStringLiteral("cues")).toArray().isEmpty()) {
            const Answer emptied = call(QNetworkAccessManager::PutOperation,
                    QStringLiteral("/api/decks/cues/%1").arg(uid),
                    QJsonObject{{QStringLiteral("base_revision"),
                                        set->value(QStringLiteral("revision"))},
                            {QStringLiteral("cues"), QJsonArray()}});
            EXPECT_TRUE(emptied.ok) << emptied.error.toStdString();
        }
    });
    // The other computer, made before this one touches the track
    std::unique_ptr<OtherMachine> pOther = otherMachine();
    CrateCueSync* pCues = m_pServers->cueSync();
    OutboxWatch watch(outbox());
    const QString queryPath = QStringLiteral("/api/decks/cues/query");
    const int queriesBefore = static_cast<int>(answered(queryPath).size());

    // This computer loads the track and sets every kind of cue
    const TrackPointer pTrack = crateTrack(uid);
    pCues->attach(pTrack, m_serverId, uid);
    ASSERT_TRUE(waitForRequest(queryPath, queriesBefore));
    QTest::qWait(300);
    pTrack->setMainCuePosition(mixxx::audio::FramePos(framesOf(250.0)));
    pTrack->createAndAddCue(mixxx::CueType::Intro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(framesOf(1000.0)),
            mixxx::audio::FramePos(framesOf(16000.0)));
    pTrack->createAndAddCue(mixxx::CueType::Outro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(framesOf(200000.0)),
            mixxx::audio::FramePos(framesOf(230000.0)));
    const CuePointer pHotcue = pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(framesOf(15240.517)),
            mixxx::audio::kInvalidFramePos,
            mixxx::RgbColor(0xFF8800));
    pHotcue->setLabel(QStringLiteral("Drop · live"));
    const CuePointer pLoop = pTrack->createAndAddCue(mixxx::CueType::Loop,
            1,
            mixxx::audio::FramePos(framesOf(60000.0)),
            mixxx::audio::FramePos(framesOf(67500.0)),
            mixxx::RgbColor(0x00AAFF));
    pLoop->setLabel(QStringLiteral("Build"));
    pTrack->createAndAddCue(mixxx::CueType::Jump,
            2,
            mixxx::audio::FramePos(framesOf(30000.0)),
            mixxx::audio::FramePos(framesOf(90000.0)),
            mixxx::RgbColor(0x32BE44));
    const SyncedCues local = crate::writeback::syncedCuesOf(pTrack);
    ASSERT_EQ(6, local.size());

    // After the debounce, the whole set goes up
    const auto put = watch.waitDelivered(outboxkind::kCueSet);
    ASSERT_TRUE(put.has_value());
    const QJsonObject body = put->first.value(QStringLiteral("body")).toObject();
    report("PUT body " + text(body));
    report("PUT answer " + text(put->second));
    EXPECT_EQ(originalRevision, body.value(QStringLiteral("base_revision")).toInteger());
    EXPECT_EQ(m_fingerprint, body.value(QStringLiteral("client_id")).toString());
    const qint64 revision = put->second.value(QStringLiteral("revision")).toInteger();
    EXPECT_EQ(originalRevision + 1, revision);
    EXPECT_FALSE(put->second.value(QStringLiteral("server_time")).toString().isEmpty());
    EXPECT_EQ(revision, pCues->revision(m_serverId, uid));

    // Read back: the same set, in the contract's canonical form
    const auto [stored, storedTime] = queryUserData(QStringLiteral("cues"), uid);
    ASSERT_TRUE(stored.has_value());
    EXPECT_FALSE(storedTime.isEmpty());
    EXPECT_EQ(revision, stored->value(QStringLiteral("revision")).toInteger());
    EXPECT_EQ(m_fingerprint, stored->value(QStringLiteral("client_id")).toString());
    const QJsonArray storedCues = stored->value(QStringLiteral("cues")).toArray();
    EXPECT_EQ(crate::writeback::toJson(local), storedCues) << text(storedCues);
    const auto cueOf = [&storedCues](const QString& type, int slot) {
        for (const QJsonValue& cue : storedCues) {
            if (cue.toObject().value(QStringLiteral("cue_type")).toString() == type &&
                    cue.toObject().value(QStringLiteral("slot")).toInt() == slot) {
                return cue.toObject();
            }
        }
        return QJsonObject();
    };
    EXPECT_DOUBLE_EQ(250.0,
            cueOf(QStringLiteral("main"), 0)
                    .value(QStringLiteral("start_ms"))
                    .toDouble());
    EXPECT_DOUBLE_EQ(16000.0,
            cueOf(QStringLiteral("intro"), 0)
                    .value(QStringLiteral("end_ms"))
                    .toDouble());
    EXPECT_DOUBLE_EQ(200000.0,
            cueOf(QStringLiteral("outro"), 0)
                    .value(QStringLiteral("start_ms"))
                    .toDouble());
    const QJsonObject hotcue = cueOf(QStringLiteral("hotcue"), 0);
    EXPECT_DOUBLE_EQ(15240.517, hotcue.value(QStringLiteral("start_ms")).toDouble());
    EXPECT_EQ(QStringLiteral("Drop · live"), hotcue.value(QStringLiteral("label")).toString());
    EXPECT_EQ(QStringLiteral("#ff8800"), hotcue.value(QStringLiteral("color")).toString());
    const QJsonObject loop = cueOf(QStringLiteral("loop"), 1);
    EXPECT_DOUBLE_EQ(67500.0, loop.value(QStringLiteral("end_ms")).toDouble());
    EXPECT_EQ(QStringLiteral("Build"), loop.value(QStringLiteral("label")).toString());
    EXPECT_EQ(QStringLiteral("#00aaff"), loop.value(QStringLiteral("color")).toString());
    const QJsonObject jump = cueOf(QStringLiteral("jump"), 2);
    EXPECT_DOUBLE_EQ(30000.0, jump.value(QStringLiteral("start_ms")).toDouble());
    EXPECT_DOUBLE_EQ(90000.0, jump.value(QStringLiteral("end_ms")).toDouble());
    EXPECT_EQ(QStringLiteral("#32be44"), jump.value(QStringLiteral("color")).toString());

    // The other computer loads the track: the server's set maps back
    QSignalSpy otherApplied(pOther->pCues.get(), &CrateCueSync::serverDataApplied);
    const TrackPointer pOtherTrack = crateTrack(uid);
    pOther->pCues->attach(pOtherTrack, m_serverId, uid);
    pOther->scheduler.runAll();
    ASSERT_TRUE(otherApplied.wait(kNetworkWaitMs));
    EXPECT_EQ(local, crate::writeback::syncedCuesOf(pOtherTrack));
    const CuePointer pOtherHotcue = pOtherTrack->findHotcueByIndex(0);
    ASSERT_NE(nullptr, pOtherHotcue);
    EXPECT_EQ(QStringLiteral("Drop · live"), pOtherHotcue->getLabel());
    EXPECT_EQ(mixxx::RgbColor(0xFF8800), pOtherHotcue->getColor());
    EXPECT_NEAR(framesOf(15240.517), pOtherHotcue->getPosition().value(), 0.01);
    const CuePointer pOtherLoop = pOtherTrack->findHotcueByIndex(1);
    ASSERT_NE(nullptr, pOtherLoop);
    EXPECT_EQ(mixxx::CueType::Loop, pOtherLoop->getType());
    EXPECT_NEAR(framesOf(67500.0), pOtherLoop->getEndPosition().value(), 0.01);
    const CuePointer pOtherJump = pOtherTrack->findHotcueByIndex(2);
    ASSERT_NE(nullptr, pOtherJump);
    EXPECT_EQ(mixxx::CueType::Jump, pOtherJump->getType());
    EXPECT_NEAR(framesOf(90000.0), pOtherJump->getEndPosition().value(), 0.01);
    ASSERT_NE(nullptr, pOtherTrack->findCueByType(mixxx::CueType::Intro));
    EXPECT_NEAR(framesOf(1000.0),
            pOtherTrack->findCueByType(mixxx::CueType::Intro)->getPosition().value(),
            0.01);
    EXPECT_NEAR(framesOf(250.0), pOtherTrack->getMainCuePosition().value(), 0.01);
    EXPECT_EQ(revision, pOther->pCues->revision(m_serverId, uid));

    // The other computer edits offline; this one edits after it, online
    QSignalSpy replaced(pOther->pCues.get(), &CrateCueSync::localEditsReplaced);
    OutboxWatch otherWatch(pOther->pOutbox.get());
    pOther->online = false;
    pOtherTrack->createAndAddCue(mixxx::CueType::HotCue,
            5,
            mixxx::audio::FramePos(framesOf(120000.0)),
            mixxx::audio::kInvalidFramePos);
    pOther->scheduler.runAll();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return pOther->pOutbox->pendingCount() == 1;
    }));
    const QJsonObject offlineEdit =
            pOther->pOutbox->pendingPayloads(m_serverId, outboxkind::kCueSet).constFirst();
    report("offline edit stamped " +
            offlineEdit.value(QStringLiteral("edited_at"))
                    .toString()
                    .toStdString());
    QTest::qWait(100);
    pHotcue->setLabel(QStringLiteral("Drop · edited later"));
    const auto later = watch.waitDelivered(outboxkind::kCueSet, 1);
    ASSERT_TRUE(later.has_value());
    const qint64 laterRevision = later->second.value(QStringLiteral("revision")).toInteger();
    EXPECT_EQ(revision + 1, laterRevision);
    report("later edit stored at " +
            later->second.value(QStringLiteral("updated_at")).toString().toStdString());

    // Back online: its PUT has a stale base_revision
    pOther->online = true;
    pOther->pOutbox->flush();
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                return !otherWatch.conflicted.isEmpty() || !otherWatch.delivered.isEmpty() ||
                        !otherWatch.dropped.isEmpty();
            },
            kNetworkWaitMs));
    ASSERT_EQ(1, otherWatch.conflicted.size()) << "no 409";
    const QJsonObject conflict = otherWatch.conflicted.constFirst().at(3).toJsonObject();
    report("409 answer " + text(conflict));
    EXPECT_FALSE(conflict.value(QStringLiteral("server_time")).toString().isEmpty());
    const QJsonObject current = conflict.value(QStringLiteral("current")).toObject();
    EXPECT_EQ(laterRevision, current.value(QStringLiteral("revision")).toInteger());
    EXPECT_EQ(m_fingerprint, current.value(QStringLiteral("client_id")).toString());
    // The server's set wins: written after the offline edit, by another computer
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return replaced.size() == 1;
    }));
    EXPECT_EQ(nullptr, pOtherTrack->findHotcueByIndex(5));
    EXPECT_EQ(QStringLiteral("Drop · edited later"), pOtherTrack->findHotcueByIndex(0)->getLabel());
    EXPECT_EQ(laterRevision, pOther->pCues->revision(m_serverId, uid));
    pOther->scheduler.runAll();
    QTest::qWait(300);
    EXPECT_TRUE(otherWatch.delivered.isEmpty()) << "the losing edit was sent again";
    EXPECT_EQ(0, pOther->pOutbox->pendingCount());
    EXPECT_EQ(laterRevision,
            queryUserData(QStringLiteral("cues"), uid)
                    .first.value_or(QJsonObject())
                    .value(QStringLiteral("revision"))
                    .toInteger());

    // The same PUT again, as a retry after a lost answer: 200, no new revision
    outbox()->enqueue(m_serverId, outboxkind::kCueSet, later->first);
    const auto retried = watch.waitDelivered(outboxkind::kCueSet, 2);
    ASSERT_TRUE(retried.has_value());
    EXPECT_TRUE(watch.conflicted.isEmpty());
    EXPECT_EQ(laterRevision, retried->second.value(QStringLiteral("revision")).toInteger());
    report("identical retry with base_revision " +
            std::to_string(later->first.value(QStringLiteral("body"))
                            .toObject()
                            .value(QStringLiteral("base_revision"))
                            .toInteger()) +
            " answered revision " +
            std::to_string(retried->second.value(QStringLiteral("revision")).toInteger()));

    // Cleared from Decks: the set is emptied
    crate::writeback::applySyncedCues(pTrack, {});
    const auto emptied = watch.waitDelivered(outboxkind::kCueSet, 3);
    ASSERT_TRUE(emptied.has_value());
    EXPECT_TRUE(emptied->second.value(QStringLiteral("cues")).toArray().isEmpty());
    const auto [after, afterTime] = queryUserData(QStringLiteral("cues"), uid);
    ASSERT_TRUE(after.has_value());
    EXPECT_TRUE(after->value(QStringLiteral("cues")).toArray().isEmpty());
    report("emptied at revision " +
            std::to_string(after->value(QStringLiteral("revision")).toInteger()));
}

// 7. A BPM, grid and key correction saved from Decks, read back, applied on
// another computer, then cleared.
TEST_F(CrateWritebackLive, CorrectionsSaveAndClear) {
    const QString uid = m_uids.at(13);
    const auto [original, originalTime] = queryUserData(QStringLiteral("corrections"), uid);
    ASSERT_FALSE(originalTime.isEmpty()) << "No server_time in the correction query";
    const auto hasValues = [](const QJsonObject& correction) {
        return !correction.value(QStringLiteral("bpm")).isNull() ||
                !correction.value(QStringLiteral("beat_grid_ms")).isNull() ||
                !correction.value(QStringLiteral("key")).isNull();
    };
    ASSERT_TRUE(!original || !hasValues(*original))
            << "The user has a correction of " << uid.toStdString() << " already: "
            << text(*original);
    const qint64 originalRevision =
            original ? original->value(QStringLiteral("revision")).toInteger() : 0;
    const auto clearCorrection = qScopeGuard([&] {
        const auto [stored, serverTime] = queryUserData(QStringLiteral("corrections"), uid);
        if (stored && hasValues(*stored)) {
            const Answer cleared = call(QNetworkAccessManager::PutOperation,
                    QStringLiteral("/api/decks/corrections/%1").arg(uid),
                    QJsonObject{{QStringLiteral("base_revision"),
                            stored->value(QStringLiteral("revision"))}});
            EXPECT_TRUE(cleared.ok) << cleared.error.toStdString();
        }
    });
    std::unique_ptr<OtherMachine> pOther = otherMachine();
    CrateCorrections* pCorrections = m_pServers->corrections();
    OutboxWatch watch(outbox());
    QSignalSpy offered(pCorrections, &CrateCorrections::correctionOffered);
    const QString queryPath = QStringLiteral("/api/decks/corrections/query");
    const int queriesBefore = static_cast<int>(answered(queryPath).size());

    // The track as the analysis left it: 120 BPM from 0.5 s, C major
    const TrackPointer pTrack = crateTrack(uid);
    pTrack->trySetBeats(mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::FramePos(0.5 * kSampleRate),
            mixxx::Bpm(120.0)));
    pTrack->setKey(mixxx::track::io::key::C_MAJOR, mixxx::track::io::key::ANALYZER);
    pCorrections->attach(pTrack, m_serverId, uid);
    ASSERT_TRUE(waitForRequest(queryPath, queriesBefore));
    QTest::qWait(300);

    // The user unlocks the BPM, puts the grid at 127.95 BPM from 512 ms, and
    // types the key
    pTrack->setBpmLocked(false);
    ASSERT_TRUE(pTrack->trySetBeats(mixxx::Beats::fromConstTempo(
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::FramePos(framesOf(512.0)),
            mixxx::Bpm(127.95))));
    pTrack->setKeyText(QStringLiteral("Am"), mixxx::track::io::key::USER);
    ASSERT_TRUE(offered.wait(kNetworkWaitMs));
    const auto offer = pCorrections->offer(m_serverId, uid);
    ASSERT_TRUE(offer.has_value());
    EXPECT_TRUE(offer->grid);
    EXPECT_TRUE(offer->key);
    const QVector<qint64> grid = crate::writeback::beatGridOf(pTrack);
    ASSERT_GT(grid.size(), 2);

    // "Save correction to Crate"
    ASSERT_TRUE(pCorrections->save(m_serverId, uid));
    const auto saved = watch.waitDelivered(outboxkind::kCorrection);
    ASSERT_TRUE(saved.has_value());
    QJsonObject savedAnswer = saved->second;
    QJsonArray savedGrid = savedAnswer.take(QStringLiteral("beat_grid_ms")).toArray();
    report("correction answer " + text(savedAnswer) + " with " +
            std::to_string(savedGrid.size()) + " beats from " +
            std::to_string(savedGrid.isEmpty() ? 0 : savedGrid.first().toInteger()) + " ms");
    const qint64 revision = saved->second.value(QStringLiteral("revision")).toInteger();
    EXPECT_EQ(originalRevision + 1, revision);
    EXPECT_FALSE(saved->second.value(QStringLiteral("server_time")).toString().isEmpty());

    // Read back
    const auto [stored, storedTime] = queryUserData(QStringLiteral("corrections"), uid);
    ASSERT_TRUE(stored.has_value());
    EXPECT_FALSE(storedTime.isEmpty());
    EXPECT_EQ(revision, stored->value(QStringLiteral("revision")).toInteger());
    EXPECT_NEAR(127.95, stored->value(QStringLiteral("bpm")).toDouble(), 0.01);
    EXPECT_EQ(QStringLiteral("delta-ms-v1"),
            stored->value(QStringLiteral("beat_grid_format")).toString());
    EXPECT_EQ(QStringLiteral("8A"), stored->value(QStringLiteral("key")).toString());
    const QJsonArray storedGrid = stored->value(QStringLiteral("beat_grid_ms")).toArray();
    ASSERT_EQ(grid.size(), storedGrid.size());
    for (int i = 0; i < grid.size(); ++i) {
        ASSERT_EQ(grid[i], storedGrid[i].toInteger()) << "beat " << i;
    }
    // From the start of the file: the beat before the one the user put at 512 ms
    EXPECT_EQ(43, storedGrid.first().toInteger());
    EXPECT_EQ(512, storedGrid.at(1).toInteger());
    EXPECT_TRUE(pCorrections->hasCorrection(m_serverId, uid));

    // Another computer applies it over the analysis
    QSignalSpy otherApplied(pOther->pCorrections.get(), &CrateCorrections::serverDataApplied);
    const TrackPointer pOtherTrack = crateTrack(uid);
    pOtherTrack->trySetBeats(mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::FramePos(0.5 * kSampleRate),
            mixxx::Bpm(120.0)));
    pOther->pCorrections->attach(pOtherTrack, m_serverId, uid);
    pOther->scheduler.runAll();
    ASSERT_TRUE(otherApplied.wait(kNetworkWaitMs));
    EXPECT_TRUE(pOtherTrack->isBpmLocked());
    EXPECT_NEAR(127.95, pOtherTrack->getBpm(), 0.01);
    EXPECT_EQ(mixxx::track::io::key::A_MINOR, pOtherTrack->getKey());
    // The same beats: delta-ms-v1 keeps whole milliseconds, so the other
    // computer fits the grid back from them (its phase within a few hundredths
    // of a millisecond) and a beat that sat on a half millisecond may round
    // the other way
    const QVector<qint64> otherGrid = crate::writeback::beatGridOf(pOtherTrack);
    ASSERT_EQ(grid.size(), otherGrid.size());
    qint64 largest = 0;
    int differing = 0;
    for (int i = 0; i < grid.size(); ++i) {
        const qint64 difference = std::abs(grid[i] - otherGrid[i]);
        differing += difference > 0 ? 1 : 0;
        largest = std::max(largest, difference);
    }
    EXPECT_LE(largest, 1);
    double furthestMs = 0.0;
    auto here = pTrack->getBeats()->iteratorFrom(mixxx::audio::kStartFramePos);
    auto there = pOtherTrack->getBeats()->iteratorFrom(mixxx::audio::kStartFramePos);
    for (int i = 0; i < grid.size(); ++i, ++here, ++there) {
        furthestMs = std::max(furthestMs,
                std::abs(here->value() - there->value()) * 1000.0 /
                        kSampleRate);
    }
    EXPECT_LT(furthestMs, 0.1);
    report("the other computer's beats are within " + std::to_string(furthestMs) +
            " ms of these; as whole ms, " + std::to_string(differing) + " of " +
            std::to_string(grid.size()) + " round the other way");

    // "Clear correction on Crate"
    ASSERT_TRUE(pCorrections->clear(m_serverId, uid));
    const auto cleared = watch.waitDelivered(outboxkind::kCorrection, 1);
    ASSERT_TRUE(cleared.has_value());
    report("cleared: " + text(cleared->second));
    EXPECT_EQ(revision + 1, cleared->second.value(QStringLiteral("revision")).toInteger());
    const auto [after, afterTime] = queryUserData(QStringLiteral("corrections"), uid);
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(hasValues(*after)) << text(*after);
    EXPECT_EQ(revision + 1, after->value(QStringLiteral("revision")).toInteger());
    EXPECT_FALSE(pCorrections->hasCorrection(m_serverId, uid));
}

} // namespace
