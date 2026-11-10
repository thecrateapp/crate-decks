#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "crate/client/crateresult.h"
#include "preferences/usersettings.h"

class QNetworkAccessManager;
class QNetworkRequest;

namespace crate {

class Client;

/// Gives the Client the access token of the session and renews it when the
/// server rejects it. crate::Session (F2.C-PR2) implements it.
class TokenSource {
  public:
    enum class Renewal {
        /// accessToken() returns the new token.
        Renewed,
        /// The session is over (revoked or expired): the user signs in again.
        SessionEnded,
        /// The server could not be reached; the session may still be valid.
        Unreachable,
    };

    virtual ~TokenSource() = default;

    /// Access token for the Authorization header; empty when signed out.
    virtual QString accessToken() const = 0;

    /// Starts renewing the access token and calls `done` once with the
    /// outcome. The Client never asks for a second renewal while one is in
    /// flight.
    virtual void renewAccessToken(const std::function<void(Renewal renewal)>& done) = 0;
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
    std::function<void(std::chrono::milliseconds delay, const std::function<void()>& callback)>
            schedule;
    /// A number in [0, 1) for the jitter of the backoff. Defaults to
    /// QRandomGenerator; tests replace it.
    std::function<double()> random;
};

struct RequestOptions {
    QUrlQuery query;
    /// Sends the access token and renews it on 401. Off for the sign-in
    /// requests, whose 401 means wrong credentials.
    bool authenticated = true;
    /// Sent instead of the TokenSource's token, and never renewed: a 401 is
    /// an Auth error. For revoking a session that is already closed locally.
    std::optional<QString> bearerToken;
    /// More request headers, such as If-None-Match or Last-Event-ID. They
    /// never replace Authorization or the Client's own headers.
    QList<std::pair<QByteArray, QByteArray>> headers;
    /// Leaves the answer unparsed: finished() carries a null document and
    /// Reply::body() the bytes. For answers that are parsed off the main
    /// thread (design §9).
    bool rawBody = false;
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

    /// The HTTP status of the answer, 0 if there was none. Like the two
    /// below, it is set when finished() is emitted. A 304 Not Modified is not
    /// an error: it finishes with a null document.
    int httpStatus() const {
        return m_response.status;
    }
    /// A header of the answer, such as ETag; empty if it has none.
    QByteArray rawHeader(const QByteArray& name) const;
    /// The body of a RequestOptions::rawBody request.
    const QByteArray& body() const {
        return m_response.body;
    }

  signals:
    void finished(const crate::JsonResult& result);
    void progress(qint64 bytesReceived, qint64 bytesTotal);

  private:
    friend class Client;
    struct Response {
        int status = 0;
        QList<QNetworkReply::RawHeaderPair> headers;
        QByteArray body;
    };

    explicit Reply(Client* pClient);
    void finish(const JsonResult& result);
    void finish(const JsonResult& result, const Response& response);

    QPointer<Client> m_pClient;
    Response m_response;
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
            const ClientOptions& options,
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

    /// Opens a long-lived GET such as a server-sent event stream: the same
    /// headers as get() plus `Accept: text/event-stream`, no transfer
    /// timeout, and outside the limit of concurrent requests. The caller owns
    /// the reply and reads it as it arrives; a 401 is not renewed here.
    QNetworkReply* openStream(const QString& path,
            const RequestOptions& options = RequestOptions());

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
    QUrl urlOf(const QString& path, const QUrlQuery& query) const;
    /// The headers every request carries, after the caller's own.
    void setHeaders(QNetworkRequest* pRequest,
            const QList<std::pair<QByteArray, QByteArray>>& extraHeaders,
            const QString& token) const;
    void pump();
    void send(const CallPtr& pCall);
    void onNetworkReplyFinished(const CallPtr& pCall);
    void onUnauthorized(const CallPtr& pCall);
    void onAccessTokenRenewed(TokenSource::Renewal renewal);
    void scheduleRetry(const CallPtr& pCall, std::chrono::milliseconds delay);
    /// Finishes `calls` with `result`. Their Replies get it last, once this
    /// Client is done with its own state: a slot connected to finished() may
    /// delete the Client, so callers return right after this.
    void complete(const QList<CallPtr>& calls,
            const JsonResult& result,
            const Reply::Response& response = Reply::Response());
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
    /// Tells the answer of the current renewal from that of an abandoned one.
    quint64 m_renewalGeneration = 0;
    /// The access token whose renewal failed, while the TokenSource still
    /// returns it: requests rejected with it fail without renewing again.
    std::optional<QString> m_rejectedToken;
};

} // namespace crate
