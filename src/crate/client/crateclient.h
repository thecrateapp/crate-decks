#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "crate/client/crateresult.h"
#include "preferences/usersettings.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace crate {

class Client;

/// Gives the Client the access token of the session and renews it when the
/// server rejects it. crate::Session (F2.C-PR2) implements it.
class TokenSource {
  public:
    virtual ~TokenSource() = default;

    /// Access token for the Authorization header; empty when signed out.
    virtual QString accessToken() const = 0;

    /// Starts renewing the access token. Calls `done(true)` once
    /// accessToken() returns the new token, or `done(false)` if the session
    /// cannot be renewed. The Client never asks for a second renewal while one
    /// is in flight.
    virtual void renewAccessToken(std::function<void(bool renewed)> done) = 0;
};

struct ClientOptions {
    /// ID of this installation, sent as X-Device-Fingerprint (see
    /// deviceFingerprint()). The server keeps one session per device with it.
    QString deviceFingerprint;
    /// API requests in flight at the same time; the rest wait in order.
    int maxConcurrentRequests = 4;
    /// Attempts of an idempotent request, the first one included.
    int maxAttempts = 3;
    /// Delay before the first retry; it doubles for each further one.
    std::chrono::milliseconds retryBaseDelay{500};
    /// A Retry-After longer than this is not waited for: the request fails.
    std::chrono::milliseconds maxRetryAfter{30000};
    /// A request that transfers nothing for this long fails as Network.
    std::chrono::milliseconds transferTimeout{30000};
    /// Runs `callback` after `delay`. Defaults to a single-shot QTimer;
    /// tests replace it to control time.
    std::function<void(std::chrono::milliseconds delay, std::function<void()> callback)> schedule;
    /// A number in [0, 1) for the jitter of the backoff. Defaults to
    /// QRandomGenerator; tests replace it.
    std::function<double()> random;
};

struct RequestOptions {
    QUrlQuery query;
    /// Sends the access token and renews it on 401. Off for the sign-in
    /// requests, whose 401 means wrong credentials.
    bool authenticated = true;
};

/// The ID of this installation, created the first time and kept in the
/// settings. It is not a secret.
QString deviceFingerprint(UserSettingsPointer pConfig);

/// One request to a Crate server, as seen by its caller.
///
/// The Client owns it. It emits finished() once and then deletes itself
/// (deleteLater), so callers only connect to it. Deleting it earlier cancels
/// the request without a signal; cancel() does the same but emits finished()
/// with a Cancelled error.
class Reply : public QObject {
    Q_OBJECT
  public:
    ~Reply() override;

    void cancel();

  signals:
    void finished(const crate::JsonResult& result);
    void progress(qint64 bytesReceived, qint64 bytesTotal);

  private:
    friend class Client;
    explicit Reply(Client* pClient);
    void finish(const JsonResult& result);

    QPointer<Client> m_pClient;
    bool m_finished = false;
};

/// HTTP client for the API of one Crate server.
///
/// Every request carries `X-Crate-App: decks/<version>`, the installation's
/// `X-Device-Fingerprint` and, if authenticated, `Authorization: Bearer`.
/// The token never goes in a URL and redirects stay on the server's origin.
///
/// - At most ClientOptions::maxConcurrentRequests requests are in flight.
/// - GET, PUT and DELETE are retried on 429, 502, 503 and 504 with
///   exponential backoff and jitter, honouring Retry-After.
/// - On 401, one renewal of the access token is started; the rejected
///   requests and the ones sent meanwhile wait for it and are replayed with
///   the new token. If it fails, they finish with an Auth error.
/// - Identical GETs in flight share one network request.
class Client : public QObject {
    Q_OBJECT
  public:
    /// `baseUrl` is the server's address, e.g. https://crate.example.com.
    /// The Client does not own `pNetworkAccessManager`.
    Client(const QUrl& baseUrl,
            QNetworkAccessManager* pNetworkAccessManager,
            ClientOptions options,
            QObject* parent = nullptr);
    ~Client() override;

    /// The Client does not own `pTokenSource`, which must outlive it or be
    /// unset first.
    void setTokenSource(TokenSource* pTokenSource);

    const QUrl& baseUrl() const {
        return m_baseUrl;
    }

    /// `path` is the API path, starting with '/', e.g. "/api/capabilities".
    Reply* get(const QString& path, const RequestOptions& options = RequestOptions());
    Reply* post(const QString& path,
            const QJsonDocument& body,
            const RequestOptions& options = RequestOptions());
    Reply* put(const QString& path,
            const QJsonDocument& body,
            const RequestOptions& options = RequestOptions());
    Reply* del(const QString& path, const RequestOptions& options = RequestOptions());

  private:
    friend class Reply;
    enum class Method {
        Get,
        Post,
        Put,
        Delete,
    };
    struct Call;
    using CallPtr = std::shared_ptr<Call>;

    Reply* request(Method method,
            const QString& path,
            const QByteArray& body,
            const RequestOptions& options);
    void pump();
    void send(const CallPtr& pCall);
    void onNetworkReplyFinished(const CallPtr& pCall);
    void onUnauthorized(const CallPtr& pCall);
    void onAccessTokenRenewed(bool renewed);
    void scheduleRetry(const CallPtr& pCall, std::chrono::milliseconds delay);
    /// Finishes `calls` with `result`. Their Replies get it last, once this
    /// Client is done with its own state: a slot connected to finished() may
    /// delete the Client, so callers return right after this.
    void complete(const QList<CallPtr>& calls, const JsonResult& result);
    void detach(Reply* pReply);
    /// Drops a call from every queue, aborting its network reply if any.
    void forget(const CallPtr& pCall);

    const QUrl m_baseUrl;
    QNetworkAccessManager* const m_pNetworkAccessManager;
    ClientOptions m_options;
    TokenSource* m_pTokenSource = nullptr;

    /// Every call that has not finished, whatever it waits for.
    std::vector<CallPtr> m_calls;
    /// Calls waiting for a free slot, in order.
    QList<CallPtr> m_queue;
    /// Calls waiting for the renewal of the access token.
    QList<CallPtr> m_waitingForToken;
    int m_inFlight = 0;
    bool m_renewingToken = false;
};

} // namespace crate
