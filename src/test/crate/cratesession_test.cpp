#include "crate/session/cratesession.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <memory>
#include <optional>

#include "crate/client/crateclient.h"
#include "crate/session/cratesecretstore.h"
#include "test/crate/fakecratenetwork.h"

namespace {

using namespace std::chrono_literals;
using crate::Client;
using crate::ClientOptions;
using crate::Error;
using crate::JsonResult;
using crate::Reply;
using crate::ServerProfile;
using crate::Session;
using crate::SessionOptions;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeScheduler;
using crate::test::sendHttpRequest;
using Status = crate::SecretStore::Status;

/// A SecretStore in memory that the test can make unavailable.
class FakeSecretStore : public crate::SecretStore {
  public:
    QHash<QString, QString> secrets;
    bool unavailable = false;

    void read(const QString& key, ReadDone done) override {
        if (unavailable) {
            done(Status::Unavailable, QString());
            return;
        }
        const auto it = secrets.constFind(key);
        if (it == secrets.cend()) {
            done(Status::NotFound, QString());
            return;
        }
        done(Status::Ok, it.value());
    }

    void write(const QString& key, const QString& secret, Done done) override {
        if (unavailable) {
            done(Status::Unavailable);
            return;
        }
        secrets.insert(key, secret);
        done(Status::Ok);
    }

    void remove(const QString& key, Done done) override {
        if (unavailable) {
            done(Status::Unavailable);
            return;
        }
        done(secrets.remove(key) > 0 ? Status::Ok : Status::NotFound);
    }
};

const QString kRefreshKey = QStringLiteral("5b0c:refresh");
const QByteArray kLoginAnswer = QByteArrayLiteral(
        R"({"id": 7, "email": "dj@crate.test", "role": "user", "token": "access-1",
            "refresh_token": "refresh-1", "session": {"id": "session-1"}})");

QJsonObject bodyOf(const FakeNetworkAccessManager::Sent& sent) {
    return QJsonDocument::fromJson(sent.body).object();
}

QString field(const FakeNetworkAccessManager::Sent& sent, const char* name) {
    return bodyOf(sent).value(QLatin1String(name)).toString();
}

QString base64UrlSha256(const QString& text) {
    return QString::fromLatin1(
            QCryptographicHash::hash(text.toLatin1(), QCryptographicHash::Sha256)
                    .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

class CrateSessionTest : public testing::Test {
  protected:
    CrateSessionTest() {
        ClientOptions clientOptions;
        clientOptions.deviceFingerprint = QStringLiteral("install-1");
        clientOptions.schedule = m_scheduler.function();
        clientOptions.random = [] { return 0.5; };
        m_pClient = std::make_unique<Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, std::move(clientOptions));
        SessionOptions options;
        options.schedule = m_scheduler.function();
        ServerProfile profile;
        profile.id = QStringLiteral("5b0c");
        profile.baseUrl = QUrl(QStringLiteral("https://crate.test"));
        m_pSession = std::make_unique<Session>(
                profile, m_pClient.get(), &m_store, std::move(options));
        QObject::connect(m_pSession.get(), &Session::loginFailed, [this](const Error& error) {
            m_loginFailures.append(error);
        });
        QObject::connect(m_pSession.get(), &Session::openBrowser, [this](const QUrl& url) {
            m_openedUrl = url;
        });
    }

    FakeNetworkAccessManager::Sent lastSent() const {
        return m_network.sent.last();
    }

    void signIn() {
        m_pSession->loginWithPassword(
                QStringLiteral("dj@crate.test"), QStringLiteral("secret-password"));
        lastSent().pReply->respond(200, kLoginAnswer);
    }

    struct BrowserStart {
        QUrl callback;
        QString challenge;
        QString state;
    };

    BrowserStart startBrowserLogin() {
        m_pSession->loginWithBrowser(QStringLiteral("google"));
        const FakeNetworkAccessManager::Sent start = lastSent();
        EXPECT_EQ(QStringLiteral("/api/auth/oauth/google/start"), start.request.url().path());
        EXPECT_FALSE(start.request.hasRawHeader("Authorization"));
        BrowserStart browser{QUrl(field(start, "return_to")),
                field(start, "native_code_challenge"),
                field(start, "native_state")};
        start.pReply->respond(200,
                QByteArrayLiteral(R"({"provider": "google",
                    "login_url": "https://accounts.example/o/auth?client=crate"})"));
        EXPECT_EQ(QUrl(QStringLiteral("https://accounts.example/o/auth?client=crate")),
                m_openedUrl);
        return browser;
    }

    static QByteArray callbackRequest(const QString& query) {
        return "GET /callback?" + query.toLatin1() + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    }

    FakeNetworkAccessManager m_network;
    FakeScheduler m_scheduler;
    FakeSecretStore m_store;
    std::unique_ptr<Client> m_pClient;
    std::unique_ptr<Session> m_pSession;
    QList<Error> m_loginFailures;
    QUrl m_openedUrl;
};

TEST_F(CrateSessionTest, PasswordLoginStoresRefreshInSecretStore) {
    QSignalSpy signedIn(m_pSession.get(), &Session::signedIn);
    int profileChanges = 0;
    QObject::connect(m_pSession.get(), &Session::profileChanged, [&](const ServerProfile&) {
        ++profileChanges;
    });

    m_pSession->loginWithPassword(
            QStringLiteral("dj@crate.test"), QStringLiteral("secret-password"));
    const FakeNetworkAccessManager::Sent login = lastSent();
    EXPECT_EQ(QStringLiteral("/api/auth/login"), login.request.url().path());
    EXPECT_FALSE(login.request.hasRawHeader("Authorization"));
    EXPECT_EQ(QStringLiteral("dj@crate.test"), field(login, "email"));
    EXPECT_EQ(QStringLiteral("secret-password"), field(login, "password"));
    EXPECT_EQ(Session::State::SigningIn, m_pSession->state());
    login.pReply->respond(200, kLoginAnswer);

    EXPECT_EQ(QStringLiteral("refresh-1"), m_store.secrets.value(kRefreshKey));
    EXPECT_EQ(QStringLiteral("access-1"), m_pSession->accessToken());
    EXPECT_EQ(Session::State::SignedIn, m_pSession->state());
    EXPECT_EQ(1, signedIn.count());
    EXPECT_EQ(QStringLiteral("7"), m_pSession->profile().accountId);
    EXPECT_EQ(QStringLiteral("dj@crate.test"), m_pSession->profile().accountEmail);
    EXPECT_EQ(1, profileChanges);
}

TEST_F(CrateSessionTest, WrongPasswordFailsTheLogin) {
    m_pSession->loginWithPassword(QStringLiteral("dj@crate.test"), QStringLiteral("wrong"));
    lastSent().pReply->respond(401, QByteArrayLiteral(R"({"detail": "Invalid credentials"})"));

    ASSERT_EQ(1, m_loginFailures.size());
    EXPECT_EQ(401, m_loginFailures.first().httpStatus);
    EXPECT_EQ(Session::State::SignedOut, m_pSession->state());
    EXPECT_TRUE(m_store.secrets.isEmpty());
}

TEST_F(CrateSessionTest, FallsBackToMemoryWhenKeychainUnavailable) {
    m_store.unavailable = true;
    QSignalSpy unavailable(m_pSession.get(), &Session::secretStoreUnavailable);

    signIn();
    m_pClient->get(QStringLiteral("/api/library"));
    lastSent().pReply->respond(401);

    EXPECT_EQ(1, unavailable.count());
    EXPECT_EQ(Session::State::SignedIn, m_pSession->state());
    EXPECT_EQ(QStringLiteral("/api/auth/refresh"), lastSent().request.url().path());
    EXPECT_EQ(QStringLiteral("refresh-1"), field(lastSent(), "refresh_token"));
}

TEST_F(CrateSessionTest, RefreshUsesStoredToken) {
    m_store.secrets.insert(kRefreshKey, QStringLiteral("refresh-0"));
    QSignalSpy signedIn(m_pSession.get(), &Session::signedIn);

    m_pSession->restore();
    const FakeNetworkAccessManager::Sent refresh = lastSent();
    EXPECT_EQ(QStringLiteral("/api/auth/refresh"), refresh.request.url().path());
    EXPECT_FALSE(refresh.request.hasRawHeader("Authorization"));
    EXPECT_EQ(QStringLiteral("refresh-0"), field(refresh, "refresh_token"));
    refresh.pReply->respond(200,
            QByteArrayLiteral(R"({"token": "access-9", "refresh_token": "refresh-1"})"));

    EXPECT_EQ(Session::State::SignedIn, m_pSession->state());
    EXPECT_EQ(QStringLiteral("access-9"), m_pSession->accessToken());
    EXPECT_EQ(QStringLiteral("refresh-1"), m_store.secrets.value(kRefreshKey));
    EXPECT_EQ(1, signedIn.count());
}

TEST_F(CrateSessionTest, RestoreWithoutStoredTokenStaysSignedOut) {
    QSignalSpy signedOut(m_pSession.get(), &Session::signedOut);

    m_pSession->restore();

    EXPECT_TRUE(m_network.sent.isEmpty());
    EXPECT_EQ(Session::State::SignedOut, m_pSession->state());
    EXPECT_EQ(1, signedOut.count());
}

TEST_F(CrateSessionTest, RenewsTheAccessTokenForTheClient) {
    signIn();
    std::optional<JsonResult> result;
    QObject::connect(m_pClient->get(QStringLiteral("/api/library")),
            &Reply::finished,
            [&](const JsonResult& finished) { result = finished; });

    lastSent().pReply->respond(401);
    const FakeNetworkAccessManager::Sent refresh = lastSent();
    EXPECT_EQ(QStringLiteral("refresh-1"), field(refresh, "refresh_token"));
    refresh.pReply->respond(200,
            QByteArrayLiteral(R"({"token": "access-2", "refresh_token": "refresh-2"})"));

    EXPECT_EQ(QStringLiteral("/api/library"), lastSent().request.url().path());
    EXPECT_EQ(QByteArrayLiteral("Bearer access-2"), lastSent().request.rawHeader("Authorization"));
    EXPECT_EQ(QStringLiteral("refresh-2"), m_store.secrets.value(kRefreshKey));
    lastSent().pReply->respond(200, QByteArrayLiteral("{}"));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok());
}

TEST_F(CrateSessionTest, RejectedRefreshEndsTheSession) {
    m_store.secrets.insert(kRefreshKey, QStringLiteral("refresh-0"));
    QSignalSpy signedOut(m_pSession.get(), &Session::signedOut);

    m_pSession->restore();
    lastSent().pReply->respond(401, QByteArrayLiteral(R"({"detail": "Invalid refresh token"})"));

    EXPECT_EQ(Session::State::SignedOut, m_pSession->state());
    EXPECT_FALSE(m_store.secrets.contains(kRefreshKey));
    EXPECT_EQ(1, signedOut.count());
}

TEST_F(CrateSessionTest, UnreachableRefreshKeepsTheSession) {
    signIn();
    std::optional<JsonResult> result;
    QObject::connect(m_pClient->get(QStringLiteral("/api/library")),
            &Reply::finished,
            [&](const JsonResult& finished) { result = finished; });

    lastSent().pReply->respond(401);
    lastSent().pReply->failNetwork();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Error::Kind::Network, result->error().kind);
    EXPECT_EQ(Session::State::SignedIn, m_pSession->state());
    EXPECT_EQ(QStringLiteral("refresh-1"), m_store.secrets.value(kRefreshKey));
}

TEST_F(CrateSessionTest, LogoutClearsSecretAndRevokes) {
    signIn();
    QSignalSpy signedOut(m_pSession.get(), &Session::signedOut);

    m_pSession->logout();

    EXPECT_EQ(Session::State::SignedOut, m_pSession->state());
    EXPECT_TRUE(m_pSession->accessToken().isEmpty());
    EXPECT_FALSE(m_store.secrets.contains(kRefreshKey));
    EXPECT_EQ(1, signedOut.count());
    const FakeNetworkAccessManager::Sent revoke = lastSent();
    EXPECT_EQ(QStringLiteral("/api/auth/logout"), revoke.request.url().path());
    EXPECT_EQ(QByteArrayLiteral("Bearer access-1"), revoke.request.rawHeader("Authorization"));
}

TEST_F(CrateSessionTest, BrowserLoginExchangesTheCode) {
    const BrowserStart start = startBrowserLogin();
    EXPECT_EQ(QStringLiteral("127.0.0.1"), start.callback.host());
    EXPECT_EQ(QStringLiteral("/callback"), start.callback.path());
    EXPECT_GE(start.state.size(), 16);

    const QByteArray page = sendHttpRequest(static_cast<quint16>(start.callback.port()),
            callbackRequest(QStringLiteral("code=one-time-code-123456&state=") + start.state));
    EXPECT_TRUE(page.startsWith("HTTP/1.1 200"));
    const FakeNetworkAccessManager::Sent exchange = lastSent();
    EXPECT_EQ(QStringLiteral("/api/auth/native/exchange"), exchange.request.url().path());
    EXPECT_EQ(QStringLiteral("one-time-code-123456"), field(exchange, "code"));
    EXPECT_EQ(start.state, field(exchange, "state"));
    const QString verifier = field(exchange, "code_verifier");
    EXPECT_GE(verifier.size(), 43);
    EXPECT_LE(verifier.size(), 128);
    EXPECT_EQ(start.challenge, base64UrlSha256(verifier));
    exchange.pReply->respond(200, kLoginAnswer);

    EXPECT_EQ(Session::State::SignedIn, m_pSession->state());
    EXPECT_EQ(QStringLiteral("refresh-1"), m_store.secrets.value(kRefreshKey));
    EXPECT_TRUE(m_loginFailures.isEmpty());
}

TEST_F(CrateSessionTest, BrowserLoginRejectsMismatchedState) {
    const BrowserStart start = startBrowserLogin();

    sendHttpRequest(static_cast<quint16>(start.callback.port()),
            callbackRequest(QStringLiteral("code=one-time-code-123456&state=forged-state-value")));

    EXPECT_EQ(1, m_network.sent.size());
    ASSERT_EQ(1, m_loginFailures.size());
    EXPECT_EQ(Error::Kind::Auth, m_loginFailures.first().kind);
    EXPECT_EQ(Session::State::SignedOut, m_pSession->state());
}

TEST_F(CrateSessionTest, BrowserLoginReportsCancellation) {
    const BrowserStart start = startBrowserLogin();

    sendHttpRequest(static_cast<quint16>(start.callback.port()),
            callbackRequest(QStringLiteral("state=") + start.state +
                    QStringLiteral("&error=cancelled")));

    ASSERT_EQ(1, m_loginFailures.size());
    EXPECT_EQ(Error::Kind::Cancelled, m_loginFailures.first().kind);
    EXPECT_EQ(Session::State::SignedOut, m_pSession->state());
}

TEST_F(CrateSessionTest, BrowserLoginTimesOut) {
    const BrowserStart start = startBrowserLogin();
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(std::chrono::milliseconds(5min), m_scheduler.tasks.first().delay);

    m_scheduler.runAll();
    const QByteArray late = sendHttpRequest(static_cast<quint16>(start.callback.port()),
            callbackRequest(QStringLiteral("code=one-time-code-123456&state=") + start.state));

    ASSERT_EQ(1, m_loginFailures.size());
    EXPECT_EQ(Session::State::SignedOut, m_pSession->state());
    EXPECT_TRUE(late.isEmpty());
    EXPECT_EQ(1, m_network.sent.size());
}

} // namespace
