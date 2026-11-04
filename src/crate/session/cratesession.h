#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <chrono>
#include <functional>

#include "crate/client/crateclient.h"
#include "crate/client/crateresult.h"
#include "crate/session/cratesecretstore.h"

namespace crate {

class LoopbackCallbackServer;

/// A Crate server this installation signs in to.
struct ServerProfile {
    /// Random UUID; also names the keychain entry of its refresh token.
    QString id;
    QUrl baseUrl;
    QString displayName;
    /// Id of the signed-in user on that server (`account_id`).
    QString accountId;
    /// Email of the signed-in account, as the server reports it at sign-in.
    /// Shown while signed in; not stored.
    QString accountEmail;
};

struct SessionOptions {
    /// How long a browser sign-in waits for the browser to come back.
    std::chrono::milliseconds browserLoginTimeout = std::chrono::minutes(5);
    /// Runs `callback` after `delay`. Defaults to a single-shot QTimer;
    /// tests replace it to control time.
    std::function<void(std::chrono::milliseconds delay, std::function<void()> callback)> schedule;
};

/// The session of this installation with one Crate server (S1).
///
/// It signs in with email and password or through the browser (PKCE and a
/// loopback callback on 127.0.0.1), keeps the access token in memory and the
/// refresh token in the SecretStore, renews the access token for the Client
/// (as its TokenSource) and signs out. The password and the tokens never
/// reach the settings or the log.
///
/// If the SecretStore is unavailable, the refresh token is kept for this run
/// only and secretStoreUnavailable() says so, so that the interface can tell
/// the user they will sign in again after a restart.
class Session : public QObject, public TokenSource {
    Q_OBJECT
  public:
    enum class State {
        SignedOut,
        SigningIn,
        SignedIn,
    };

    /// Owns neither `pClient` nor `pSecretStore`, which must outlive it.
    /// The Session makes itself the Client's TokenSource.
    Session(ServerProfile profile,
            Client* pClient,
            SecretStore* pSecretStore,
            SessionOptions options = SessionOptions(),
            QObject* parent = nullptr);
    ~Session() override;

    const ServerProfile& profile() const {
        return m_profile;
    }
    State state() const {
        return m_state;
    }

    /// Signs in again with the refresh token kept from an earlier run, if
    /// there is one. Without a network the session counts as signed in and
    /// renews its access token once the server answers.
    void restore();
    void loginWithPassword(const QString& email, const QString& password);
    /// `provider` is "google" or "apple". Emits openBrowser() with the
    /// page the browser must open.
    void loginWithBrowser(const QString& provider);
    /// Stops a sign-in in progress; loginFailed() reports it as Cancelled.
    void cancelLogin();
    /// Signs out at once and revokes the session on the server.
    void logout();

    QString accessToken() const override;
    void renewAccessToken(const std::function<void(Renewal renewal)>& done) override;

  signals:
    void stateChanged(crate::Session::State state);
    void signedIn();
    void signedOut();
    void loginFailed(const crate::Error& error);
    void openBrowser(const QUrl& url);
    void profileChanged(const crate::ServerProfile& profile);
    void secretStoreUnavailable();

  private:
    void setState(State state);
    void onLoginAnswer(int attempt, const JsonResult& result);
    void onBrowserCallback(int attempt, const QUrlQuery& query);
    void failLogin(const Error& error);
    void endBrowserLogin();
    void endSession();
    void restoreWith(int attempt, const QString& refreshToken);
    void storeRefreshToken(const QString& refreshToken, std::function<void()> then);
    void onKeychainUnavailable();
    void schedule(std::chrono::milliseconds delay, std::function<void()> callback);

    ServerProfile m_profile;
    const QPointer<Client> m_pClient;
    SecretStore* const m_pSecretStore;
    /// Holds the refresh token while the keychain fails. The keychain is
    /// still tried first on every write, and both are cleared on sign-out.
    SessionOnlySecretStore m_sessionOnlyStore;
    /// The last keychain operation failed: secretStoreUnavailable() was
    /// emitted for this outage.
    bool m_keychainUnavailable = false;
    SessionOptions m_options;

    State m_state = State::SignedOut;
    QString m_accessToken;
    QString m_refreshToken;
    /// Tells the answers of the current sign-in from those of an older one.
    int m_loginAttempt = 0;
    /// Changes when the session ends or new tokens are installed, so that a
    /// renewal answered later never touches a newer session.
    quint64 m_sessionGeneration = 0;
    QPointer<LoopbackCallbackServer> m_pCallbackServer;
    QString m_codeVerifier;
    QString m_oauthState;
};

} // namespace crate
