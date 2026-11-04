#include "crate/session/cratesession.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimer>
#include <utility>
#include <vector>

#include "crate/client/cratejson.h"
#include "crate/session/crateloopbackserver.h"
#include "util/logger.h"

namespace crate {

namespace {

// Never log a password, a token, the PKCE verifier, the OAuth state or the
// one-time code: the messages below only say what happened.
const mixxx::Logger kLogger("crate::Session");

RequestOptions anonymous() {
    RequestOptions options;
    options.authenticated = false;
    return options;
}

/// `bytes` bytes from the system's secure random source, as base64url
/// without padding.
QString randomBase64Url(int bytes) {
    std::vector<quint32> words(static_cast<size_t>((bytes + 3) / 4));
    QRandomGenerator::system()->generate(words.begin(), words.end());
    const QByteArray data(reinterpret_cast<const char*>(words.data()), bytes);
    return QString::fromLatin1(
            data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

/// The S256 code challenge of a PKCE verifier (RFC 7636).
QString pkceChallenge(const QString& verifier) {
    return QString::fromLatin1(
            QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
                    .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

struct Tokens {
    QString access;
    QString refresh;
    QString accountId;
    QString email;
};

/// AuthLoginResponse (`withAccount`) or AuthRefreshResponse.
Result<Tokens> readTokens(const QJsonDocument& document, bool withAccount) {
    const Result<QJsonObject> object = json::requireObject(document);
    if (!object.ok()) {
        return object.error();
    }
    json::ObjectReader reader(object.value());
    Tokens tokens;
    tokens.access = reader.requiredString(u"token");
    tokens.refresh = reader.optionalString(u"refresh_token");
    if (withAccount) {
        if (const std::optional<qint64> id = reader.optionalInteger(u"id")) {
            tokens.accountId = QString::number(*id);
        }
        tokens.email = reader.requiredString(u"email");
    }
    if (!reader.ok()) {
        return reader.error();
    }
    if (tokens.access.isEmpty()) {
        return Error::decode(QStringLiteral("The answer has an empty access token"));
    }
    return tokens;
}

void logRevokeFailure(const Error& error) {
    kLogger.info() << "Could not revoke the session on the server:" << error.message;
}

void revokeWith(const QPointer<Client>& pClient, const QString& accessToken);

/// Gets a fresh access token for the session of `refreshToken`, only to
/// revoke that session with it. Nothing is stored.
void renewThenRevoke(const QPointer<Client>& pClient, const QString& refreshToken) {
    if (!pClient || refreshToken.isEmpty()) {
        return;
    }
    const QJsonObject body{{QStringLiteral("refresh_token"), refreshToken}};
    Reply* pReply = pClient->post(
            QStringLiteral("/api/auth/refresh"), QJsonDocument(body), anonymous());
    QObject::connect(pReply, &Reply::finished, pReply, [pClient](const JsonResult& result) {
        if (!result.ok()) {
            logRevokeFailure(result.error());
            return;
        }
        const Result<Tokens> tokens = readTokens(result.value(), false);
        if (!tokens.ok()) {
            logRevokeFailure(tokens.error());
            return;
        }
        revokeWith(pClient, tokens.value().access);
    });
}

void revokeWith(const QPointer<Client>& pClient, const QString& accessToken) {
    if (!pClient) {
        return;
    }
    RequestOptions revoke;
    revoke.bearerToken = accessToken;
    Reply* pReply = pClient->post(
            QStringLiteral("/api/auth/logout"), QJsonDocument(QJsonObject()), revoke);
    QObject::connect(pReply, &Reply::finished, pReply, [](const JsonResult& result) {
        if (!result.ok()) {
            logRevokeFailure(result.error());
        }
    });
}

/// Revokes the session on the server with `accessToken`. If the server
/// rejects it, as after an hour without requests, or there is none, as
/// after an offline restore, it renews the session once to revoke it.
void revokeSession(const QPointer<Client>& pClient,
        const QString& accessToken,
        const QString& refreshToken) {
    if (!pClient) {
        return;
    }
    if (accessToken.isEmpty()) {
        renewThenRevoke(pClient, refreshToken);
        return;
    }
    RequestOptions revoke;
    revoke.bearerToken = accessToken;
    Reply* pReply = pClient->post(
            QStringLiteral("/api/auth/logout"), QJsonDocument(QJsonObject()), revoke);
    QObject::connect(pReply,
            &Reply::finished,
            pReply,
            [pClient, refreshToken](const JsonResult& result) {
                if (result.ok()) {
                    return;
                }
                if (result.error().kind == Error::Kind::Auth) {
                    renewThenRevoke(pClient, refreshToken);
                    return;
                }
                logRevokeFailure(result.error());
            });
}

} // namespace

Session::Session(ServerProfile profile,
        Client* pClient,
        SecretStore* pSecretStore,
        SessionOptions options,
        QObject* parent)
        : QObject(parent),
          m_profile(std::move(profile)),
          m_pClient(pClient),
          m_pSecretStore(pSecretStore),
          m_options(std::move(options)) {
    m_pClient->setTokenSource(this);
}

Session::~Session() {
    if (m_pClient) {
        m_pClient->setTokenSource(nullptr);
    }
}

void Session::restore() {
    endBrowserLogin();
    const int attempt = ++m_loginAttempt;
    setState(State::SigningIn);
    const QPointer<Session> pGuard(this);
    const QString key = refreshTokenKey(m_profile.id);
    m_pSecretStore->read(key,
            [pGuard, attempt, key](SecretStore::Status status, const QString& secret) {
                if (!pGuard || pGuard->m_loginAttempt != attempt) {
                    return;
                }
                if (status != SecretStore::Status::Unavailable) {
                    pGuard->restoreWith(attempt,
                            status == SecretStore::Status::Ok ? secret : QString());
                    return;
                }
                pGuard->onKeychainUnavailable();
                if (!pGuard) {
                    return;
                }
                // A sign-in earlier in this run may be kept in memory.
                pGuard->m_sessionOnlyStore.read(key,
                        [pGuard, attempt](SecretStore::Status memoryStatus,
                                const QString& memorySecret) {
                            if (pGuard && pGuard->m_loginAttempt == attempt) {
                                pGuard->restoreWith(attempt,
                                        memoryStatus == SecretStore::Status::Ok
                                                ? memorySecret
                                                : QString());
                            }
                        });
            });
}

void Session::restoreWith(int attempt, const QString& refreshToken) {
    if (refreshToken.isEmpty()) {
        const QPointer<Session> pGuard(this);
        setState(State::SignedOut);
        if (pGuard) {
            emit signedOut();
        }
        return;
    }
    m_refreshToken = refreshToken;
    const QPointer<Session> pGuard(this);
    renewAccessToken([pGuard, attempt](Renewal renewal) {
        // SessionEnded already signed out. Unreachable still counts as signed
        // in: the access token comes once the server answers.
        if (!pGuard || pGuard->m_loginAttempt != attempt || renewal == Renewal::SessionEnded) {
            return;
        }
        pGuard->setState(State::SignedIn);
        if (pGuard) {
            emit pGuard->signedIn();
        }
    });
}

void Session::loginWithPassword(const QString& email, const QString& password) {
    endBrowserLogin();
    const int attempt = ++m_loginAttempt;
    setState(State::SigningIn);
    const QJsonObject body{
            {QStringLiteral("email"), email},
            {QStringLiteral("password"), password},
    };
    Reply* pReply = m_pClient->post(
            QStringLiteral("/api/auth/login"), QJsonDocument(body), anonymous());
    connect(pReply, &Reply::finished, this, [this, attempt](const JsonResult& result) {
        onLoginAnswer(attempt, result);
    });
}

void Session::loginWithBrowser(const QString& provider) {
    endBrowserLogin();
    const int attempt = ++m_loginAttempt;
    setState(State::SigningIn);
    auto* pServer = new LoopbackCallbackServer(this);
    m_pCallbackServer = pServer;
    if (!pServer->listen()) {
        failLogin(Error::network(QStringLiteral("Cannot wait for the browser on 127.0.0.1")));
        return;
    }
    m_codeVerifier = randomBase64Url(64);
    m_oauthState = randomBase64Url(32);
    pServer->setExpectedState(m_oauthState);
    connect(pServer,
            &LoopbackCallbackServer::callbackReceived,
            this,
            [this, attempt](const QUrlQuery& query) {
                onBrowserCallback(attempt, query);
            });
    const QPointer<Session> pGuard(this);
    schedule(m_options.browserLoginTimeout, [pGuard, attempt] {
        if (pGuard && pGuard->m_loginAttempt == attempt && pGuard->m_pCallbackServer) {
            kLogger.info() << "The browser sign-in timed out";
            pGuard->failLogin(Error::network(QStringLiteral("The browser sign-in timed out")));
        }
    });

    const QJsonObject body{
            {QStringLiteral("return_to"), pServer->callbackUrl().toString()},
            {QStringLiteral("native_code_challenge"), pkceChallenge(m_codeVerifier)},
            {QStringLiteral("native_state"), m_oauthState},
    };
    const QString path = QStringLiteral("/api/auth/oauth/%1/start")
                                 .arg(QString::fromLatin1(QUrl::toPercentEncoding(provider)));
    Reply* pReply = m_pClient->post(path, QJsonDocument(body), anonymous());
    connect(pReply, &Reply::finished, this, [this, attempt](const JsonResult& result) {
        if (attempt != m_loginAttempt) {
            return;
        }
        if (!result.ok()) {
            failLogin(result.error());
            return;
        }
        const Result<QJsonObject> object = json::requireObject(result.value());
        if (!object.ok()) {
            failLogin(object.error());
            return;
        }
        json::ObjectReader reader(object.value());
        const QUrl loginUrl(reader.requiredString(u"login_url"));
        if (!reader.ok()) {
            failLogin(reader.error());
            return;
        }
        if (!loginUrl.isValid() ||
                (loginUrl.scheme() != QLatin1String("https") &&
                        loginUrl.scheme() != QLatin1String("http"))) {
            failLogin(Error::decode(QStringLiteral("The sign-in page is not a web address")));
            return;
        }
        emit openBrowser(loginUrl);
    });
}

void Session::cancelLogin() {
    if (m_state == State::SigningIn) {
        failLogin(Error::cancelled());
    }
}

void Session::logout() {
    endBrowserLogin();
    ++m_loginAttempt;
    // With copies of the tokens: the session is cleared right after.
    revokeSession(m_pClient, m_accessToken, m_refreshToken);
    endSession();
}

QString Session::accessToken() const {
    return m_accessToken;
}

void Session::renewAccessToken(std::function<void(Renewal renewal)> done) {
    if (m_refreshToken.isEmpty() || !m_pClient) {
        done(Renewal::SessionEnded);
        return;
    }
    const QJsonObject body{{QStringLiteral("refresh_token"), m_refreshToken}};
    Reply* pReply = m_pClient->post(
            QStringLiteral("/api/auth/refresh"), QJsonDocument(body), anonymous());
    const quint64 generation = m_sessionGeneration;
    connect(pReply, &Reply::finished, this, [this, done, generation](const JsonResult& result) {
        // A copy: endSession() emits signals whose slots may delete this.
        const auto renewalDone = done;
        if (generation != m_sessionGeneration) {
            // Signed out or signed in again meanwhile: the answer is not for
            // this session, whose token is current or gone.
            renewalDone(m_accessToken.isEmpty() ? Renewal::SessionEnded : Renewal::Renewed);
            return;
        }
        if (!result.ok()) {
            const Error& error = result.error();
            if (error.kind == Error::Kind::Http &&
                    (error.httpStatus == 401 || error.httpStatus == 403)) {
                kLogger.info() << "The server ended the session";
                endSession();
                renewalDone(Renewal::SessionEnded);
                return;
            }
            renewalDone(Renewal::Unreachable);
            return;
        }
        const Result<Tokens> tokens = readTokens(result.value(), false);
        if (!tokens.ok()) {
            kLogger.warning() << "Unexpected answer to a renewal:" << tokens.error().message;
            renewalDone(Renewal::Unreachable);
            return;
        }
        m_accessToken = tokens.value().access;
        const QString refreshToken = tokens.value().refresh;
        if (!refreshToken.isEmpty() && refreshToken != m_refreshToken) {
            m_refreshToken = refreshToken;
            storeRefreshToken(refreshToken, nullptr);
        }
        renewalDone(Renewal::Renewed);
    });
}

void Session::setState(State state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state);
}

void Session::onLoginAnswer(int attempt, const JsonResult& result) {
    if (attempt != m_loginAttempt) {
        return;
    }
    if (!result.ok()) {
        failLogin(result.error());
        return;
    }
    const Result<Tokens> tokens = readTokens(result.value(), true);
    if (!tokens.ok()) {
        failLogin(tokens.error());
        return;
    }
    if (tokens.value().refresh.isEmpty()) {
        failLogin(Error::decode(QStringLiteral("The server did not return a refresh token")));
        return;
    }
    endBrowserLogin();
    ++m_sessionGeneration;
    m_accessToken = tokens.value().access;
    m_refreshToken = tokens.value().refresh;
    const QPointer<Session> pGuard(this);
    if (m_profile.accountId != tokens.value().accountId ||
            m_profile.accountEmail != tokens.value().email) {
        m_profile.accountId = tokens.value().accountId;
        m_profile.accountEmail = tokens.value().email;
        emit profileChanged(m_profile);
        if (!pGuard) {
            return;
        }
    }
    storeRefreshToken(m_refreshToken, [pGuard, attempt] {
        if (!pGuard || pGuard->m_loginAttempt != attempt) {
            return;
        }
        pGuard->setState(State::SignedIn);
        if (pGuard) {
            emit pGuard->signedIn();
        }
    });
}

void Session::onBrowserCallback(int attempt, const QUrlQuery& query) {
    if (attempt != m_loginAttempt) {
        return;
    }
    const QString state = query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
    const QString error = query.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
    const QString code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    const QString verifier = m_codeVerifier;
    const bool stateMatches = !m_oauthState.isEmpty() && state == m_oauthState;
    endBrowserLogin();
    if (!stateMatches) {
        kLogger.warning() << "The browser came back from another sign-in";
        failLogin(Error::auth(QStringLiteral("The browser sign-in does not match this request")));
        return;
    }
    if (error == QLatin1String("cancelled")) {
        failLogin(Error::cancelled());
        return;
    }
    if (!error.isEmpty()) {
        failLogin(Error::auth(QStringLiteral("The sign-in provider refused the sign-in")));
        return;
    }
    if (code.isEmpty()) {
        failLogin(Error::decode(QStringLiteral("The browser came back without a code")));
        return;
    }
    const QJsonObject body{
            {QStringLiteral("code"), code},
            {QStringLiteral("code_verifier"), verifier},
            {QStringLiteral("state"), state},
    };
    Reply* pReply = m_pClient->post(
            QStringLiteral("/api/auth/native/exchange"), QJsonDocument(body), anonymous());
    connect(pReply, &Reply::finished, this, [this, attempt](const JsonResult& result) {
        onLoginAnswer(attempt, result);
    });
}

void Session::failLogin(const Error& error) {
    endBrowserLogin();
    // Answers still in flight belong to the failed sign-in.
    ++m_loginAttempt;
    const QPointer<Session> pGuard(this);
    // A failed new sign-in keeps the session that was already there.
    setState(m_refreshToken.isEmpty() ? State::SignedOut : State::SignedIn);
    if (pGuard) {
        emit loginFailed(error);
    }
}

void Session::endBrowserLogin() {
    if (m_pCallbackServer) {
        m_pCallbackServer->close();
        m_pCallbackServer->deleteLater();
        m_pCallbackServer.clear();
    }
    m_codeVerifier.clear();
    m_oauthState.clear();
}

void Session::endSession() {
    ++m_sessionGeneration;
    const bool wasSignedOut = m_state == State::SignedOut;
    m_accessToken.clear();
    m_refreshToken.clear();
    // From both: a token written during a keychain outage is in memory, and
    // one from an earlier run may still be in the keychain.
    const QString key = refreshTokenKey(m_profile.id);
    m_sessionOnlyStore.remove(key, [](SecretStore::Status) {});
    m_pSecretStore->remove(key, [](SecretStore::Status status) {
        if (status == SecretStore::Status::Unavailable) {
            kLogger.warning() << "Could not delete the refresh token from the keychain";
        }
    });
    if (wasSignedOut) {
        return;
    }
    const QPointer<Session> pGuard(this);
    setState(State::SignedOut);
    if (pGuard) {
        emit signedOut();
    }
}

void Session::storeRefreshToken(const QString& refreshToken, std::function<void()> then) {
    const QPointer<Session> pGuard(this);
    const QString key = refreshTokenKey(m_profile.id);
    m_pSecretStore->write(key,
            refreshToken,
            [pGuard, key, refreshToken, then](SecretStore::Status status) {
                if (!pGuard) {
                    return;
                }
                if (status == SecretStore::Status::Unavailable) {
                    pGuard->m_sessionOnlyStore.write(key, refreshToken, [](SecretStore::Status) {});
                    pGuard->onKeychainUnavailable();
                    if (!pGuard) {
                        return;
                    }
                } else {
                    pGuard->m_keychainUnavailable = false;
                    pGuard->m_sessionOnlyStore.remove(key, [](SecretStore::Status) {});
                }
                if (then) {
                    then();
                }
            });
}

void Session::onKeychainUnavailable() {
    if (m_keychainUnavailable) {
        return;
    }
    kLogger.warning() << "The keychain is unavailable: the session lasts for this run only";
    m_keychainUnavailable = true;
    emit secretStoreUnavailable();
}

void Session::schedule(std::chrono::milliseconds delay, std::function<void()> callback) {
    if (m_options.schedule) {
        m_options.schedule(delay, std::move(callback));
    } else {
        QTimer::singleShot(delay, this, std::move(callback));
    }
}

} // namespace crate

#include "moc_cratesession.cpp"
