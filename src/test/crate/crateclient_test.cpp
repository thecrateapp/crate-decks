#include "crate/client/crateclient.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEvent>
#include <QJsonObject>
#include <QTemporaryDir>
#include <memory>
#include <optional>

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
        QObject::connect(pReply, &Reply::finished, [this](const JsonResult& finished) {
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
        return m_network.sent.last().pReply.data();
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
    const QString settingsFile = installation.filePath(QStringLiteral("mixxx.cfg"));

    UserSettingsPointer pFirstRun(new UserSettings(settingsFile));
    const QString fingerprint = crate::deviceFingerprint(pFirstRun);
    ASSERT_TRUE(pFirstRun->save());
    UserSettingsPointer pRestart(new UserSettings(settingsFile));
    const QString fingerprintAfterRestart = crate::deviceFingerprint(pRestart);
    UserSettingsPointer pOther(
            new UserSettings(otherInstallation.filePath(QStringLiteral("mixxx.cfg"))));

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
    for (FakeReply* pReply : m_network.openReplies()) {
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
    QObject::connect(pFirst, &Reply::finished, [&pClient](const JsonResult&) {
        pClient.reset();
    });
    QObject::connect(pSecond, &Reply::finished, [&secondFinished](const JsonResult&) {
        secondFinished = true;
    });

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
    QObject::connect(pReply, &Reply::finished, [pReply, &finished](const JsonResult&) {
        finished = true;
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
    QObject::connect(pCancelled, &Reply::progress, [pCancelled](qint64, qint64) {
        pCancelled->cancel();
    });
    QObject::connect(pKept, &Reply::progress, [&keptProgress](qint64, qint64) {
        keptProgress = true;
    });

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

TEST_F(CrateClientTest, ForwardsDownloadProgress) {
    const auto pClient = makeClient();
    Reply* pReply = pClient->get(QStringLiteral("/api/capabilities"));
    qint64 received = -1;
    qint64 total = -1;
    QObject::connect(pReply, &Reply::progress, [&](qint64 bytesReceived, qint64 bytesTotal) {
        received = bytesReceived;
        total = bytesTotal;
    });

    lastReply()->respond(200, kOkBody);

    EXPECT_EQ(kOkBody.size(), received);
    EXPECT_EQ(kOkBody.size(), total);
}

} // namespace
