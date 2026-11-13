#include "crate/client/crateclient.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEvent>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <memory>
#include <optional>

#include "config.h"
#include "crate/appidentity.h"
#include "test/crate/fakecratenetwork.h"

namespace {

using namespace std::chrono_literals;
using crate::Client;
using crate::ClientOptions;
using crate::Error;
using crate::JsonResult;
using crate::Reply;
using crate::RequestOptions;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeReply;
using crate::test::FakeScheduler;
using crate::test::FakeTokenSource;

const QByteArray kOkBody = QByteArrayLiteral(R"({"ok": true})");

/// What a Reply finished with, if it did.
struct Outcome {
    std::optional<JsonResult> result;

    explicit Outcome(Reply* pReply) {
        QObject::connect(pReply, &Reply::finished, pReply, [this](const JsonResult& finished) {
            result = finished;
        });
    }

    bool finished() const {
        return result.has_value();
    }
};

class CrateClientTest : public testing::Test {
  protected:
    std::unique_ptr<Client> makeClient(const QString& fingerprint = QStringLiteral("install-1")) {
        ClientOptions options;
        options.deviceFingerprint = fingerprint;
        options.schedule = m_scheduler.function();
        options.random = [] { return 0.5; };
        auto pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test/base")), &m_network, std::move(options));
        pClient->setTokenSource(&m_tokens);
        return pClient;
    }

    FakeReply* lastReply() const {
        return m_network.last().pReply.data();
    }

    FakeNetworkAccessManager m_network;
    FakeScheduler m_scheduler;
    FakeTokenSource m_tokens;
};

TEST_F(CrateClientTest, AddsAuthorizationAndAppHeaders) {
    const auto pClient = makeClient();
    RequestOptions search;
    search.query.addQueryItem(QStringLiteral("q"), QStringLiteral("techno"));
    RequestOptions anonymous;
    anonymous.authenticated = false;

    pClient->get(QStringLiteral("/api/search"), search);
    pClient->post(QStringLiteral("/api/auth/login"),
            QJsonDocument(QJsonObject{{QStringLiteral("email"), QStringLiteral("dj@crate.test")}}),
            anonymous);

    ASSERT_EQ(2, m_network.sent.size());
    const QByteArray appHeader = QByteArray("decks/") + crate::kApplicationVersion;
    const QNetworkRequest& searchRequest = m_network.sent[0].request;
    EXPECT_EQ(
            QUrl(QStringLiteral("https://crate.test/base/api/search?q=techno")),
            searchRequest.url());
    EXPECT_EQ(QByteArrayLiteral("Bearer access-1"), searchRequest.rawHeader("Authorization"));
    EXPECT_EQ(appHeader, searchRequest.rawHeader("X-Crate-App"));
    EXPECT_EQ(QByteArrayLiteral("install-1"), searchRequest.rawHeader("X-Device-Fingerprint"));
    EXPECT_EQ(QByteArrayLiteral("application/json"), searchRequest.rawHeader("Accept"));
    EXPECT_FALSE(searchRequest.url().toString().contains(QStringLiteral("access-1")));

    const FakeNetworkAccessManager::Sent& login = m_network.sent[1];
    EXPECT_EQ(QNetworkAccessManager::PostOperation, login.operation);
    EXPECT_FALSE(login.request.hasRawHeader("Authorization"));
    EXPECT_EQ(appHeader, login.request.rawHeader("X-Crate-App"));
    EXPECT_EQ(QByteArrayLiteral("application/json"),
            login.request.header(QNetworkRequest::ContentTypeHeader).toByteArray());
    EXPECT_EQ(QByteArrayLiteral(R"({"email":"dj@crate.test"})"), login.body);
}

TEST_F(CrateClientTest, SendsTheSameDeviceFingerprintAcrossRestarts) {
    QTemporaryDir installation;
    QTemporaryDir otherInstallation;
    ASSERT_TRUE(installation.isValid());
    ASSERT_TRUE(otherInstallation.isValid());
    const QString settingsFile = installation.filePath(QStringLiteral(MIXXX_SETTINGS_FILE));

    UserSettingsPointer pFirstRun(new UserSettings(settingsFile));
    const QString fingerprint = crate::deviceFingerprint(pFirstRun);
    ASSERT_TRUE(pFirstRun->save());
    UserSettingsPointer pRestart(new UserSettings(settingsFile));
    const QString fingerprintAfterRestart = crate::deviceFingerprint(pRestart);
    UserSettingsPointer pOther(
            new UserSettings(otherInstallation.filePath(QStringLiteral(MIXXX_SETTINGS_FILE))));

    EXPECT_FALSE(fingerprint.isEmpty());
    EXPECT_EQ(fingerprint, fingerprintAfterRestart);
    EXPECT_NE(fingerprint, crate::deviceFingerprint(pOther));
    const auto pClient = makeClient(fingerprintAfterRestart);
    pClient->get(QStringLiteral("/api/auth/me"));
    EXPECT_EQ(fingerprint.toUtf8(), lastReply()->request().rawHeader("X-Device-Fingerprint"));
}

TEST_F(CrateClientTest, KeepsRedirectsOnTheSameOriginAndTimesOut) {
    const auto pClient = makeClient();

    pClient->get(QStringLiteral("/api/capabilities"));

    const QNetworkRequest& request = lastReply()->request();
    EXPECT_EQ(QNetworkRequest::SameOriginRedirectPolicy,
            request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt());
    EXPECT_EQ(30000, request.transferTimeout());
}

TEST_F(CrateClientTest, RetriesIdempotentOn503WithBackoff) {
    const auto pClient = makeClient();
    Outcome outcome(pClient->get(QStringLiteral("/api/capabilities")));

    lastReply()->respond(503);
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(500ms, m_scheduler.tasks[0].delay);
    EXPECT_FALSE(outcome.finished());
    m_scheduler.runAll();
    ASSERT_EQ(2, m_network.sent.size());
    lastReply()->respond(503);
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(1000ms, m_scheduler.tasks[0].delay);
    m_scheduler.runAll();
    ASSERT_EQ(3, m_network.sent.size());
    lastReply()->respond(503);

    EXPECT_TRUE(m_scheduler.tasks.isEmpty());
    ASSERT_TRUE(outcome.finished());
    ASSERT_FALSE(outcome.result->ok());
    EXPECT_EQ(Error::Kind::Http, outcome.result->error().kind);
    EXPECT_EQ(503, outcome.result->error().httpStatus);
}

TEST_F(CrateClientTest, DoesNotRetryPost) {
    const auto pClient = makeClient();
    Outcome outcome(pClient->post(QStringLiteral("/api/playlists"), QJsonDocument(QJsonObject())));

    lastReply()->respond(503);

    EXPECT_TRUE(m_scheduler.tasks.isEmpty());
    EXPECT_EQ(1, m_network.sent.size());
    ASSERT_TRUE(outcome.finished());
    EXPECT_EQ(503, outcome.result->error().httpStatus);
}

TEST_F(CrateClientTest, HonoursRetryAfter) {
    const auto pClient = makeClient();
    Outcome shortWait(pClient->get(QStringLiteral("/api/a")));
    Outcome longWait(pClient->get(QStringLiteral("/api/b")));

    m_network.sent[0].pReply->respond(429, QByteArray(), {{"Retry-After", "2"}});
    m_network.sent[1].pReply->respond(503, QByteArray(), {{"Retry-After", "120"}});

    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(2000ms, m_scheduler.tasks[0].delay);
    EXPECT_FALSE(shortWait.finished());
    ASSERT_TRUE(longWait.finished());
    EXPECT_EQ(503, longWait.result->error().httpStatus);
}

TEST_F(CrateClientTest, RefreshesOnceOn401AndReplaysQueuedRequests) {
    const auto pClient = makeClient();
    Outcome first(pClient->get(QStringLiteral("/api/a")));
    Outcome second(pClient->get(QStringLiteral("/api/b")));
    ASSERT_EQ(2, m_network.sent.size());

    m_network.sent[0].pReply->respond(401);
    m_network.sent[1].pReply->respond(401);
    Outcome third(pClient->get(QStringLiteral("/api/c")));

    EXPECT_EQ(1, m_tokens.renewals);
    EXPECT_EQ(2, m_network.sent.size());
    m_tokens.finishRenewal(true, QStringLiteral("access-2"));

    ASSERT_EQ(5, m_network.sent.size());
    const QStringList paths = {QStringLiteral("/base/api/a"),
            QStringLiteral("/base/api/b"),
            QStringLiteral("/base/api/c")};
    for (int i = 0; i < 3; ++i) {
        const QNetworkRequest& replayed = m_network.sent[2 + i].request;
        EXPECT_EQ(paths[i], replayed.url().path());
        EXPECT_EQ(QByteArrayLiteral("Bearer access-2"), replayed.rawHeader("Authorization"));
    }
    const QList<FakeReply*> openReplies = m_network.openReplies();
    for (FakeReply* pReply : openReplies) {
        pReply->respond(200, kOkBody);
    }
    ASSERT_TRUE(first.finished() && second.finished() && third.finished());
    EXPECT_TRUE(first.result->ok());
    EXPECT_TRUE(second.result->ok());
    EXPECT_TRUE(third.result->ok());
}

TEST_F(CrateClientTest, FailsWithAuthErrorWhenRefreshFails) {
    const auto pClient = makeClient();
    Outcome outcome(pClient->get(QStringLiteral("/api/a")));

    lastReply()->respond(401);
    m_tokens.finishRenewal(false);

    EXPECT_EQ(1, m_network.sent.size());
    ASSERT_TRUE(outcome.finished());
    EXPECT_EQ(Error::Kind::Auth, outcome.result->error().kind);
}

TEST_F(CrateClientTest, RejectedRenewedTokenFailsWithAuthError) {
    const auto pClient = makeClient();
    Outcome outcome(pClient->get(QStringLiteral("/api/a")));

    lastReply()->respond(401);
    m_tokens.finishRenewal(true, QStringLiteral("access-2"));
    ASSERT_EQ(2, m_network.sent.size());
    lastReply()->respond(401);

    EXPECT_EQ(1, m_tokens.renewals);
    ASSERT_TRUE(outcome.finished());
    EXPECT_EQ(Error::Kind::Auth, outcome.result->error().kind);
}

TEST_F(CrateClientTest, UnauthenticatedRequestReports401AsHttp) {
    const auto pClient = makeClient();
    RequestOptions anonymous;
    anonymous.authenticated = false;
    Outcome outcome(pClient->post(
            QStringLiteral("/api/auth/login"), QJsonDocument(QJsonObject()), anonymous));

    lastReply()->respond(401, QByteArrayLiteral(R"({"detail": "Invalid credentials"})"));

    EXPECT_EQ(0, m_tokens.renewals);
    ASSERT_TRUE(outcome.finished());
    EXPECT_EQ(Error::Kind::Http, outcome.result->error().kind);
    EXPECT_EQ(401, outcome.result->error().httpStatus);
    EXPECT_EQ(QStringLiteral("Invalid credentials"), outcome.result->error().message);
}

TEST_F(CrateClientTest, DeduplicatesIdenticalInFlightGets) {
    const auto pClient = makeClient();
    RequestOptions secondPage;
    secondPage.query.addQueryItem(QStringLiteral("page"), QStringLiteral("2"));

    Outcome first(pClient->get(QStringLiteral("/api/capabilities")));
    Outcome second(pClient->get(QStringLiteral("/api/capabilities")));
    Outcome otherQuery(pClient->get(QStringLiteral("/api/capabilities"), secondPage));
    ASSERT_EQ(2, m_network.sent.size());
    m_network.sent[0].pReply->respond(200, kOkBody);

    ASSERT_TRUE(first.finished() && second.finished());
    EXPECT_TRUE(first.result->value().object().value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(second.result->value().object().value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(otherQuery.finished());
    pClient->get(QStringLiteral("/api/capabilities"));
    EXPECT_EQ(3, m_network.sent.size());
}

TEST_F(CrateClientTest, DroppingOneSharedReplyKeepsTheRequest) {
    const auto pClient = makeClient();
    Reply* pDropped = pClient->get(QStringLiteral("/api/capabilities"));
    Outcome kept(pClient->get(QStringLiteral("/api/capabilities")));

    delete pDropped;
    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_FALSE(lastReply()->aborted());
    lastReply()->respond(200, kOkBody);

    ASSERT_TRUE(kept.finished());
    EXPECT_TRUE(kept.result->ok());
}

TEST_F(CrateClientTest, CancelsOnDestruction) {
    auto pClient = makeClient();
    Reply* pDropped = pClient->get(QStringLiteral("/api/a"));
    Reply* pCancelled = pClient->get(QStringLiteral("/api/b"));
    Outcome cancelled(pCancelled);
    pClient->get(QStringLiteral("/api/c"));
    ASSERT_EQ(3, m_network.sent.size());

    delete pDropped;
    EXPECT_TRUE(m_network.sent[0].pReply->aborted());
    pCancelled->cancel();
    EXPECT_TRUE(m_network.sent[1].pReply->aborted());
    ASSERT_TRUE(cancelled.finished());
    EXPECT_EQ(Error::Kind::Cancelled, cancelled.result->error().kind);
    pClient.reset();
    EXPECT_TRUE(m_network.sent[2].pReply->aborted());
}

TEST_F(CrateClientTest, LimitsConcurrentRequestsPerServer) {
    const auto pClient = makeClient();

    for (int i = 0; i < 6; ++i) {
        pClient->get(QStringLiteral("/api/items/%1").arg(i));
    }
    EXPECT_EQ(4, m_network.sent.size());
    m_network.sent[0].pReply->respond(200, kOkBody);
    EXPECT_EQ(5, m_network.sent.size());
    m_network.sent[1].pReply->failNetwork();

    ASSERT_EQ(6, m_network.sent.size());
    EXPECT_EQ(QStringLiteral("/base/api/items/5"), m_network.sent[5].request.url().path());
}

TEST_F(CrateClientTest, ReportsTheServerErrorDetail) {
    const auto pClient = makeClient();
    Outcome text(pClient->get(QStringLiteral("/api/a")));
    Outcome coded(pClient->get(QStringLiteral("/api/b")));
    Outcome validation(pClient->get(QStringLiteral("/api/c")));
    Outcome empty(pClient->get(QStringLiteral("/api/d")));

    m_network.sent[0].pReply->respond(403,
            QByteArrayLiteral(R"({"detail": "User account is disabled"})"));
    m_network.sent[1].pReply->respond(400,
            QByteArrayLiteral(
                    R"({"detail": {"code": "invalid_cursor", "message": "Cursor expired"}})"));
    m_network.sent[2].pReply->respond(422,
            QByteArrayLiteral(R"({"detail": [{"loc": ["query", "q"], "msg": "Field required"}]})"));
    m_network.sent[3].pReply->respond(404);

    ASSERT_TRUE(text.finished() && coded.finished() && validation.finished() && empty.finished());
    EXPECT_EQ(403, text.result->error().httpStatus);
    EXPECT_EQ(QStringLiteral("User account is disabled"), text.result->error().message);
    EXPECT_EQ(QStringLiteral("invalid_cursor"), coded.result->error().code);
    EXPECT_EQ(QStringLiteral("Cursor expired"), coded.result->error().message);
    EXPECT_EQ(QStringLiteral("Field required"), validation.result->error().message);
    EXPECT_EQ(404, empty.result->error().httpStatus);
    EXPECT_FALSE(empty.result->error().message.isEmpty());
}

TEST_F(CrateClientTest, AnswerThatIsNotJsonIsADecodeError) {
    const auto pClient = makeClient();
    Outcome html(pClient->get(QStringLiteral("/api/a")));
    Outcome noContent(pClient->del(QStringLiteral("/api/b")));

    m_network.sent[0].pReply->respond(200, QByteArrayLiteral("<html>"));
    m_network.sent[1].pReply->respond(204);

    ASSERT_TRUE(html.finished() && noContent.finished());
    EXPECT_EQ(Error::Kind::Decode, html.result->error().kind);
    ASSERT_TRUE(noContent.result->ok());
    EXPECT_TRUE(noContent.result->value().isNull());
}

TEST_F(CrateClientTest, UnreachableServerIsANetworkError) {
    const auto pClient = makeClient();
    Outcome outcome(pClient->get(QStringLiteral("/api/capabilities")));

    lastReply()->failNetwork();

    EXPECT_TRUE(m_scheduler.tasks.isEmpty());
    ASSERT_TRUE(outcome.finished());
    EXPECT_EQ(Error::Kind::Network, outcome.result->error().kind);
}

TEST_F(CrateClientTest, DeletingTheClientInsideFinishedIsSafe) {
    auto pClient = makeClient();
    Reply* pFirst = pClient->get(QStringLiteral("/api/capabilities"));
    const QPointer<Reply> pSecond = pClient->get(QStringLiteral("/api/capabilities"));
    bool secondFinished = false;
    QObject::connect(pFirst, &Reply::finished, pFirst, [pOwner = &pClient](const JsonResult&) {
        pOwner->reset();
    });
    QObject::connect(pSecond,
            &Reply::finished,
            pSecond,
            [pSecondFinished = &secondFinished](const JsonResult&) { *pSecondFinished = true; });

    lastReply()->respond(200, kOkBody);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    EXPECT_EQ(nullptr, pClient);
    EXPECT_TRUE(pSecond.isNull());
    EXPECT_FALSE(secondFinished);
}

TEST_F(CrateClientTest, DeletingTheReplyInsideFinishedIsSafe) {
    const auto pClient = makeClient();
    Reply* pReply = pClient->get(QStringLiteral("/api/capabilities"));
    bool finished = false;
    QObject::connect(pReply,
            &Reply::finished,
            pReply,
            [pReply, pFinished = &finished](const JsonResult&) {
                *pFinished = true;
                delete pReply;
            });

    lastReply()->respond(200, kOkBody);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    EXPECT_TRUE(finished);
    pClient->get(QStringLiteral("/api/next"));
    EXPECT_EQ(2, m_network.sent.size());
}

TEST_F(CrateClientTest, CancellingFromProgressIsSafe) {
    const auto pClient = makeClient();
    Reply* pCancelled = pClient->get(QStringLiteral("/api/capabilities"));
    Reply* pKept = pClient->get(QStringLiteral("/api/capabilities"));
    Outcome cancelled(pCancelled);
    Outcome kept(pKept);
    bool keptProgress = false;
    QObject::connect(pCancelled, &Reply::progress, pCancelled, [pCancelled](qint64, qint64) {
        pCancelled->cancel();
    });
    QObject::connect(pKept,
            &Reply::progress,
            pKept,
            [pKeptProgress = &keptProgress](
                    qint64, qint64) { *pKeptProgress = true; });

    lastReply()->respond(200, kOkBody);

    ASSERT_TRUE(cancelled.finished() && kept.finished());
    EXPECT_EQ(Error::Kind::Cancelled, cancelled.result->error().kind);
    EXPECT_TRUE(keptProgress);
    EXPECT_TRUE(kept.result->ok());
}

TEST_F(CrateClientTest, TruncatedSuccessIsANetworkError) {
    const auto pClient = makeClient();
    Outcome empty(pClient->del(QStringLiteral("/api/playlists/7")));
    Outcome partial(pClient->get(QStringLiteral("/api/a")));

    m_network.sent[0].pReply->respondTruncated(
            200, QByteArray(), QNetworkReply::RemoteHostClosedError);
    m_network.sent[1].pReply->respondTruncated(
            200, QByteArrayLiteral(R"({"ok": tr)"), QNetworkReply::RemoteHostClosedError);

    ASSERT_TRUE(empty.finished() && partial.finished());
    EXPECT_EQ(Error::Kind::Network, empty.result->error().kind);
    EXPECT_EQ(Error::Kind::Network, partial.result->error().kind);
}

TEST_F(CrateClientTest, ChangingTheTokenSourceDuringARenewalFailsTheWaiters) {
    const auto pClient = makeClient();
    FakeTokenSource newSession;
    newSession.token = QStringLiteral("other-1");
    Outcome waiting(pClient->get(QStringLiteral("/api/a")));
    lastReply()->respond(401);
    ASSERT_TRUE(m_tokens.renewing());

    pClient->setTokenSource(&newSession);

    ASSERT_TRUE(waiting.finished());
    EXPECT_EQ(Error::Kind::Auth, waiting.result->error().kind);
    Outcome next(pClient->get(QStringLiteral("/api/b")));
    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_EQ(QByteArrayLiteral("Bearer other-1"),
            lastReply()->request().rawHeader("Authorization"));
    lastReply()->respond(401);
    EXPECT_EQ(1, newSession.renewals);
    m_tokens.finishRenewal(false);
    EXPECT_FALSE(next.finished());
    newSession.finishRenewal(true, QStringLiteral("other-2"));
    ASSERT_EQ(3, m_network.sent.size());
    EXPECT_EQ(QByteArrayLiteral("Bearer other-2"),
            lastReply()->request().rawHeader("Authorization"));
}

TEST_F(CrateClientTest, FailedRenewalIsNotRetriedByOtherRequests) {
    const auto pClient = makeClient();
    Outcome first(pClient->get(QStringLiteral("/api/a")));
    Outcome second(pClient->get(QStringLiteral("/api/b")));
    Outcome third(pClient->get(QStringLiteral("/api/c")));

    m_network.sent[0].pReply->respond(401);
    m_tokens.finishRenewal(false);
    m_network.sent[1].pReply->respond(401);
    m_network.sent[2].pReply->respond(401);

    EXPECT_EQ(1, m_tokens.renewals);
    ASSERT_TRUE(first.finished() && second.finished() && third.finished());
    EXPECT_EQ(Error::Kind::Auth, second.result->error().kind);
    EXPECT_EQ(Error::Kind::Auth, third.result->error().kind);
}

TEST_F(CrateClientTest, UnreachableRenewalFailsAsNetworkAndKeepsTheSession) {
    const auto pClient = makeClient();
    Outcome offline(pClient->get(QStringLiteral("/api/a")));

    lastReply()->respond(401);
    m_tokens.finishRenewalUnreachable();
    ASSERT_TRUE(offline.finished());
    EXPECT_EQ(Error::Kind::Network, offline.result->error().kind);
    Outcome later(pClient->get(QStringLiteral("/api/b")));
    lastReply()->respond(401);

    EXPECT_EQ(2, m_tokens.renewals);
    m_tokens.finishRenewal(true, QStringLiteral("access-2"));
    EXPECT_EQ(QByteArrayLiteral("Bearer access-2"),
            lastReply()->request().rawHeader("Authorization"));
}

TEST_F(CrateClientTest, ExplicitBearerTokenIsSentAndNeverRenewed) {
    const auto pClient = makeClient();
    RequestOptions revoke;
    revoke.bearerToken = QStringLiteral("closing-session");
    Outcome outcome(pClient->post(
            QStringLiteral("/api/auth/logout"), QJsonDocument(QJsonObject()), revoke));

    EXPECT_EQ(QByteArrayLiteral("Bearer closing-session"),
            lastReply()->request().rawHeader("Authorization"));
    lastReply()->respond(401);

    EXPECT_EQ(0, m_tokens.renewals);
    ASSERT_TRUE(outcome.finished());
    EXPECT_EQ(Error::Kind::Auth, outcome.result->error().kind);
}

TEST_F(CrateClientTest, ForwardsDownloadProgress) {
    const auto pClient = makeClient();
    Reply* pReply = pClient->get(QStringLiteral("/api/capabilities"));
    QSignalSpy progress(pReply, &Reply::progress);

    lastReply()->respond(200, kOkBody);

    ASSERT_EQ(1, progress.count());
    EXPECT_EQ(kOkBody.size(), progress.first().at(0).toLongLong());
    EXPECT_EQ(kOkBody.size(), progress.first().at(1).toLongLong());
}

TEST_F(CrateClientTest, SendsExtraHeadersButNeverAnotherAuthorization) {
    const auto pClient = makeClient();
    RequestOptions conditional;
    conditional.headers = {{QByteArrayLiteral("If-None-Match"), QByteArrayLiteral("\"v7\"")},
            {QByteArrayLiteral("authorization"), QByteArrayLiteral("Bearer stolen")}};

    pClient->get(QStringLiteral("/api/decks/containers"), conditional);

    const QNetworkRequest& request = m_network.last().request;
    EXPECT_EQ(QByteArrayLiteral("\"v7\""), request.rawHeader("If-None-Match"));
    EXPECT_EQ(QByteArrayLiteral("Bearer access-1"), request.rawHeader("Authorization"));
}

TEST_F(CrateClientTest, NotModifiedFinishesWithStatusAndHeaders) {
    const auto pClient = makeClient();
    Reply* pReply = pClient->get(QStringLiteral("/api/decks/containers"));
    std::optional<int> status;
    QByteArray etag;
    QObject::connect(pReply,
            &Reply::finished,
            pReply,
            [pReply, pStatus = &status, pEtag = &etag](const JsonResult& result) {
                EXPECT_TRUE(result.ok());
                *pStatus = pReply->httpStatus();
                *pEtag = pReply->rawHeader(QByteArrayLiteral("ETag"));
            });

    lastReply()->respond(304,
            QByteArray(),
            {{QByteArrayLiteral("ETag"), QByteArrayLiteral("\"v7\"")}});

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(304, *status);
    EXPECT_EQ(QByteArrayLiteral("\"v7\""), etag);
}

TEST_F(CrateClientTest, RawBodyIsLeftUnparsed) {
    const auto pClient = makeClient();
    RequestOptions raw;
    raw.rawBody = true;
    Reply* pReply = pClient->get(QStringLiteral("/api/decks/containers"), raw);
    // An identical GET that wants JSON is a different request
    Reply* pParsed = pClient->get(QStringLiteral("/api/decks/containers"));
    Outcome parsed(pParsed);
    QByteArray body;
    bool documentIsNull = false;
    QObject::connect(pReply,
            &Reply::finished,
            pReply,
            [pReply, pBody = &body, pDocumentIsNull = &documentIsNull](const JsonResult& result) {
                ASSERT_TRUE(result.ok());
                *pDocumentIsNull = result.value().isNull();
                *pBody = pReply->body();
            });

    ASSERT_EQ(2, m_network.sent.size());
    m_network.sent[0].pReply->respond(200, kOkBody);
    m_network.sent[1].pReply->respond(200, kOkBody);

    EXPECT_TRUE(documentIsNull);
    EXPECT_EQ(kOkBody, body);
    ASSERT_TRUE(parsed.finished());
    EXPECT_TRUE(parsed.result->value().object().value(QStringLiteral("ok")).toBool());
}

TEST_F(CrateClientTest, OpensAnEventStreamWithTheSessionHeaders) {
    const auto pClient = makeClient();
    RequestOptions resume;
    resume.headers = {{QByteArrayLiteral("Last-Event-ID"), QByteArrayLiteral("42")}};

    std::unique_ptr<QNetworkReply> pStream(
            pClient->openStream(QStringLiteral("/api/cache/events"), resume));

    ASSERT_NE(nullptr, pStream);
    const QNetworkRequest& request = m_network.last().request;
    EXPECT_EQ(QUrl(QStringLiteral("https://crate.test/base/api/cache/events")), request.url());
    EXPECT_EQ(QByteArrayLiteral("Bearer access-1"), request.rawHeader("Authorization"));
    EXPECT_EQ(QByteArrayLiteral("text/event-stream"), request.rawHeader("Accept"));
    EXPECT_EQ(QByteArrayLiteral("42"), request.rawHeader("Last-Event-ID"));
    // A stream stays open: no transfer timeout, and it takes no request slot
    EXPECT_EQ(0, request.transferTimeout());
    RequestOptions plain;
    for (int i = 0; i < 4; ++i) {
        pClient->get(QStringLiteral("/api/item/%1").arg(i), plain);
    }
    EXPECT_EQ(5, m_network.sent.size());
}

TEST_F(CrateClientTest, StreamsTheBodyAsItArrives) {
    const auto pClient = makeClient();
    RequestOptions stream;
    stream.streamBody = true;
    stream.headers = {{QByteArrayLiteral("Range"), QByteArrayLiteral("bytes=4-")}};
    Reply* pReply = pClient->get(QStringLiteral("/api/tracks/by-entity/t1/stream"), stream);
    Outcome outcome(pReply);
    int statusAtHeaders = 0;
    QByteArray etagAtHeaders;
    QByteArray received;
    QObject::connect(pReply,
            &Reply::headersReceived,
            pReply,
            [pReply, pStatus = &statusAtHeaders, pEtag = &etagAtHeaders] {
                *pStatus = pReply->httpStatus();
                *pEtag = pReply->rawHeader("ETag");
            });
    QObject::connect(pReply,
            &Reply::bodyReceived,
            pReply,
            [pStatus = &statusAtHeaders, pReceived = &received](const QByteArray& chunk) {
                EXPECT_EQ(206, *pStatus);
                *pReceived += chunk;
            });

    const QNetworkRequest& request = m_network.last().request;
    EXPECT_EQ(QByteArrayLiteral("Bearer access-1"), request.rawHeader("Authorization"));
    EXPECT_EQ(QByteArrayLiteral("bytes=4-"), request.rawHeader("Range"));
    EXPECT_EQ(QByteArrayLiteral("*/*"), request.rawHeader("Accept"));
    FakeReply* pFake = lastReply();
    pFake->respondHeaders(206, {{QByteArrayLiteral("ETag"), QByteArrayLiteral("\"e1\"")}});
    pFake->sendBody(QByteArrayLiteral("4567"));
    EXPECT_EQ(QByteArrayLiteral("4567"), received);
    // Nothing is kept: what was handed over is gone from the network reply
    EXPECT_EQ(0, pFake->bytesAvailable());
    pFake->sendBody(QByteArrayLiteral("89"));
    pFake->finishBody();

    EXPECT_EQ(206, statusAtHeaders);
    EXPECT_EQ(QByteArrayLiteral("\"e1\""), etagAtHeaders);
    EXPECT_EQ(QByteArrayLiteral("456789"), received);
    ASSERT_TRUE(outcome.finished());
    ASSERT_TRUE(outcome.result->ok());
    EXPECT_TRUE(outcome.result->value().isNull());
}

TEST_F(CrateClientTest, StreamsAreNeitherSharedNorLimited) {
    const auto pClient = makeClient();
    for (int i = 0; i < 4; ++i) {
        pClient->get(QStringLiteral("/api/items/%1").arg(i));
    }
    ASSERT_EQ(4, m_network.sent.size());
    RequestOptions stream;
    stream.streamBody = true;

    pClient->get(QStringLiteral("/api/tracks/by-entity/t1/stream"), stream);
    pClient->get(QStringLiteral("/api/tracks/by-entity/t1/stream"), stream);

    // Two downloads of their own, although the four API slots are taken
    EXPECT_EQ(6, m_network.sent.size());
    m_network.sent[4].pReply->respond(200, QByteArrayLiteral("audio"));
    // ...and a finished stream frees no API slot
    EXPECT_EQ(6, m_network.sent.size());
    pClient->get(QStringLiteral("/api/items/4"));
    EXPECT_EQ(6, m_network.sent.size());
}

TEST_F(CrateClientTest, StreamRenewsTheTokenAndStreamsNoErrorBody) {
    const auto pClient = makeClient();
    RequestOptions stream;
    stream.streamBody = true;
    Reply* pReply = pClient->get(QStringLiteral("/api/tracks/by-entity/t1/stream"), stream);
    Outcome outcome(pReply);
    QByteArray received;
    int headers = 0;
    QObject::connect(pReply, &Reply::headersReceived, pReply, [pHeaders = &headers] {
        ++*pHeaders;
    });
    QObject::connect(pReply,
            &Reply::bodyReceived,
            pReply,
            [pReceived = &received](const QByteArray& chunk) {
                *pReceived += chunk;
            });

    lastReply()->respond(401, QByteArrayLiteral(R"({"detail": "expired"})"));
    m_tokens.finishRenewal(true, QStringLiteral("access-2"));
    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_EQ(QByteArrayLiteral("Bearer access-2"),
            lastReply()->request().rawHeader("Authorization"));
    lastReply()->respond(200, QByteArray());

    EXPECT_TRUE(received.isEmpty());
    // An empty body still tells its headers
    EXPECT_EQ(1, headers);
    ASSERT_TRUE(outcome.finished());
    EXPECT_TRUE(outcome.result->ok());
}

TEST_F(CrateClientTest, CancellingAStreamFromItsBodyIsSafe) {
    const auto pClient = makeClient();
    RequestOptions stream;
    stream.streamBody = true;
    Reply* pReply = pClient->get(QStringLiteral("/api/tracks/by-entity/t1/stream"), stream);
    Outcome outcome(pReply);
    QObject::connect(pReply, &Reply::bodyReceived, pReply, [pReply] { pReply->cancel(); });
    FakeReply* pFake = lastReply();

    pFake->respondHeaders(200);
    pFake->sendBody(QByteArrayLiteral("01"));

    EXPECT_TRUE(pFake->aborted());
    ASSERT_TRUE(outcome.finished());
    EXPECT_EQ(Error::Kind::Cancelled, outcome.result->error().kind);
}

} // namespace
