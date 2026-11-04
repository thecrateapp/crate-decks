#include "crate/session/crateloopbackserver.h"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTcpSocket>

namespace {

using crate::LoopbackCallbackServer;

/// Sends `request` to the server on 127.0.0.1:`port` and returns the whole
/// answer, running the event loop meanwhile so that the server can reply.
QByteArray exchange(quint16 port, const QByteArray& request) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    QSignalSpy connected(&socket, &QTcpSocket::connected);
    if (socket.state() != QAbstractSocket::ConnectedState && !connected.wait(5000)) {
        return QByteArray();
    }
    socket.write(request);
    QSignalSpy disconnected(&socket, &QTcpSocket::disconnected);
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        disconnected.wait(5000);
    }
    return socket.readAll();
}

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

    const QByteArray answer = exchange(server.port(),
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

TEST(CrateLoopbackServerTest, KeepsWaitingAfterOtherRequests) {
    LoopbackCallbackServer server;
    ASSERT_TRUE(server.listen());
    QSignalSpy received(&server, &LoopbackCallbackServer::callbackReceived);

    const QByteArray favicon = exchange(server.port(), "GET /favicon.ico HTTP/1.1\r\n\r\n");
    const QByteArray post = exchange(server.port(), "POST /callback HTTP/1.1\r\n\r\n");

    EXPECT_TRUE(favicon.startsWith("HTTP/1.1 404"));
    EXPECT_TRUE(post.startsWith("HTTP/1.1 405"));
    EXPECT_EQ(0, received.count());
    EXPECT_TRUE(server.isListening());
}

} // namespace
