#include "crate/client/crateclient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "crate/appidentity.h"
#include "util/assert.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::Client");

const ConfigKey kDeviceFingerprintKey(
        QStringLiteral("[CrateDecks]"), QStringLiteral("DeviceFingerprint"));

bool isRetryableStatus(int status) {
    return status == 429 || status == 502 || status == 503 || status == 504;
}

/// Retry-After as delta-seconds, the form the Crate server sends.
std::optional<std::chrono::milliseconds> parseRetryAfter(const QByteArray& value) {
    bool ok = false;
    const qint64 seconds = value.trimmed().toLongLong(&ok);
    if (!ok || seconds < 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(seconds * 1000);
}

std::chrono::milliseconds backoffDelay(const ClientOptions& options, int attemptsMade) {
    const double random = options.random
            ? options.random()
            : QRandomGenerator::global()->generateDouble();
    const double factor = std::ldexp(1.0, attemptsMade - 1) * (0.5 + random);
    return std::chrono::milliseconds(
            std::llround(static_cast<double>(options.retryBaseDelay.count()) * factor));
}

/// The error of an HTTP answer, with the detail the server put in its
/// FastAPI body: a text, {code, message} or a list of validation errors.
Error httpError(int status, const QByteArray& body) {
    QString code;
    QString message;
    const QJsonValue detail =
            QJsonDocument::fromJson(body).object().value(QStringLiteral("detail"));
    if (detail.isString()) {
        message = detail.toString();
    } else if (detail.isObject()) {
        const QJsonObject object = detail.toObject();
        code = object.value(QStringLiteral("code")).toString();
        message = object.value(QStringLiteral("message")).toString();
    } else if (detail.isArray() && !detail.toArray().isEmpty()) {
        message = detail.toArray().first().toObject().value(QStringLiteral("msg")).toString();
    }
    if (message.isEmpty()) {
        message = QStringLiteral("HTTP %1").arg(status);
    }
    return Error::http(status, code, message);
}

} // namespace

struct Client::Call {
    Method method = Method::Get;
    QUrl url;
    QByteArray body;
    bool authenticated = true;
    /// RequestOptions::bearerToken.
    std::optional<QString> bearerToken;
    QList<std::pair<QByteArray, QByteArray>> headers;
    bool rawBody = false;
    /// RequestOptions::streamBody.
    bool streamBody = false;
    /// A streamed 2xx answer has told its headers to the Replies.
    bool streaming = false;
    /// Set for GETs, which share one call while it is active.
    QString sharedKey;
    int attemptsMade = 0;
    /// A call gets at most one renewal of the access token.
    bool renewalUsed = false;
    QString sentToken;
    QList<QPointer<Reply>> subscribers;
    QPointer<QNetworkReply> pNetworkReply;
    bool done = false;

    bool isIdempotent() const {
        return method != Method::Post;
    }
    /// Streams have no ClientOptions::maxConcurrentRequests slot.
    bool takesSlot() const {
        return !streamBody;
    }
};

QString deviceFingerprint(UserSettingsPointer pConfig) {
    VERIFY_OR_DEBUG_ASSERT(pConfig) {
        return QString();
    }
    QString fingerprint = pConfig->getValueString(kDeviceFingerprintKey).trimmed();
    if (fingerprint.isEmpty()) {
        fingerprint = QUuid::createUuid().toString(QUuid::WithoutBraces);
        pConfig->set(kDeviceFingerprintKey, ConfigValue(fingerprint));
    }
    return fingerprint;
}

Reply::Reply(Client* pClient)
        : QObject(pClient),
          m_pClient(pClient) {
}

Reply::~Reply() {
    if (!m_finished && m_pClient) {
        m_pClient->detach(this);
    }
}

void Reply::cancel() {
    if (m_finished) {
        return;
    }
    if (m_pClient) {
        m_pClient->detach(this);
    }
    finish(Error::cancelled());
}

QByteArray Reply::rawHeader(const QByteArray& name) const {
    for (const auto& [headerName, value] : m_response.headers) {
        if (headerName.compare(name, Qt::CaseInsensitive) == 0) {
            return value;
        }
    }
    return QByteArray();
}

void Reply::finish(const JsonResult& result) {
    finish(result, Response());
}

void Reply::finish(const JsonResult& result, const Response& response) {
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_response = response;
    // Posted before the signal and nothing after it: a slot may delete this
    // Reply or the Client that owns it.
    deleteLater();
    emit finished(result);
}

Client::Client(const QUrl& baseUrl,
        QNetworkAccessManager* pNetworkAccessManager,
        const ClientOptions& options,
        QObject* parent)
        : QObject(parent),
          m_baseUrl(baseUrl),
          m_pNetworkAccessManager(pNetworkAccessManager),
          m_options(options) {
}

Client::~Client() {
    for (const CallPtr& pCall : m_calls) {
        pCall->done = true;
        for (const QPointer<Reply>& pReply : std::as_const(pCall->subscribers)) {
            if (pReply) {
                pReply->m_pClient = nullptr;
            }
        }
        if (QNetworkReply* pNetworkReply = pCall->pNetworkReply) {
            pNetworkReply->disconnect(this);
            pNetworkReply->abort();
            pNetworkReply->deleteLater();
        }
    }
    m_calls.clear();
    m_queue.clear();
    m_waitingForToken.clear();
}

void Client::setTokenSource(TokenSource* pTokenSource) {
    if (pTokenSource == m_pTokenSource) {
        return;
    }
    m_pTokenSource = pTokenSource;
    m_rejectedToken.reset();
    if (!m_renewingToken) {
        return;
    }
    // The renewal in flight belongs to the previous session: its answer is
    // ignored. The requests it rejected fail; the ones that only waited go
    // out with the new session.
    m_renewingToken = false;
    ++m_renewalGeneration;
    QList<CallPtr> rejected;
    QList<CallPtr> waited;
    const QList<CallPtr> waiting = std::exchange(m_waitingForToken, {});
    for (const CallPtr& pCall : waiting) {
        (pCall->renewalUsed ? rejected : waited).append(pCall);
    }
    m_queue = waited + m_queue;
    complete(rejected, Error::auth(QStringLiteral("The session changed")));
}

Reply* Client::get(const QString& path, const RequestOptions& options) {
    return request(Method::Get, path, QByteArray(), options);
}

Reply* Client::post(const QString& path,
        const QJsonDocument& body,
        const RequestOptions& options) {
    return request(Method::Post, path, body.toJson(QJsonDocument::Compact), options);
}

Reply* Client::put(const QString& path,
        const QJsonDocument& body,
        const RequestOptions& options) {
    return request(Method::Put, path, body.toJson(QJsonDocument::Compact), options);
}

Reply* Client::del(const QString& path, const RequestOptions& options) {
    return request(Method::Delete, path, QByteArray(), options);
}

Reply* Client::request(Method method,
        const QString& path,
        const QByteArray& body,
        const RequestOptions& options) {
    auto* pReply = new Reply(this);
    VERIFY_OR_DEBUG_ASSERT(path.startsWith(QLatin1Char('/')) &&
            !path.contains(QStringLiteral("://"))) {
        const Error error = Error::network(QStringLiteral("Invalid API path: %1").arg(path));
        QTimer::singleShot(0, pReply, [pReply, error] { pReply->finish(error); });
        return pReply;
    }

    const QUrl url = urlOf(path, options.query);

    QString sharedKey;
    if (method == Method::Get && !options.bearerToken && !options.streamBody) {
        sharedKey = (options.authenticated ? QStringLiteral("auth ") : QStringLiteral("anon ")) +
                (options.rawBody ? QStringLiteral("raw ") : QString()) +
                url.toString(QUrl::FullyEncoded);
        for (const auto& [name, value] : options.headers) {
            sharedKey += QLatin1Char('\n') + QString::fromLatin1(name) + QStringLiteral(": ") +
                    QString::fromLatin1(value);
        }
        for (const CallPtr& pActive : m_calls) {
            if (pActive->sharedKey == sharedKey) {
                pActive->subscribers.append(pReply);
                return pReply;
            }
        }
    }

    auto pCall = std::make_shared<Call>();
    pCall->method = method;
    pCall->url = url;
    pCall->body = body;
    pCall->authenticated = options.authenticated;
    pCall->bearerToken = options.bearerToken;
    pCall->headers = options.headers;
    pCall->rawBody = options.rawBody;
    pCall->streamBody = options.streamBody && method == Method::Get;
    pCall->sharedKey = sharedKey;
    pCall->subscribers.append(pReply);
    m_calls.push_back(pCall);
    m_queue.append(pCall);
    pump();
    return pReply;
}

void Client::pump() {
    for (int i = 0; i < m_queue.size();) {
        const CallPtr pCall = m_queue.at(i);
        if (pCall->takesSlot() && m_inFlight >= m_options.maxConcurrentRequests) {
            ++i;
            continue;
        }
        m_queue.removeAt(i);
        if (pCall->authenticated && !pCall->bearerToken && m_renewingToken) {
            m_waitingForToken.append(pCall);
            continue;
        }
        send(pCall);
    }
}

QUrl Client::urlOf(const QString& path, const QUrlQuery& query) const {
    QUrl url = m_baseUrl;
    QString basePath = m_baseUrl.path();
    while (basePath.endsWith(QLatin1Char('/'))) {
        basePath.chop(1);
    }
    url.setPath(basePath + path);
    url.setQuery(QString());
    url.setFragment(QString());
    if (!query.isEmpty()) {
        url.setQuery(query);
    }
    return url;
}

void Client::setHeaders(QNetworkRequest* pRequest,
        const QList<std::pair<QByteArray, QByteArray>>& extraHeaders,
        const QString& token) const {
    for (const auto& [name, value] : extraHeaders) {
        // Only the session's token goes out
        if (name.compare(QByteArrayLiteral("Authorization"), Qt::CaseInsensitive) != 0) {
            pRequest->setRawHeader(name, value);
        }
    }
    pRequest->setRawHeader("Accept", "application/json");
    pRequest->setRawHeader("X-Crate-App", QByteArray("decks/") + kApplicationVersion);
    if (!m_options.deviceFingerprint.isEmpty()) {
        pRequest->setRawHeader("X-Device-Fingerprint", m_options.deviceFingerprint.toUtf8());
    }
    if (!token.isEmpty()) {
        pRequest->setRawHeader("Authorization", "Bearer " + token.toUtf8());
    }
    // The bearer token must never follow a redirect to another host.
    pRequest->setAttribute(QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::SameOriginRedirectPolicy);
}

QNetworkReply* Client::openStream(const QString& path, const RequestOptions& options) {
    VERIFY_OR_DEBUG_ASSERT(path.startsWith(QLatin1Char('/')) &&
            !path.contains(QStringLiteral("://"))) {
        return nullptr;
    }
    QNetworkRequest request(urlOf(path, options.query));
    QString token;
    if (options.bearerToken) {
        token = *options.bearerToken;
    } else if (options.authenticated && m_pTokenSource) {
        token = m_pTokenSource->accessToken();
    }
    setHeaders(&request, options.headers, token);
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setTransferTimeout(0);
    return m_pNetworkAccessManager->get(request);
}

void Client::send(const CallPtr& pCall) {
    QNetworkRequest request(pCall->url);
    pCall->sentToken.clear();
    if (pCall->bearerToken) {
        pCall->sentToken = *pCall->bearerToken;
    } else if (pCall->authenticated && m_pTokenSource) {
        pCall->sentToken = m_pTokenSource->accessToken();
    }
    setHeaders(&request, pCall->headers, pCall->sentToken);
    if (pCall->method == Method::Post || pCall->method == Method::Put) {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                QByteArrayLiteral("application/json"));
    }
    if (pCall->streamBody) {
        request.setRawHeader("Accept", "*/*");
    }
    request.setTransferTimeout(static_cast<int>(m_options.transferTimeout.count()));

    QNetworkReply* pNetworkReply = nullptr;
    switch (pCall->method) {
    case Method::Get:
        pNetworkReply = m_pNetworkAccessManager->get(request);
        break;
    case Method::Post:
        pNetworkReply = m_pNetworkAccessManager->post(request, pCall->body);
        break;
    case Method::Put:
        pNetworkReply = m_pNetworkAccessManager->put(request, pCall->body);
        break;
    case Method::Delete:
        pNetworkReply = m_pNetworkAccessManager->deleteResource(request);
        break;
    }
    ++pCall->attemptsMade;
    if (pCall->takesSlot()) {
        ++m_inFlight;
    }
    pCall->pNetworkReply = pNetworkReply;

    const std::weak_ptr<Call> weakCall = pCall;
    if (pCall->streamBody) {
        // The headers may come before any of the body.
        const auto deliver = [this, weakCall] {
            if (const CallPtr pActive = weakCall.lock()) {
                deliverStream(pActive);
            }
        };
        connect(pNetworkReply, &QNetworkReply::metaDataChanged, this, deliver);
        connect(pNetworkReply, &QIODevice::readyRead, this, deliver);
    }
    connect(pNetworkReply,
            &QNetworkReply::downloadProgress,
            this,
            [weakCall](qint64 bytesReceived, qint64 bytesTotal) {
                if (const CallPtr pActive = weakCall.lock()) {
                    // A copy: a slot may cancel or delete Replies meanwhile.
                    const QList<QPointer<Reply>> subscribers = pActive->subscribers;
                    for (const QPointer<Reply>& pReply : subscribers) {
                        if (pReply) {
                            emit pReply->progress(bytesReceived, bytesTotal);
                        }
                    }
                }
            });
    connect(pNetworkReply, &QNetworkReply::finished, this, [this, weakCall] {
        if (const CallPtr pActive = weakCall.lock()) {
            onNetworkReplyFinished(pActive);
        }
    });
}

bool Client::deliverStream(const CallPtr& pCall) {
    QNetworkReply* const pNetworkReply = pCall->pNetworkReply;
    if (!pNetworkReply || pCall->done) {
        return false;
    }
    if (!pCall->streaming) {
        const QVariant statusAttribute =
                pNetworkReply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
        if (status < 200 || status >= 300) {
            // An error answer, a retry or a renewal: its small body is read
            // when it finishes.
            return true;
        }
        pCall->streaming = true;
        Reply::Response response;
        response.status = status;
        response.headers = pNetworkReply->rawHeaderPairs();
        const QPointer<Client> pGuard(this);
        const QList<QPointer<Reply>> subscribers = pCall->subscribers;
        for (const QPointer<Reply>& pReply : subscribers) {
            if (pReply) {
                pReply->m_response = response;
                emit pReply->headersReceived();
            }
            if (!pGuard || pCall->done) {
                return false;
            }
        }
    }
    const QByteArray chunk = pNetworkReply->readAll();
    if (chunk.isEmpty()) {
        return true;
    }
    const QPointer<Client> pGuard(this);
    const QList<QPointer<Reply>> subscribers = pCall->subscribers;
    for (const QPointer<Reply>& pReply : subscribers) {
        if (pReply) {
            emit pReply->bodyReceived(chunk);
        }
        if (!pGuard || pCall->done) {
            return false;
        }
    }
    return true;
}

void Client::onNetworkReplyFinished(const CallPtr& pCall) {
    // What a stream has not handed over yet goes first, also when the
    // transfer broke off: the bytes that came are good. A 2xx with an empty
    // body tells its headers here.
    if (pCall->streamBody && !deliverStream(pCall)) {
        return;
    }
    QNetworkReply* const pNetworkReply = pCall->pNetworkReply;
    VERIFY_OR_DEBUG_ASSERT(pNetworkReply) {
        return;
    }
    pCall->pNetworkReply = nullptr;
    pNetworkReply->disconnect(this);
    pNetworkReply->deleteLater();
    if (pCall->takesSlot()) {
        --m_inFlight;
    }

    const QVariant statusAttribute =
            pNetworkReply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!statusAttribute.isValid()) {
        kLogger.warning() << "Request to" << pCall->url.path()
                          << "failed:" << pNetworkReply->errorString();
        complete({pCall}, Error::network(pNetworkReply->errorString()));
        return;
    }
    const int status = statusAttribute.toInt();
    const QByteArray body = pNetworkReply->readAll();
    // Qt keeps the status of the headers when the transfer breaks off later,
    // or when a redirect is refused: that is not an answer.
    if (status < 400 && pNetworkReply->error() != QNetworkReply::NoError) {
        kLogger.warning() << "Request to" << pCall->url.path()
                          << "broke off:" << pNetworkReply->errorString();
        complete({pCall}, Error::network(pNetworkReply->errorString()));
        return;
    }

    if (status == 401 && pCall->bearerToken) {
        complete({pCall}, Error::auth(QStringLiteral("The server rejected the token")));
        return;
    }
    if (status == 401 && pCall->authenticated) {
        onUnauthorized(pCall);
        return;
    }
    if (isRetryableStatus(status) && pCall->isIdempotent() &&
            pCall->attemptsMade < m_options.maxAttempts) {
        std::optional<std::chrono::milliseconds> delay =
                backoffDelay(m_options, pCall->attemptsMade);
        if (const auto retryAfter = parseRetryAfter(pNetworkReply->rawHeader("Retry-After"))) {
            delay = *retryAfter <= m_options.maxRetryAfter
                    ? std::optional<std::chrono::milliseconds>(*retryAfter)
                    : std::nullopt;
        }
        if (delay) {
            kLogger.debug() << "Retrying" << pCall->url.path() << "after HTTP" << status;
            scheduleRetry(pCall, *delay);
            pump();
            return;
        }
    }
    Reply::Response response;
    response.status = status;
    response.headers = pNetworkReply->rawHeaderPairs();
    if (status == 304) {
        // Not Modified: the caller keeps what it has
        complete({pCall}, QJsonDocument(), response);
        return;
    }
    if (status < 200 || status >= 300) {
        kLogger.warning() << "Request to" << pCall->url.path() << "failed with HTTP" << status;
        complete({pCall}, httpError(status, body), response);
        return;
    }
    if (pCall->streamBody) {
        complete({pCall}, QJsonDocument(), response);
        return;
    }
    if (pCall->rawBody) {
        response.body = body;
        complete({pCall}, QJsonDocument(), response);
        return;
    }
    if (body.trimmed().isEmpty()) {
        complete({pCall}, QJsonDocument(), response);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        complete({pCall},
                Error::decode(QStringLiteral("The answer is not JSON: %1")
                                .arg(parseError.errorString())),
                response);
        return;
    }
    complete({pCall}, document, response);
}

void Client::onUnauthorized(const CallPtr& pCall) {
    if (!m_pTokenSource) {
        complete({pCall}, Error::auth(QStringLiteral("Not signed in")));
        return;
    }
    if (m_rejectedToken) {
        if (m_pTokenSource->accessToken() == *m_rejectedToken) {
            // Its renewal already failed: do not ask again for every request.
            complete({pCall}, Error::auth(QStringLiteral("The session could not be renewed")));
            return;
        }
        m_rejectedToken.reset();
    }
    if (pCall->renewalUsed) {
        complete({pCall}, Error::auth(QStringLiteral("The server rejected the renewed session")));
        return;
    }
    pCall->renewalUsed = true;
    if (!m_renewingToken && m_pTokenSource->accessToken() != pCall->sentToken) {
        // Another request renewed the token after this one was sent.
        m_queue.prepend(pCall);
        pump();
        return;
    }
    m_waitingForToken.append(pCall);
    if (m_renewingToken) {
        pump();
        return;
    }
    m_renewingToken = true;
    const quint64 generation = ++m_renewalGeneration;
    kLogger.debug() << "Renewing the access token";
    const QPointer<Client> pGuard(this);
    m_pTokenSource->renewAccessToken([pGuard, generation](TokenSource::Renewal renewal) {
        // Ignore the answer of a renewal that setTokenSource() abandoned.
        if (pGuard && pGuard->m_renewingToken && pGuard->m_renewalGeneration == generation) {
            pGuard->onAccessTokenRenewed(renewal);
        }
    });
    // The renewal may have finished already and a slot may have deleted us.
    if (pGuard) {
        pump();
    }
}

void Client::onAccessTokenRenewed(TokenSource::Renewal renewal) {
    m_renewingToken = false;
    const QList<CallPtr> waiting = std::exchange(m_waitingForToken, {});
    if (renewal == TokenSource::Renewal::Renewed) {
        m_queue = waiting + m_queue;
        pump();
        return;
    }
    if (renewal == TokenSource::Renewal::Unreachable) {
        // The session may still be valid: the next 401 tries again.
        kLogger.info() << "The server could not be reached to renew the session";
        complete(waiting,
                Error::network(QStringLiteral(
                        "The server could not be reached to renew the session")));
        return;
    }
    kLogger.info() << "The session could not be renewed";
    m_rejectedToken = m_pTokenSource ? m_pTokenSource->accessToken() : QString();
    // The queued authenticated requests would go out with the rejected token.
    QList<CallPtr> failed = waiting;
    for (const CallPtr& pCall : std::as_const(m_queue)) {
        if (pCall->authenticated) {
            failed.append(pCall);
        }
    }
    complete(failed, Error::auth(QStringLiteral("The session could not be renewed")));
}

void Client::scheduleRetry(const CallPtr& pCall, std::chrono::milliseconds delay) {
    const std::weak_ptr<Call> weakCall = pCall;
    const QPointer<Client> pGuard(this);
    auto retry = [pGuard, weakCall] {
        const CallPtr pActive = weakCall.lock();
        if (!pGuard || !pActive || pActive->done) {
            return;
        }
        pGuard->m_queue.append(pActive);
        pGuard->pump();
    };
    if (m_options.schedule) {
        m_options.schedule(delay, std::move(retry));
    } else {
        QTimer::singleShot(delay, this, std::move(retry));
    }
}

void Client::complete(const QList<CallPtr>& calls,
        const JsonResult& result,
        const Reply::Response& response) {
    QList<QPointer<Reply>> subscribers;
    for (const CallPtr& pCall : calls) {
        subscribers += pCall->subscribers;
        forget(pCall);
    }
    pump();
    // Nothing of this Client is used from here on.
    for (const QPointer<Reply>& pReply : std::as_const(subscribers)) {
        if (pReply) {
            pReply->finish(result, response);
        }
    }
}

void Client::detach(Reply* pReply) {
    CallPtr pCall;
    for (const CallPtr& pActive : m_calls) {
        const bool subscribed = std::any_of(pActive->subscribers.cbegin(),
                pActive->subscribers.cend(),
                [pReply](const QPointer<Reply>& pSubscriber) {
                    return pSubscriber.data() == pReply;
                });
        if (subscribed) {
            pCall = pActive;
            break;
        }
    }
    if (!pCall) {
        return;
    }
    pCall->subscribers.removeIf([pReply](const QPointer<Reply>& pSubscriber) {
        return pSubscriber.isNull() || pSubscriber.data() == pReply;
    });
    if (pCall->subscribers.isEmpty()) {
        forget(pCall);
        pump();
    }
}

void Client::forget(const CallPtr& pCall) {
    // Keep the call alive while it is removed from the containers.
    const CallPtr pKept = pCall;
    pKept->done = true;
    pKept->subscribers.clear();
    m_queue.removeAll(pKept);
    m_waitingForToken.removeAll(pKept);
    if (QNetworkReply* const pNetworkReply = pKept->pNetworkReply) {
        pKept->pNetworkReply = nullptr;
        pNetworkReply->disconnect(this);
        pNetworkReply->abort();
        pNetworkReply->deleteLater();
        if (pKept->takesSlot()) {
            --m_inFlight;
        }
    }
    m_calls.erase(std::remove(m_calls.begin(), m_calls.end(), pKept), m_calls.end());
}

} // namespace crate

#include "moc_crateclient.cpp"
