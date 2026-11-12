#include "crate/analysis/profilestore.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTest>
#include <QUrlQuery>
#include <memory>

#include "crate/client/cratecapabilities.h"
#include "crate/db/crateserverschema.h"
#include "crate/library/cratelibrarystore.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"

namespace {

using crate::Client;
using crate::ClientOptions;
using crate::analysis::AppliedCues;
using crate::analysis::ProfileFetch;
using crate::analysis::ProfileResults;
using crate::analysis::ProfileStore;
using crate::library::RemoteTrack;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeTokenSource;

const QString kServer = QStringLiteral("srv-1");
// The uids of the fixtures (src/test/crate/data/mix-profiles/).
const QString kConstant = QStringLiteral("0b6f2c1e-5d6a-4c1b-9a51-2f0c8a3c9e10");
const QString kDrifting = QStringLiteral("7d1e9b44-3f2a-4e8e-8c0d-61b5a2f4d7aa");
const QString kPartial = QStringLiteral("3c2a7f10-9e4b-4d21-b6f8-0a5e1c7d9b32");
const QString kMissing = QStringLiteral("e5b8d3a6-1f7c-4a90-8d2e-6b4c0f9a1e57");

const QString kBatchPath = QStringLiteral("/api/decks/mix-profiles");

QString singlePath(const QString& uid) {
    return QStringLiteral("/api/tracks/by-entity/%1/mix-profile").arg(uid);
}

class ProfileStoreTest : public MixxxDbTest {
  protected:
    ProfileStoreTest()
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
        // The catalogue as the library sync left it: each track with the
        // revision of its current profile.
        catalogue(kConstant, revisionOf(QStringLiteral("full-constant-124.json")));
        catalogue(kDrifting, revisionOf(QStringLiteral("full-drifting-128.json")));
        catalogue(kPartial, revisionOf(QStringLiteral("partial.json")));
        catalogue(kMissing, QStringLiteral("gone"));
    }

    void TearDown() override {
        m_pStore.reset();
        m_pClient.reset();
    }

    ProfileStore* makeStore(bool batch, ProfileStore::Options options = ProfileStore::Options()) {
        m_pStore = std::make_unique<ProfileStore>(
                kServer, m_pClient.get(), dbConnectionPooler(), std::move(options));
        m_pStore->setBatchAvailable(batch);
        return m_pStore.get();
    }

    QByteArray fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/mix-profiles/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return file.readAll();
    }

    QString revisionOf(const QString& name) const {
        return QJsonDocument::fromJson(fixture(name))
                .object()
                .value(QStringLiteral("profileRevision"))
                .toString();
    }

    void catalogue(const QString& uid, const QString& profileRevision) {
        RemoteTrack track;
        track.entityUid = uid;
        track.title = QStringLiteral("Track");
        track.profileRevision = profileRevision;
        track.profileQuality = profileRevision.isEmpty() ? QString() : QStringLiteral("full");
        ASSERT_EQ(crate::library::WriteStatus::Ok,
                crate::library::storeTracks(dbConnection(),
                        kServer,
                        {track},
                        QStringLiteral("2026-11-11T00:00:00.000Z")));
    }

    /// Runs the event loop until `count` requests went out.
    bool waitForRequests(int count) {
        return QTest::qWaitFor(
                [&] {
                    return m_network.sent.size() >= count;
                },
                5000);
    }

    static ProfileResults resultOf(QSignalSpy& spy) {
        if (spy.isEmpty() && !spy.wait(5000)) {
            ADD_FAILURE() << "The fetch did not finish";
            return ProfileResults();
        }
        return spy.first().first().value<ProfileResults>();
    }

    int storedRows() const {
        QSqlQuery query(dbConnection());
        query.exec(QStringLiteral("SELECT COUNT(*) FROM crateserver_profiles"));
        return query.next() ? query.value(0).toInt() : -1;
    }

    /// The uid of the profile of `quality` in a batch answer's profiles.
    static QString uidOfQuality(const QJsonObject& profiles, const QString& quality) {
        for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it) {
            if (it.value().toObject().value(QStringLiteral("quality")).toString() == quality) {
                return it.key();
            }
        }
        return QString();
    }

    static QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
        return QJsonDocument::fromJson(sent.body).object();
    }

    FakeNetworkAccessManager m_network;
    FakeTokenSource m_tokens;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<ProfileStore> m_pStore;
};

TEST_F(ProfileStoreTest, UsesBatchWhenAvailable) {
    ProfileStore* pStore = makeStore(true);
    QSignalSpy finished(pStore->get({kConstant, kPartial.toUpper(), kMissing}),
            &ProfileFetch::finished);

    ASSERT_TRUE(waitForRequests(1));
    const auto sent = m_network.last();
    EXPECT_EQ(QNetworkAccessManager::PostOperation, sent.operation);
    EXPECT_EQ(kBatchPath, sent.request.url().path());
    const QJsonObject body = bodyOf(sent);
    EXPECT_EQ(QStringLiteral("full"), body.value(QStringLiteral("detail")).toString());
    EXPECT_EQ((QJsonArray{kConstant, kPartial, kMissing}),
            body.value(QStringLiteral("entity_uids")).toArray());
    sent.pReply->respond(200, fixture(QStringLiteral("batch-response.json")));

    const ProfileResults results = resultOf(finished);
    EXPECT_EQ(2, results.profiles.size());
    ASSERT_TRUE(results.profiles.contains(kConstant));
    EXPECT_TRUE(results.profiles.value(kConstant).profile.isFull());
    EXPECT_EQ(413, results.profiles.value(kConstant).profile.beatGridMs.size());
    ASSERT_TRUE(results.profiles.contains(kPartial));
    EXPECT_FALSE(results.profiles.value(kPartial).profile.isFull());
    EXPECT_EQ(QStringList{kMissing}, results.missing);
    EXPECT_TRUE(results.failed.isEmpty());
    EXPECT_EQ(1, m_network.sent.size());
    EXPECT_EQ(2, storedRows());
}

// The S4 answer of the dev stack, anonymised (real-batch-response.json, from
// export_real_mix_profiles.py): a full profile, a partial one and a uid the
// server does not know.
TEST_F(ProfileStoreTest, ParsesRealBatchAnswer) {
    const QJsonObject answer = QJsonDocument::fromJson(fixture(QStringLiteral(
                                                               "real-batch-response.json")))
                                       .object();
    const QJsonObject profiles = answer.value(QStringLiteral("profiles")).toObject();
    const QJsonArray missing = answer.value(QStringLiteral("missing")).toArray();
    ASSERT_EQ(2, profiles.size());
    ASSERT_EQ(1, missing.size());
    const QString full = uidOfQuality(profiles, QStringLiteral("full"));
    const QString partial = uidOfQuality(profiles, QStringLiteral("partial"));
    const QString unknown = missing.first().toString();
    ASSERT_FALSE(full.isEmpty());
    ASSERT_FALSE(partial.isEmpty());
    for (const QString& uid : {full, partial}) {
        catalogue(uid,
                profiles.value(uid)
                        .toObject()
                        .value(QStringLiteral("profileRevision"))
                        .toString());
    }
    catalogue(unknown, QStringLiteral("gone"));

    ProfileStore* pStore = makeStore(true);
    QSignalSpy finished(pStore->get({full, partial, unknown}), &ProfileFetch::finished);
    ASSERT_TRUE(waitForRequests(1));
    EXPECT_EQ(kBatchPath, m_network.last().request.url().path());
    m_network.last().pReply->respond(200, fixture(QStringLiteral("real-batch-response.json")));

    const ProfileResults results = resultOf(finished);
    EXPECT_TRUE(results.failed.isEmpty());
    EXPECT_EQ(QStringList{unknown}, results.missing);
    ASSERT_TRUE(results.profiles.contains(full));
    const auto& fullProfile = results.profiles.value(full).profile;
    EXPECT_TRUE(fullProfile.isFull());
    EXPECT_EQ(QStringLiteral("smart-mix-audio-v10"), fullProfile.analyzerVersion);
    EXPECT_EQ(profiles.value(full)
                      .toObject()
                      .value(QStringLiteral("beatGridMs"))
                      .toArray()
                      .size(),
            fullProfile.beatGridMs.size());
    EXPECT_TRUE(fullProfile.bpm.has_value());
    EXPECT_FALSE(fullProfile.camelot.isEmpty());
    EXPECT_TRUE(fullProfile.integratedLufs.has_value());
    ASSERT_TRUE(results.profiles.contains(partial));
    EXPECT_EQ(crate::analysis::MixProfile::Quality::Partial,
            results.profiles.value(partial).profile.quality);
    EXPECT_EQ(2, storedRows());

    // The batch serves the same profile as the single endpoint.
    const QJsonObject single =
            QJsonDocument::fromJson(fixture(QStringLiteral("real-full-whole-bpm.json"))).object();
    EXPECT_EQ(single, profiles.value(full).toObject());
}

TEST_F(ProfileStoreTest, UsesSingleRequestsWithoutBatch) {
    ProfileStore::Options options;
    options.maxConcurrentRequests = 2;
    ProfileStore* pStore = makeStore(false, options);
    QSignalSpy finished(pStore->get({kConstant, kDrifting, kMissing}), &ProfileFetch::finished);

    // Two at a time.
    ASSERT_TRUE(waitForRequests(2));
    QTest::qWait(20);
    ASSERT_EQ(2, m_network.sent.size());
    for (const auto& sent : std::as_const(m_network.sent)) {
        EXPECT_EQ(QNetworkAccessManager::GetOperation, sent.operation);
        EXPECT_EQ(QStringLiteral("full"),
                QUrlQuery(sent.request.url()).queryItemValue(QStringLiteral("detail")));
    }
    EXPECT_EQ(singlePath(kConstant), m_network.sent[0].request.url().path());
    EXPECT_EQ(singlePath(kDrifting), m_network.sent[1].request.url().path());
    m_network.sent[0].pReply->respond(200, fixture(QStringLiteral("full-constant-124.json")));

    ASSERT_TRUE(waitForRequests(3));
    EXPECT_EQ(singlePath(kMissing), m_network.sent[2].request.url().path());
    m_network.sent[1].pReply->respond(200, fixture(QStringLiteral("full-drifting-128.json")));
    m_network.sent[2].pReply->respond(404, QByteArrayLiteral(R"({"detail": "Not found"})"));

    const ProfileResults results = resultOf(finished);
    EXPECT_EQ(2, results.profiles.size());
    EXPECT_TRUE(results.profiles.contains(kConstant));
    EXPECT_TRUE(results.profiles.contains(kDrifting));
    EXPECT_EQ(QStringList{kMissing}, results.missing);
    EXPECT_TRUE(results.failed.isEmpty());
}

TEST_F(ProfileStoreTest, ListsMissingProfiles) {
    // The catalogue knows the track has no profile: no request.
    catalogue(kMissing, QString());
    ProfileStore* pStore = makeStore(true);
    QSignalSpy finished(pStore->get({kMissing}), &ProfileFetch::finished);

    const ProfileResults results = resultOf(finished);
    EXPECT_EQ(QStringList{kMissing}, results.missing);
    EXPECT_TRUE(results.profiles.isEmpty());
    EXPECT_TRUE(m_network.sent.isEmpty());

    // A uid the batch answer leaves out altogether is a failure, not a
    // missing profile.
    QSignalSpy second(pStore->get({kConstant, kDrifting}), &ProfileFetch::finished);
    ASSERT_TRUE(waitForRequests(1));
    QJsonObject answer;
    answer.insert(QStringLiteral("profiles"),
            QJsonObject{{kConstant,
                    QJsonDocument::fromJson(fixture(QStringLiteral("full-constant-124.json")))
                            .object()}});
    answer.insert(QStringLiteral("missing"), QJsonArray());
    m_network.last().pReply->respond(200, QJsonDocument(answer).toJson());
    const ProfileResults secondResults = resultOf(second);
    EXPECT_TRUE(secondResults.profiles.contains(kConstant));
    EXPECT_TRUE(secondResults.missing.isEmpty());
    EXPECT_TRUE(secondResults.failed.contains(kDrifting));
}

TEST_F(ProfileStoreTest, ServesCurrentProfilesFromTheStore) {
    ProfileStore* pStore = makeStore(true);
    QSignalSpy first(pStore->get({kConstant}), &ProfileFetch::finished);
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(200, fixture(QStringLiteral("batch-response.json")));
    ASSERT_TRUE(resultOf(first).profiles.contains(kConstant));

    QSignalSpy second(pStore->get({kConstant, kPartial}), &ProfileFetch::finished);
    const ProfileResults results = resultOf(second);
    EXPECT_TRUE(results.profiles.contains(kConstant));
    EXPECT_TRUE(results.profiles.contains(kPartial));
    EXPECT_EQ(1, m_network.sent.size());
}

TEST_F(ProfileStoreTest, InvalidatesByProfileRevision) {
    ProfileStore* pStore = makeStore(false);
    QSignalSpy first(pStore->get({kConstant}), &ProfileFetch::finished);
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(200, fixture(QStringLiteral("full-constant-124.json")));
    ASSERT_TRUE(resultOf(first).profiles.contains(kConstant));

    AppliedCues applied;
    applied.profileRevision = revisionOf(QStringLiteral("full-constant-124.json"));
    applied.intro = AppliedCues::Range{475.0, 31964.0};
    pStore->recordAppliedCues(kConstant, applied);

    // The library sync saw a new profile of the track.
    catalogue(kConstant, QStringLiteral("rev-2"));
    QSignalSpy second(pStore->get({kConstant}), &ProfileFetch::finished);
    ASSERT_TRUE(waitForRequests(2));
    QJsonObject newer =
            QJsonDocument::fromJson(fixture(QStringLiteral("full-constant-124.json"))).object();
    newer.insert(QStringLiteral("profileRevision"), QStringLiteral("rev-2"));
    m_network.last().pReply->respond(200, QJsonDocument(newer).toJson());

    const ProfileResults results = resultOf(second);
    ASSERT_TRUE(results.profiles.contains(kConstant));
    const auto stored = results.profiles.value(kConstant);
    EXPECT_EQ(QStringLiteral("rev-2"), stored.profile.profileRevision);
    // The cues set from the old revision stay known, so the new one can
    // replace them.
    ASSERT_TRUE(stored.appliedCues.has_value());
    EXPECT_EQ(applied, *stored.appliedCues);

    // Current again: no more requests.
    QSignalSpy third(pStore->get({kConstant}), &ProfileFetch::finished);
    EXPECT_TRUE(resultOf(third).profiles.contains(kConstant));
    EXPECT_EQ(2, m_network.sent.size());
}

TEST_F(ProfileStoreTest, SplitsBatchesOf200) {
    QStringList uids;
    for (int i = 0; i < 450; ++i) {
        const QString uid = QStringLiteral("00000000-0000-4000-8000-%1")
                                    .arg(i, 12, 10, QLatin1Char('0'));
        catalogue(uid, QStringLiteral("r"));
        uids.append(uid);
    }
    ProfileStore* pStore = makeStore(true);
    QSignalSpy finished(pStore->get(uids), &ProfileFetch::finished);

    QList<int> sizes;
    for (int batch = 1; batch <= 3; ++batch) {
        ASSERT_TRUE(waitForRequests(batch));
        const auto sent = m_network.last();
        const QJsonArray requested = bodyOf(sent).value(QStringLiteral("entity_uids")).toArray();
        sizes.append(requested.size());
        QJsonObject answer;
        answer.insert(QStringLiteral("profiles"), QJsonObject());
        answer.insert(QStringLiteral("missing"), requested);
        sent.pReply->respond(200, QJsonDocument(answer).toJson());
    }

    EXPECT_EQ((QList<int>{200, 200, 50}), sizes);
    EXPECT_EQ(450, resultOf(finished).missing.size());
}

TEST_F(ProfileStoreTest, FallsBackToSingleRequestsWhenBatchIsGone) {
    ProfileStore* pStore = makeStore(true);
    QSignalSpy finished(pStore->get({kConstant}), &ProfileFetch::finished);
    ASSERT_TRUE(waitForRequests(1));
    m_network.last().pReply->respond(404, QByteArrayLiteral(R"({"detail": "Not Found"})"));

    ASSERT_TRUE(waitForRequests(2));
    EXPECT_EQ(singlePath(kConstant), m_network.last().request.url().path());
    m_network.last().pReply->respond(200, fixture(QStringLiteral("full-constant-124.json")));
    EXPECT_TRUE(resultOf(finished).profiles.contains(kConstant));
    EXPECT_FALSE(pStore->batchAvailable());
}

TEST_F(ProfileStoreTest, ReportsFailures) {
    ProfileStore* pStore = makeStore(false);
    QSignalSpy finished(pStore->get({kConstant, kDrifting}), &ProfileFetch::finished);
    ASSERT_TRUE(waitForRequests(2));
    m_network.sent[0].pReply->respond(200, QByteArrayLiteral(R"({"trackEntityUid": 3})"));
    m_network.sent[1].pReply->failNetwork();

    const ProfileResults results = resultOf(finished);
    EXPECT_TRUE(results.profiles.isEmpty());
    ASSERT_TRUE(results.failed.contains(kConstant));
    EXPECT_EQ(crate::Error::Kind::Decode, results.failed.value(kConstant).kind);
    ASSERT_TRUE(results.failed.contains(kDrifting));
    EXPECT_EQ(0, storedRows());
}

TEST_F(ProfileStoreTest, ReadsBatchFeatureFromCapabilities) {
    ProfileStore* pStore = makeStore(false);
    const auto capabilities = crate::Capabilities::fromJson(QJsonDocument::fromJson(
            R"({"decks": {"available": true, "min_client_version": "0.1.0",
                "contract_version": 1, "features": ["library_cursor", "profiles_batch"]}})"));
    ASSERT_TRUE(capabilities.ok());
    pStore->setCapabilities(capabilities.value());
    EXPECT_TRUE(pStore->batchAvailable());
}

TEST(ProfileStoreAppliedCuesTest, RoundTripsJson) {
    AppliedCues applied;
    applied.profileRevision = QStringLiteral("rev-1");
    applied.intro = AppliedCues::Range{475.0, 31964.0};
    applied.outro = AppliedCues::Range{165802.0, std::nullopt};
    const auto back = crate::analysis::profiles::appliedCuesFromJson(
            crate::analysis::profiles::appliedCuesToJson(applied));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(applied, *back);
    EXPECT_FALSE(crate::analysis::profiles::appliedCuesFromJson(QByteArray()).has_value());
}

} // namespace
