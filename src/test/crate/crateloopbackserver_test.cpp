#include "crate/session/crateloopbackserver.h"

#include <gtest/gtest.h>

#include <QSignalSpy>

#include "test/crate/fakecratenetwork.h"

namespace {

using crate::LoopbackCallbackServer;

using crate::test::sendHttpRequest;

TEST(CrateLoopbackServerTest, CallbackServerBindsLoopbackOnly) {
    LoopbackCallbackServer server;

    ASSERT_TRUE(server.listen());

    EXPECT_EQ(QHostAddress(QHostAddress::LocalHost), server.address());
    EXPECT_NE(0, server.port());
    EXPECT_EQ(QUrl(QStringLiteral("http://127.0.0.1:%1/callback").arg(server.port())),
            server.callbackUrl());
}

TEST(CrateLoopbackServerTest, AnswersOneCallbackAndStops) {
    LoopbackCallbackServer server;
    ASSERT_TRUE(server.listen());
    QSignalSpy received(&server, &LoopbackCallbackServer::callbackReceived);

    const QByteArray answer = sendHttpRequest(server.port(),
            "GET /callback?code=one-time-code&state=the-state HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n\r\n");

    EXPECT_TRUE(answer.startsWith("HTTP/1.1 200"));
    EXPECT_TRUE(answer.contains("Crate Decks"));
    ASSERT_EQ(1, received.count());
    const auto query = received.first().first().value<QUrlQuery>();
    EXPECT_EQ(QStringLiteral("one-time-code"), query.queryItemValue(QStringLiteral("code")));
    EXPECT_EQ(QStringLiteral("the-state"), query.queryItemValue(QStringLiteral("state")));
    EXPECT_FALSE(server.isListening());
}

TEST(CrateLoopbackServerTest, AnswersEvenWhenDeletedRightAfterTheCallback) {
    auto* pServer = new LoopbackCallbackServer();
    ASSERT_TRUE(pServer->listen());
    QObject::connect(pServer,
            &LoopbackCallbackServer::callbackReceived,
            pServer,
            &QObject::deleteLater);
    const QPointer<LoopbackCallbackServer> pDeleted(pServer);

    const QByteArray answer = sendHttpRequest(pServer->port(),
            "GET /callback?code=one-time-code&state=the-state HTTP/1.1\r\n\r\n");

    EXPECT_TRUE(answer.startsWith("HTTP/1.1 200"));
    EXPECT_TRUE(pDeleted.isNull());
}

TEST(CrateLoopbackServerTest, IgnoresCallbacksWithAnotherState) {
    LoopbackCallbackServer server;
    server.setExpectedState(QStringLiteral("the-state"));
    ASSERT_TRUE(server.listen());
    QSignalSpy received(&server, &LoopbackCallbackServer::callbackReceived);

    const QByteArray forged = sendHttpRequest(server.port(),
            "GET /callback?state=forged&error=cancelled HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(forged.startsWith("HTTP/1.1 400"));
    EXPECT_EQ(0, received.count());
    EXPECT_TRUE(server.isListening());
    const QByteArray real = sendHttpRequest(server.port(),
            "GET /callback?code=one-time-code&state=the-state HTTP/1.1\r\n\r\n");

    EXPECT_TRUE(real.startsWith("HTTP/1.1 200"));
    EXPECT_EQ(1, received.count());
    EXPECT_FALSE(server.isListening());
}

TEST(CrateLoopbackServerTest, KeepsWaitingAfterOtherRequests) {
    LoopbackCallbackServer server;
    ASSERT_TRUE(server.listen());
    QSignalSpy received(&server, &LoopbackCallbackServer::callbackReceived);

    const QByteArray favicon = sendHttpRequest(server.port(), "GET /favicon.ico HTTP/1.1\r\n\r\n");
    const QByteArray post = sendHttpRequest(server.port(), "POST /callback HTTP/1.1\r\n\r\n");

    EXPECT_TRUE(favicon.startsWith("HTTP/1.1 404"));
    EXPECT_TRUE(post.startsWith("HTTP/1.1 405"));
    EXPECT_EQ(0, received.count());
    EXPECT_TRUE(server.isListening());
}

} // namespace
