// F8.SM, live: the Smart Mix plans Decks asks a Crate dev stack for follow the
// signed-in user's BPM, grid and key corrections. The corrections are saved
// the way the track menu's "Save correction to Crate" saves them (the real
// CrateCorrections and Outbox of CrateServers, signed in through
// AddServerFlow), the plans are asked with Auto DJ's own context (v3 with the
// candidate menu) through a PlanClient that drops a track's plans on
// correctionChanged() as CrateServerFeature does, and then:
//
// - a BPM and grid correction more than 2 % from the profile's BPM: the edge
//   beatmatches at the corrected tempo, and the corrected track's phrases are
//   gone (the planner's bar heuristic);
// - a grid correction within 2 %, a few hundred ms later: the phrases stay,
//   snapped to the corrected downbeats, which the menu's incoming cues sit on;
// - a key-only correction: the harmonic term of the menu changes, the tempo
//   and the phrases do not;
// - Decks' limit check (TransitionPlan::limitViolations) agrees with the
//   server on every candidate, before and after;
// - the same edges asked as Listen asks them (source "playlist") get the
//   plans they got before the corrections;
// - cleared from Decks ("Clear correction on Crate"), the plans are the
//   shared ones again.
//
// Every correction the test made is cleared, through Decks or, if the test
// stopped before, by TearDown with a PUT of nulls; the corrections query then
// shows none.
//
// Skipped unless CRATE_DECKS_LIVE_URL is set, so CI never runs it:
//   CRATE_DECKS_LIVE_URL         a dev stack (loopback or private address
//                                only), e.g. http://127.0.0.1:8585
//   CRATE_DECKS_LIVE_EMAIL,
//   CRATE_DECKS_LIVE_PASSWORD    an account of that dev stack; never
//                                production
// The edges come from the playlist "G3 · Electronic (Q3 Q5 grids)", whose
// tracks have smart-mix-audio-v10 profiles with grids. It reports [ LIVE ]
// lines.

#include <gtest/gtest.h>

#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "crate/analysis/analysisimporter.h"
#include "crate/client/crateclient.h"
#include "crate/crateaddserverflow.h"
#include "crate/crateservers.h"
#include "crate/session/cratesecretstore.h"
#include "crate/session/cratesession.h"
#include "crate/smartmix/autodjsmartmixadapter.h"
#include "crate/smartmix/planclient.h"
#include "crate/writeback/cratecorrections.h"
#include "crate/writeback/outbox.h"
#include "test/mixxxdbtest.h"
#include "track/beats.h"
#include "track/track.h"

namespace {

using crate::PlanEdge;
using crate::PlanFetch;
using crate::PlanResults;
using crate::TransitionPlan;
using crate::writeback::Correction;
using crate::writeback::CrateCorrections;
using crate::writeback::Outbox;
namespace outboxkind = crate::writeback::outboxkind;

const QString kPlaylist = QStringLiteral("G3 · Electronic (Q3 Q5 grids)");
const QString kPlansPath = QStringLiteral("/api/playback/transition-plans");
const QString kQueryPath = QStringLiteral("/api/decks/corrections/query");
/// How Listen asks for the plans of a playlist.
const QString kListenSource = QStringLiteral("playlist");
constexpr int kNetworkWaitMs = 60000;
constexpr int kSampleRate = 44100;
/// The grid correction within 2 %: 0.5 % faster, its first beat 200 ms later.
constexpr double kWithinFactor = 1.005;
constexpr qint64 kWithinShiftMs = 200;

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
    // QHostAddress::isPrivateUse() needs Qt 6.6; the CI builds with Qt 6.4
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

std::string phraseText(const std::optional<int>& phrase) {
    return phrase ? std::to_string(*phrase) : std::string("-");
}

/// The plan fields a correction may change, for the report.
std::string describe(const TransitionPlan& plan) {
    std::ostringstream out;
    out << crate::toString(plan.mode).toStdString() << " fallback="
        << (plan.fallbackReason ? crate::toString(*plan.fallbackReason).toStdString()
                                : std::string("none"))
        << " ratio=" << plan.incomingTempoRatio << " phase=" << plan.beatPhaseOffsetMs
        << " cues=" << plan.outgoingCueMs << "/" << plan.incomingCueMs
        << " confidence=" << plan.confidence
        << " structure=" << (plan.structureUsed.value_or(false) ? "yes" : "no")
        << " stems=" << (plan.stemsUsed ? "yes" : "no") << " menu=[";
    for (const crate::TransitionPlanCandidate& candidate : plan.candidates) {
        out << " #" << candidate.index << " phrases " << phraseText(candidate.outgoingPhrase)
            << ">" << phraseText(candidate.incomingPhrase) << " in@" << candidate.plan.incomingCueMs
            << " harmonic " << candidate.breakdown.harmonic;
    }
    out << " ]";
    return out.str();
}

double maxHarmonic(const TransitionPlan& plan) {
    double best = 0.0;
    for (const crate::TransitionPlanCandidate& candidate : plan.candidates) {
        best = std::max(best, candidate.breakdown.harmonic);
    }
    return best;
}

/// What the profile says of a track.
struct TrackInfo {
    QString uid;
    QString title;
    QString quality;
    qint64 durationMs = 0;
    double bpm = 0.0;
    double bpmConfidence = 0.0;
    double introBpm = 0.0;
    double outroBpm = 0.0;
    qint64 beatAnchorMs = 0;
    QString camelot;
};

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

class CrateSmartMixCorrectionsLive : public MixxxDbTest {
  public:
    CrateSmartMixCorrectionsLive()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        const QString url = qEnvironmentVariable("CRATE_DECKS_LIVE_URL");
        if (url.isEmpty()) {
            GTEST_SKIP() << "Live check against a Crate dev stack (F8.SM): set "
                            "CRATE_DECKS_LIVE_URL, CRATE_DECKS_LIVE_EMAIL and "
                            "CRATE_DECKS_LIVE_PASSWORD";
        }
        ASSERT_TRUE(isDevStack(QUrl(url))) << "Only a dev stack: " << url.toStdString();
        const QString email = qEnvironmentVariable("CRATE_DECKS_LIVE_EMAIL");
        const QString password = qEnvironmentVariable("CRATE_DECKS_LIVE_PASSWORD");
        ASSERT_FALSE(email.isEmpty() || password.isEmpty())
                << "Set CRATE_DECKS_LIVE_EMAIL and CRATE_DECKS_LIVE_PASSWORD";

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
                    m_answeredPaths.append(pReply->url().path());
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
                            m_pServers->corrections()->isLoaded();
                },
                kNetworkWaitMs));
        // S7 is there
        ASSERT_TRUE(m_pServers->serverHas(m_serverId, crate::DecksFeature::Cues));
    }

    void TearDown() override {
        if (m_pServers) {
            // What the test sent has left
            EXPECT_TRUE(QTest::qWaitFor(
                    [this] {
                        return m_pServers->outbox()->pendingCount() == 0;
                    },
                    kNetworkWaitMs));
            clearLeftCorrections();
            // The test's session is revoked on the server
            if (!m_serverId.isEmpty()) {
                m_pServers->removeServer(m_serverId);
                QTest::qWait(1500);
            }
        }
        m_tracks.clear();
        m_pServers.reset();
    }

  protected:
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

    /// The tracks of the playlist with a mix profile, in order, as their
    /// profiles say.
    std::vector<TrackInfo> playlistTracks(const QString& name) {
        std::vector<TrackInfo> tracks;
        const Answer tree = get(QStringLiteral("/api/decks/containers"));
        EXPECT_TRUE(tree.ok) << tree.error.toStdString();
        const QJsonArray containers = tree.object().value(QStringLiteral("containers")).toArray();
        for (const QJsonValue& value : containers) {
            const QJsonObject container = value.toObject();
            if (container.value(QStringLiteral("kind")).toString() != QLatin1String("playlist") ||
                    container.value(QStringLiteral("name")).toString() != name) {
                continue;
            }
            const Answer rows = get(QStringLiteral("/api/decks/containers/playlist/%1/tracks")
                                            .arg(container.value(QStringLiteral("id"))
                                                            .toVariant()
                                                            .toString()),
                    QUrlQuery(QStringLiteral("limit=100")));
            EXPECT_TRUE(rows.ok) << rows.error.toStdString();
            const QJsonArray array = rows.object().value(QStringLiteral("tracks")).toArray();
            for (const QJsonValue& row : array) {
                const QJsonObject track = row.toObject();
                const QString quality = track.value(QStringLiteral("profile_quality")).toString();
                if (quality != QLatin1String("full") && quality != QLatin1String("partial")) {
                    continue;
                }
                const QString uid =
                        track.value(QStringLiteral("entity_uid")).toString().toLower();
                const Answer profile =
                        get(QStringLiteral("/api/tracks/by-entity/%1/mix-profile").arg(uid));
                EXPECT_TRUE(profile.ok) << uid.toStdString() << ": " << profile.error.toStdString();
                const QJsonObject json = profile.object();
                TrackInfo info;
                info.uid = uid;
                info.title = track.value(QStringLiteral("title")).toString();
                info.quality = quality;
                info.durationMs = json.value(QStringLiteral("durationMs")).toInteger();
                info.bpm = json.value(QStringLiteral("bpm")).toDouble();
                info.bpmConfidence = json.value(QStringLiteral("bpmConfidence")).toDouble();
                info.introBpm = json.value(QStringLiteral("introBpm")).toDouble(info.bpm);
                info.outroBpm = json.value(QStringLiteral("outroBpm")).toDouble(info.bpm);
                info.beatAnchorMs = json.value(QStringLiteral("beatAnchorMs")).toInteger();
                info.camelot = json.value(QStringLiteral("camelot")).toString();
                tracks.push_back(info);
            }
            return tracks;
        }
        ADD_FAILURE() << "No playlist \"" << name.toStdString() << "\" on the server";
        return tracks;
    }

    /// Asks for `edges` and waits.
    PlanResults plans(crate::PlanClient* pPlans,
            const QList<PlanEdge>& edges,
            const crate::PlanContext& context) {
        QSignalSpy finished(pPlans->requestPlans(edges, context), &PlanFetch::finished);
        EXPECT_TRUE(finished.wait(kNetworkWaitMs));
        PlanResults results =
                finished.isEmpty() ? PlanResults() : finished.first().first().value<PlanResults>();
        EXPECT_EQ(static_cast<size_t>(edges.size()), results.plans.size());
        for (const crate::Result<TransitionPlan>& result : results.plans) {
            EXPECT_TRUE(result.ok()) << result.error().message.toStdString();
        }
        return results;
    }

    /// The plans of `edges` asked as another client asks them: the same
    /// context but `source`, without Decks' own fields.
    QJsonArray plansAs(const QString& source,
            const QList<PlanEdge>& edges,
            const crate::PlanContext& context) {
        QJsonObject edgeContext = context.toJson();
        edgeContext.insert(QStringLiteral("source"), source);
        edgeContext.remove(QStringLiteral("localRefinement"));
        edgeContext.remove(QStringLiteral("assist"));
        QJsonArray array;
        for (const PlanEdge& edge : edges) {
            array.append(QJsonObject{
                    {QStringLiteral("outgoingTrackEntityUid"), edge.outgoingTrackEntityUid},
                    {QStringLiteral("incomingTrackEntityUid"), edge.incomingTrackEntityUid},
                    {QStringLiteral("context"), edgeContext},
            });
        }
        const Answer answer = call(QNetworkAccessManager::PostOperation,
                kPlansPath,
                QJsonObject{{QStringLiteral("edges"), array},
                        {QStringLiteral("plannerVersion"), context.plannerVersion}});
        EXPECT_TRUE(answer.ok) << answer.error.toStdString() << " " << text(answer.object());
        return answer.object().value(QStringLiteral("plans")).toArray();
    }

    /// The user's stored corrections of `uids`, by lowercase uid; a cleared
    /// one is there with null values.
    QJsonObject storedCorrections(const QStringList& uids) {
        QJsonArray array;
        for (const QString& uid : uids) {
            array.append(uid);
        }
        const Answer answer = call(QNetworkAccessManager::PostOperation,
                kQueryPath,
                QJsonObject{{QStringLiteral("entity_uids"), array}});
        EXPECT_TRUE(answer.ok) << answer.error.toStdString();
        return answer.object().value(QStringLiteral("corrections")).toObject();
    }

    /// Whether a stored correction has a value; one not there (`missing`)
    /// has none.
    static bool hasValues(const QJsonObject& correction) {
        return correction.value(QStringLiteral("bpm")).isDouble() ||
                correction.value(QStringLiteral("beat_grid_ms")).isArray() ||
                correction.value(QStringLiteral("key")).isString();
    }

    /// TearDown's net: clears, with a PUT of nulls as "Clear correction on
    /// Crate" sends, what the test corrected and did not clear, then checks
    /// that the server has none of them.
    void clearLeftCorrections() {
        if (m_touched.isEmpty() || !m_pClient) {
            return;
        }
        const QJsonObject stored = storedCorrections(m_touched);
        int cleared = 0;
        for (const QString& uid : std::as_const(m_touched)) {
            const QJsonObject correction = stored.value(uid).toObject();
            if (!hasValues(correction)) {
                continue;
            }
            QJsonObject body = crate::writeback::toJson(Correction());
            body.insert(QStringLiteral("base_revision"),
                    correction.value(QStringLiteral("revision")).toInteger());
            const Answer answer = call(QNetworkAccessManager::PutOperation,
                    QStringLiteral("/api/decks/corrections/%1").arg(uid),
                    body);
            EXPECT_TRUE(answer.ok) << uid.toStdString() << ": " << answer.error.toStdString()
                                   << " " << text(answer.object());
            cleared += answer.ok ? 1 : 0;
        }
        const QJsonObject after = storedCorrections(m_touched);
        int left = 0;
        for (const QString& uid : std::as_const(m_touched)) {
            if (hasValues(after.value(uid).toObject())) {
                ++left;
                ADD_FAILURE() << "The correction of " << uid.toStdString()
                              << " is still on the server: " << text(after.value(uid));
            }
        }
        report("cleanup: " + std::to_string(cleared) + " left by the test cleared in TearDown; " +
                std::to_string(left) + " of " + std::to_string(m_touched.size()) +
                " corrected tracks still have a correction");
    }

    /// A Crate track as the decks have it once its file is open, with the
    /// profile's grid and key, as Crate's analysis puts them.
    TrackPointer crateTrack(const TrackInfo& info) {
        TrackPointer pTrack = Track::newTemporary(
                QStringLiteral("/crate-decks-live/%1/%2.flac").arg(m_serverId, info.uid));
        pTrack->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
                mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::Bitrate(320),
                mixxx::Duration::fromMillis(info.durationMs));
        pTrack->setTitle(info.title);
        pTrack->setArtist(QStringLiteral("Decks live check"));
        pTrack->trySetBeats(mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::FramePos(framesOf(static_cast<double>(info.beatAnchorMs))),
                mixxx::Bpm(info.bpm)));
        const auto key = crate::analysis::keyFromCamelot(info.camelot);
        if (key != mixxx::track::io::key::INVALID) {
            pTrack->setKey(key, mixxx::track::io::key::ANALYZER);
        }
        m_tracks.append(pTrack);
        return pTrack;
    }

    /// Attaches the track to CrateCorrections and waits for its query.
    void attach(const TrackPointer& pTrack, const QString& uid) {
        const auto queries = std::count(
                m_answeredPaths.cbegin(), m_answeredPaths.cend(), kQueryPath);
        m_pServers->corrections()->attach(pTrack, m_serverId, uid);
        ASSERT_TRUE(QTest::qWaitFor(
                [&] {
                    return std::count(m_answeredPaths.cbegin(),
                                   m_answeredPaths.cend(),
                                   kQueryPath) > queries;
                },
                kNetworkWaitMs));
        QTest::qWait(300);
    }

    /// Waits for the offer of the user's edit and saves it, as "Save
    /// correction to Crate" does; the outbox's answer.
    QJsonObject offerAndSave(const QString& uid, QSignalSpy* pDelivered, bool grid, bool key) {
        CrateCorrections* pCorrections = m_pServers->corrections();
        EXPECT_TRUE(QTest::qWaitFor(
                [&] {
                    return pCorrections->offer(m_serverId, uid).has_value();
                },
                kNetworkWaitMs))
                << "No offer to save the correction of " << uid.toStdString();
        const auto offer = pCorrections->offer(m_serverId, uid);
        if (!offer) {
            return QJsonObject();
        }
        EXPECT_EQ(grid, offer->grid);
        EXPECT_EQ(key, offer->key);
        const auto delivered = pDelivered->size();
        EXPECT_TRUE(pCorrections->save(m_serverId, uid));
        m_touched.append(uid);
        EXPECT_TRUE(QTest::qWaitFor(
                [&] {
                    for (auto i = delivered; i < pDelivered->size(); ++i) {
                        if (pDelivered->at(i).at(1).toString() == outboxkind::kCorrection) {
                            return true;
                        }
                    }
                    return false;
                },
                kNetworkWaitMs))
                << "The correction of " << uid.toStdString() << " did not reach the server";
        for (auto i = delivered; i < pDelivered->size(); ++i) {
            if (pDelivered->at(i).at(1).toString() == outboxkind::kCorrection) {
                return pDelivered->at(i).at(3).value<QJsonDocument>().object();
            }
        }
        return QJsonObject();
    }

    /// Decks' limit check of every candidate of `results`, which the server
    /// serves only when it has no `limit_violations`.
    void checkLimits(const PlanResults& results, int* pCandidates, int* pAgreeing) {
        for (const crate::Result<TransitionPlan>& result : results.plans) {
            if (!result.ok()) {
                continue;
            }
            EXPECT_FALSE(result.value().candidates.empty());
            for (const crate::TransitionPlanCandidate& candidate : result.value().candidates) {
                ++*pCandidates;
                const QStringList codes = candidate.plan.mixes() && candidate.plan.policyLimits
                        ? candidate.plan.limitViolations(*candidate.plan.policyLimits)
                        : QStringList();
                *pAgreeing += codes.isEmpty() ? 1 : 0;
                EXPECT_TRUE(codes.isEmpty())
                        << result.value().outgoingTrackEntityUid.toStdString() << " > "
                        << result.value().incomingTrackEntityUid.toStdString() << " candidate "
                        << candidate.index << ": "
                        << codes.join(QStringLiteral(", ")).toStdString();
            }
        }
    }

    QNetworkAccessManager m_network;
    crate::SessionOnlySecretStore m_secrets;
    std::unique_ptr<crate::CrateServers> m_pServers;
    QString m_serverId;
    crate::Client* m_pClient = nullptr;
    QStringList m_answeredPaths;
    /// The tracks the test corrected: TearDown clears what is left.
    QStringList m_touched;
    QList<TrackPointer> m_tracks;
};

TEST_F(CrateSmartMixCorrectionsLive, PlansFollowTheUsersCorrections) {
    // Auto DJ's own context: smart-mix-v3 with the candidate menu
    const QByteArray capabilities = m_pServers->lastCapabilities(m_serverId);
    crate::AutoDJSmartMixAdapter::Options options;
    options.pConfig = config();
    options.install = false;
    options.capabilitiesOf = [capabilities](const QString&) -> std::optional<crate::Capabilities> {
        const auto parsed = crate::Capabilities::fromJson(QJsonDocument::fromJson(capabilities));
        if (!parsed.ok()) {
            return std::nullopt;
        }
        return parsed.value();
    };
    options.defaultEqOn = [](const QStringList&) {
        return true;
    };
    crate::AutoDJSmartMixAdapter adapter(std::move(options));
    const crate::PlanContext context = adapter.contextFor(m_serverId);
    ASSERT_EQ(crate::PlanContext::kSmartMixV3, context.plannerVersion);
    ASSERT_TRUE(context.localRefinement);

    const std::vector<TrackInfo> tracks = playlistTracks(kPlaylist);
    ASSERT_GE(tracks.size(), 6u) << "Too few profiled tracks in " << kPlaylist.toStdString();
    QHash<QString, TrackInfo> infos;
    QList<PlanEdge> edges;
    for (size_t i = 0; i < tracks.size(); ++i) {
        infos.insert(tracks[i].uid, tracks[i]);
        if (i + 1 < tracks.size()) {
            edges.append(PlanEdge{tracks[i].uid, tracks[i + 1].uid, QString(), QString()});
        }
    }

    // The plan client as CrateServerFeature keeps it: a correction that
    // changes drops the track's plans
    crate::PlanClient planClient(m_pClient);
    CrateCorrections* pCorrections = m_pServers->corrections();
    QObject::connect(pCorrections,
            &CrateCorrections::correctionChanged,
            &planClient,
            [this, &planClient](const QString& serverId, const QString& uid) {
                if (serverId == m_serverId) {
                    planClient.invalidateTrack(uid);
                }
            });

    // 1. Every edge of the playlist without corrections
    const PlanResults survey = plans(&planClient, edges, context);
    ASSERT_EQ(static_cast<size_t>(edges.size()), survey.plans.size());
    for (const auto& result : survey.plans) {
        ASSERT_TRUE(result.ok());
    }
    const auto surveyed = [&](int i) -> const TransitionPlan& {
        return survey.plans[static_cast<size_t>(i)].value();
    };
    int candidatesBefore = 0;
    int agreeingBefore = 0;
    checkLimits(survey, &candidatesBefore, &agreeingBefore);

    // The edges of each case, each correcting its incoming track, with no
    // track in the edges of another case
    QSet<QString> used;
    const auto freeEdge = [&](int i) {
        return !used.contains(edges[i].outgoingTrackEntityUid) &&
                !used.contains(edges[i].incomingTrackEntityUid);
    };
    // A: a tempo the planner cannot match, from confident BPMs 2.5 to 8 %
    // apart; the incoming track is corrected to the outgoing's BPM
    int edgeA = -1;
    for (int i = 0; i < edges.size() && edgeA < 0; ++i) {
        const TrackInfo out = infos.value(edges[i].outgoingTrackEntityUid);
        const TrackInfo in = infos.value(edges[i].incomingTrackEntityUid);
        const double gap = std::abs(in.bpm - out.bpm) / in.bpm;
        if (surveyed(i).fallbackReason == TransitionPlan::FallbackReason::IncompatibleTempo &&
                out.bpmConfidence >= 0.9 && in.bpmConfidence >= 0.9 && gap > 0.025 &&
                gap < 0.08) {
            edgeA = i;
        }
    }
    ASSERT_GE(edgeA, 0) << "No edge with an incompatible tempo 2.5 to 8 % apart";
    // And the next edge, where the corrected track goes out
    const int edgeANext = edgeA + 1 < edges.size() ? edgeA + 1 : -1;
    for (const int i : {edgeA, edgeANext}) {
        if (i >= 0) {
            used.insert(edges[i].outgoingTrackEntityUid);
            used.insert(edges[i].incomingTrackEntityUid);
        }
    }
    // B: a beatmatch into a fully profiled track whose phrases make the menu
    int edgeB = -1;
    for (int i = 0; i < edges.size() && edgeB < 0; ++i) {
        const TrackInfo in = infos.value(edges[i].incomingTrackEntityUid);
        const auto& menu = surveyed(i).candidates;
        if (freeEdge(i) && surveyed(i).mode == TransitionPlan::Mode::Beatmatch &&
                in.quality == QLatin1String("full") && in.bpmConfidence >= 0.95 &&
                std::any_of(menu.cbegin(), menu.cend(), [](const auto& candidate) {
                    return candidate.incomingPhrase.has_value();
                })) {
            edgeB = i;
        }
    }
    ASSERT_GE(edgeB, 0) << "No beatmatch into a full profile with phrases";
    used.insert(edges[edgeB].outgoingTrackEntityUid);
    used.insert(edges[edgeB].incomingTrackEntityUid);
    // C: a beatmatch between two different keys that score low
    int edgeC = -1;
    for (int i = 0; i < edges.size() && edgeC < 0; ++i) {
        const TrackInfo out = infos.value(edges[i].outgoingTrackEntityUid);
        const TrackInfo in = infos.value(edges[i].incomingTrackEntityUid);
        if (freeEdge(i) && surveyed(i).mode == TransitionPlan::Mode::Beatmatch &&
                !out.camelot.isEmpty() && !in.camelot.isEmpty() && out.camelot != in.camelot &&
                maxHarmonic(surveyed(i)) < 0.3) {
            edgeC = i;
        }
    }
    ASSERT_GE(edgeC, 0) << "No beatmatch between keys that score low";

    QList<int> chosen{edgeA};
    if (edgeANext >= 0) {
        chosen.append(edgeANext);
    }
    chosen.append(edgeB);
    chosen.append(edgeC);
    QList<PlanEdge> chosenEdges;
    for (const int i : std::as_const(chosen)) {
        chosenEdges.append(edges[i]);
        report("edge " + std::to_string(i) + " " +
                infos.value(edges[i].outgoingTrackEntityUid).title.toStdString() + " > " +
                infos.value(edges[i].incomingTrackEntityUid).title.toStdString() +
                " before: " + describe(surveyed(i)));
    }
    // An edge none of the corrections touch
    const TrackInfo trackA = infos.value(edges[edgeA].incomingTrackEntityUid);
    const TrackInfo trackB = infos.value(edges[edgeB].incomingTrackEntityUid);
    const TrackInfo trackC = infos.value(edges[edgeC].incomingTrackEntityUid);
    int untouched = -1;
    for (int i = 0; i < edges.size() && untouched < 0; ++i) {
        const QSet<QString> uids{edges[i].outgoingTrackEntityUid, edges[i].incomingTrackEntityUid};
        if (!uids.contains(trackA.uid) && !uids.contains(trackB.uid) &&
                !uids.contains(trackC.uid)) {
            untouched = i;
        }
    }

    // How Listen gets the same edges
    const QJsonArray listenBefore = plansAs(kListenSource, chosenEdges, context);
    ASSERT_EQ(chosenEdges.size(), listenBefore.size());

    // 2. The corrections, saved from Decks. The user has none of them yet
    const QJsonObject original = storedCorrections({trackA.uid, trackB.uid, trackC.uid});
    for (const QString& uid : {trackA.uid, trackB.uid, trackC.uid}) {
        ASSERT_FALSE(hasValues(original.value(uid).toObject()))
                << "The user has a correction of " << uid.toStdString()
                << " already: " << text(original.value(uid));
    }
    QSignalSpy delivered(m_pServers->outbox(), &Outbox::delivered);

    // A: the BPM and the grid, the BPM more than 2 % away (the outgoing's)
    const double bpmA = infos.value(edges[edgeA].outgoingTrackEntityUid).bpm;
    const TrackPointer pTrackA = crateTrack(trackA);
    ASSERT_NO_FATAL_FAILURE(attach(pTrackA, trackA.uid));
    pTrackA->setBpmLocked(false);
    ASSERT_TRUE(pTrackA->trySetBeats(mixxx::Beats::fromConstTempo(
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::FramePos(framesOf(static_cast<double>(trackA.beatAnchorMs))),
            mixxx::Bpm(bpmA))));
    const QJsonObject savedA = offerAndSave(trackA.uid, &delivered, true, false);
    ASSERT_FALSE(savedA.isEmpty());

    // B: the grid within 2 %, 0.5 % faster and 200 ms later
    const double bpmB = trackB.bpm * kWithinFactor;
    const TrackPointer pTrackB = crateTrack(trackB);
    ASSERT_NO_FATAL_FAILURE(attach(pTrackB, trackB.uid));
    pTrackB->setBpmLocked(false);
    ASSERT_TRUE(pTrackB->trySetBeats(
            mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kSampleRate),
                    mixxx::audio::FramePos(framesOf(
                            static_cast<double>(trackB.beatAnchorMs + kWithinShiftMs))),
                    mixxx::Bpm(bpmB))));
    const QJsonObject savedB = offerAndSave(trackB.uid, &delivered, true, false);
    ASSERT_FALSE(savedB.isEmpty());

    // C: the key alone, the outgoing track's
    const QString keyC = infos.value(edges[edgeC].outgoingTrackEntityUid).camelot;
    const TrackPointer pTrackC = crateTrack(trackC);
    ASSERT_NO_FATAL_FAILURE(attach(pTrackC, trackC.uid));
    pTrackC->setKey(crate::analysis::keyFromCamelot(keyC), mixxx::track::io::key::USER);
    const QJsonObject savedC = offerAndSave(trackC.uid, &delivered, false, true);
    ASSERT_FALSE(savedC.isEmpty());

    // What the server keeps
    const QJsonObject stored = storedCorrections({trackA.uid, trackB.uid, trackC.uid});
    const QJsonObject storedA = stored.value(trackA.uid).toObject();
    const QJsonObject storedB = stored.value(trackB.uid).toObject();
    const QJsonObject storedC = stored.value(trackC.uid).toObject();
    for (const QJsonObject& correction : {storedA, storedB, storedC}) {
        QJsonObject shown = correction;
        const QJsonArray grid = shown.take(QStringLiteral("beat_grid_ms")).toArray();
        report("stored " + text(shown) + " with " + std::to_string(grid.size()) +
                " beats from " + std::to_string(grid.isEmpty() ? 0 : grid.first().toInteger()) +
                " ms");
    }
    const double storedBpmA = storedA.value(QStringLiteral("bpm")).toDouble();
    const double storedBpmB = storedB.value(QStringLiteral("bpm")).toDouble();
    EXPECT_NEAR(bpmA, storedBpmA, 0.01);
    EXPECT_NEAR(bpmB, storedBpmB, 0.01);
    EXPECT_GT(std::abs(storedBpmA - trackA.bpm) / trackA.bpm, 0.02);
    EXPECT_LT(std::abs(storedBpmB - trackB.bpm) / trackB.bpm, 0.02);
    EXPECT_EQ(QStringLiteral("delta-ms-v1"),
            storedA.value(QStringLiteral("beat_grid_format")).toString());
    EXPECT_EQ(keyC, storedC.value(QStringLiteral("key")).toString());
    EXPECT_TRUE(storedC.value(QStringLiteral("bpm")).isNull());
    EXPECT_TRUE(storedC.value(QStringLiteral("beat_grid_ms")).isNull());
    QSet<qint64> gridB;
    const QJsonArray storedGridB = storedB.value(QStringLiteral("beat_grid_ms")).toArray();
    for (const QJsonValue& beat : storedGridB) {
        gridB.insert(beat.toInteger());
    }
    ASSERT_GT(gridB.size(), 16);

    // 3. Decks dropped the plans of the corrected tracks, and only those
    for (const PlanEdge& edge : std::as_const(chosenEdges)) {
        EXPECT_FALSE(planClient.cachedPlan(edge, context).has_value())
                << edge.outgoingTrackEntityUid.toStdString() << " > "
                << edge.incomingTrackEntityUid.toStdString();
    }
    if (untouched >= 0) {
        EXPECT_TRUE(planClient.cachedPlan(edges[untouched], context).has_value());
    }
    const PlanResults corrected = plans(&planClient, chosenEdges, context);
    ASSERT_EQ(static_cast<size_t>(chosenEdges.size()), corrected.plans.size());
    for (const auto& result : corrected.plans) {
        ASSERT_TRUE(result.ok());
    }
    int candidatesAfter = 0;
    int agreeingAfter = 0;
    checkLimits(corrected, &candidatesAfter, &agreeingAfter);
    const auto after = [&](int i) -> const TransitionPlan& {
        return corrected.plans[static_cast<size_t>(chosen.indexOf(i))].value();
    };
    for (const int i : std::as_const(chosen)) {
        report("edge " + std::to_string(i) + " after:  " + describe(after(i)));
    }

    // A, into the corrected track: the tempos match now, at the corrected
    // BPM, and its phrases are gone (the bar heuristic)
    {
        const TransitionPlan& plan = after(edgeA);
        const TrackInfo out = infos.value(edges[edgeA].outgoingTrackEntityUid);
        EXPECT_FALSE(plan.fallbackReason == TransitionPlan::FallbackReason::IncompatibleTempo);
        EXPECT_TRUE(plan.mode == TransitionPlan::Mode::Beatmatch);
        if (plan.mode == TransitionPlan::Mode::Beatmatch) {
            EXPECT_NEAR(out.outroBpm / storedBpmA, plan.incomingTempoRatio, 5e-4);
        }
        for (const auto& candidate : plan.candidates) {
            EXPECT_FALSE(candidate.incomingPhrase.has_value())
                    << "Candidate " << candidate.index << " mixes into phrase "
                    << *candidate.incomingPhrase << " of a track whose BPM moved more than 2 %";
        }
    }
    // A, out of the corrected track: no phrases there either
    if (edgeANext >= 0) {
        const TransitionPlan& plan = after(edgeANext);
        const TrackInfo in = infos.value(edges[edgeANext].incomingTrackEntityUid);
        if (plan.mode == TransitionPlan::Mode::Beatmatch) {
            EXPECT_NEAR(storedBpmA / in.introBpm, plan.incomingTempoRatio, 5e-4);
        }
        for (const auto& candidate : plan.candidates) {
            EXPECT_FALSE(candidate.outgoingPhrase.has_value())
                    << "Candidate " << candidate.index << " mixes out of phrase "
                    << *candidate.outgoingPhrase << " of a track whose BPM moved more than 2 %";
        }
    }
    // B: the phrases stay, on the corrected downbeats
    {
        const TransitionPlan& before = surveyed(edgeB);
        const TransitionPlan& plan = after(edgeB);
        const TrackInfo out = infos.value(edges[edgeB].outgoingTrackEntityUid);
        if (plan.mode == TransitionPlan::Mode::Beatmatch) {
            EXPECT_NEAR(out.outroBpm / storedBpmB, plan.incomingTempoRatio, 5e-4);
        }
        EXPECT_GT(std::abs(plan.incomingTempoRatio - before.incomingTempoRatio), 1e-3);
        int phrased = 0;
        int onGrid = 0;
        for (const auto& candidate : plan.candidates) {
            if (!candidate.incomingPhrase) {
                continue;
            }
            ++phrased;
            onGrid += gridB.contains(candidate.plan.incomingCueMs) ? 1 : 0;
            EXPECT_TRUE(gridB.contains(candidate.plan.incomingCueMs))
                    << "Candidate " << candidate.index << " enters phrase "
                    << *candidate.incomingPhrase << " at " << candidate.plan.incomingCueMs
                    << " ms, off the corrected grid";
        }
        EXPECT_GT(phrased, 0) << "The phrases of a grid corrected within 2 % were dropped";
        int movedBefore = 0;
        int phrasedBefore = 0;
        for (const auto& candidate : before.candidates) {
            if (candidate.incomingPhrase) {
                ++phrasedBefore;
                movedBefore += gridB.contains(candidate.plan.incomingCueMs) ? 0 : 1;
            }
        }
        EXPECT_GT(movedBefore, 0) << "The uncorrected cues were on the corrected grid already";
        report("B: " + std::to_string(onGrid) + " of " + std::to_string(phrased) +
                " phrase entries on the corrected grid (before: " +
                std::to_string(phrasedBefore - movedBefore) + " of " +
                std::to_string(phrasedBefore) + ")");
    }
    // C: the key changes the harmonic term, not the tempo or the phrases
    {
        const TransitionPlan& before = surveyed(edgeC);
        const TransitionPlan& plan = after(edgeC);
        EXPECT_GT(maxHarmonic(plan), maxHarmonic(before) + 0.1);
        EXPECT_TRUE(before.mode == plan.mode);
        EXPECT_TRUE(before.fallbackReason == plan.fallbackReason);
        EXPECT_NEAR(before.incomingTempoRatio, plan.incomingTempoRatio, 1e-6);
        const auto phrased = [](const TransitionPlan& menu) {
            return std::count_if(menu.candidates.cbegin(),
                    menu.candidates.cend(),
                    [](const auto& candidate) {
                        return candidate.incomingPhrase.has_value();
                    });
        };
        EXPECT_EQ(phrased(before), phrased(plan));
        std::ostringstream line;
        line << "C: best harmonic " << maxHarmonic(before) << " -> " << maxHarmonic(plan);
        report(line.str());
    }

    report("Decks' limit check agrees with the server on " + std::to_string(agreeingBefore) +
            " of " + std::to_string(candidatesBefore) + " candidates of " +
            std::to_string(edges.size()) + " uncorrected edges, and on " +
            std::to_string(agreeingAfter) + " of " + std::to_string(candidatesAfter) +
            " candidates of " + std::to_string(chosenEdges.size()) + " corrected edges");
    EXPECT_EQ(candidatesBefore, agreeingBefore);
    EXPECT_EQ(candidatesAfter, agreeingAfter);

    // 4. Listen's plans of the same edges do not change
    const QJsonArray listenAfter = plansAs(kListenSource, chosenEdges, context);
    ASSERT_EQ(listenBefore.size(), listenAfter.size());
    int sameForListen = 0;
    for (int i = 0; i < listenBefore.size(); ++i) {
        const bool same = listenBefore[i].toObject() == listenAfter[i].toObject();
        sameForListen += same ? 1 : 0;
        EXPECT_TRUE(same) << "source \"" << kListenSource.toStdString()
                          << "\" got another plan with the user's correction:\n before "
                          << text(listenBefore[i]) << "\n after  " << text(listenAfter[i]);
    }
    report("source \"" + kListenSource.toStdString() + "\": " + std::to_string(sameForListen) +
            " of " + std::to_string(listenBefore.size()) +
            " plans as before the corrections");

    // 5. "Clear correction on Crate" for each: the shared plans again
    for (const QString& uid : {trackA.uid, trackB.uid, trackC.uid}) {
        const auto deliveredBefore = delivered.size();
        ASSERT_TRUE(pCorrections->clear(m_serverId, uid)) << uid.toStdString();
        EXPECT_TRUE(QTest::qWaitFor(
                [&] {
                    return delivered.size() > deliveredBefore;
                },
                kNetworkWaitMs))
                << "The clear of " << uid.toStdString() << " did not reach the server";
    }
    const QJsonObject clearedStore = storedCorrections({trackA.uid, trackB.uid, trackC.uid});
    for (const QString& uid : {trackA.uid, trackB.uid, trackC.uid}) {
        EXPECT_FALSE(hasValues(clearedStore.value(uid).toObject()))
                << text(clearedStore.value(uid));
    }
    for (const PlanEdge& edge : std::as_const(chosenEdges)) {
        EXPECT_FALSE(planClient.cachedPlan(edge, context).has_value());
    }
    const PlanResults shared = plans(&planClient, chosenEdges, context);
    int asBefore = 0;
    for (int k = 0; k < chosen.size(); ++k) {
        const auto& result = shared.plans[static_cast<size_t>(k)];
        if (!result.ok()) {
            continue;
        }
        const TransitionPlan& before = surveyed(chosen[k]);
        const TransitionPlan& plan = result.value();
        const bool same = plan.mode == before.mode &&
                plan.fallbackReason == before.fallbackReason &&
                plan.outgoingCueMs == before.outgoingCueMs &&
                plan.incomingCueMs == before.incomingCueMs &&
                plan.incomingTempoRatio == before.incomingTempoRatio &&
                plan.candidates.size() == before.candidates.size();
        asBefore += same ? 1 : 0;
        EXPECT_TRUE(same) << "edge " << chosen[k] << " after the clear: " << describe(plan)
                          << "\n before: " << describe(before);
    }
    report("cleared from Decks: " + std::to_string(asBefore) + " of " +
            std::to_string(chosen.size()) + " plans as before the corrections");
}

} // namespace
