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

void Reply::finish(const JsonResult& result) {
    if (m_finished) {
        return;
    }
    m_finished = true;
    emit finished(result);
    deleteLater();
}

Client::Client(const QUrl& baseUrl,
        QNetworkAccessManager* pNetworkAccessManager,
        ClientOptions options,
        QObject* parent)
        : QObject(parent),
          m_baseUrl(baseUrl),
          m_pNetworkAccessManager(pNetworkAccessManager),
          m_options(std::move(options)) {
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
    m_pTokenSource = pTokenSource;
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

    QUrl url = m_baseUrl;
    QString basePath = m_baseUrl.path();
    while (basePath.endsWith(QLatin1Char('/'))) {
        basePath.chop(1);
    }
    url.setPath(basePath + path);
    url.setQuery(QString());
    url.setFragment(QString());
    if (!options.query.isEmpty()) {
        url.setQuery(options.query);
    }

    QString sharedKey;
    if (method == Method::Get) {
        sharedKey = (options.authenticated ? QStringLiteral("auth ") : QStringLiteral("anon ")) +
                url.toString(QUrl::FullyEncoded);
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
    pCall->sharedKey = sharedKey;
    pCall->subscribers.append(pReply);
    m_calls.push_back(pCall);
    m_queue.append(pCall);
    pump();
    return pReply;
}

void Client::pump() {
    while (m_inFlight < m_options.maxConcurrentRequests && !m_queue.isEmpty()) {
        const CallPtr pCall = m_queue.takeFirst();
        if (pCall->authenticated && m_renewingToken) {
            m_waitingForToken.append(pCall);
            continue;
        }
        send(pCall);
    }
}

void Client::send(const CallPtr& pCall) {
    QNetworkRequest request(pCall->url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Crate-App", QByteArray("decks/") + kApplicationVersion);
    if (!m_options.deviceFingerprint.isEmpty()) {
        request.setRawHeader("X-Device-Fingerprint", m_options.deviceFingerprint.toUtf8());
    }
    pCall->sentToken.clear();
    if (pCall->authenticated && m_pTokenSource) {
        pCall->sentToken = m_pTokenSource->accessToken();
        if (!pCall->sentToken.isEmpty()) {
            request.setRawHeader("Authorization", "Bearer " + pCall->sentToken.toUtf8());
        }
    }
    if (pCall->method == Method::Post || pCall->method == Method::Put) {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                QByteArrayLiteral("application/json"));
    }
    // The bearer token must never follow a redirect to another host.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::SameOriginRedirectPolicy);
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
    ++m_inFlight;
    pCall->pNetworkReply = pNetworkReply;

    const std::weak_ptr<Call> weakCall = pCall;
    connect(pNetworkReply,
            &QNetworkReply::downloadProgress,
            this,
            [weakCall](qint64 bytesReceived, qint64 bytesTotal) {
                if (const CallPtr pActive = weakCall.lock()) {
                    for (const QPointer<Reply>& pReply : std::as_const(pActive->subscribers)) {
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

void Client::onNetworkReplyFinished(const CallPtr& pCall) {
    QNetworkReply* const pNetworkReply = pCall->pNetworkReply;
    VERIFY_OR_DEBUG_ASSERT(pNetworkReply) {
        return;
    }
    pCall->pNetworkReply = nullptr;
    pNetworkReply->disconnect(this);
    pNetworkReply->deleteLater();
    --m_inFlight;

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
    if (status < 200 || status >= 300) {
        kLogger.warning() << "Request to" << pCall->url.path() << "failed with HTTP" << status;
        complete({pCall}, httpError(status, body));
        return;
    }
    if (body.trimmed().isEmpty()) {
        complete({pCall}, QJsonDocument());
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        complete({pCall},
                Error::decode(QStringLiteral("The answer is not JSON: %1")
                                .arg(parseError.errorString())));
        return;
    }
    complete({pCall}, document);
}

void Client::onUnauthorized(const CallPtr& pCall) {
    if (!m_pTokenSource) {
        complete({pCall}, Error::auth(QStringLiteral("Not signed in")));
        return;
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
    kLogger.debug() << "Renewing the access token";
    const QPointer<Client> pGuard(this);
    m_pTokenSource->renewAccessToken([pGuard](bool renewed) {
        if (pGuard) {
            pGuard->onAccessTokenRenewed(renewed);
        }
    });
    // The renewal may have finished already and a slot may have deleted us.
    if (pGuard) {
        pump();
    }
}

void Client::onAccessTokenRenewed(bool renewed) {
    m_renewingToken = false;
    const QList<CallPtr> waiting = std::exchange(m_waitingForToken, {});
    if (renewed) {
        m_queue = waiting + m_queue;
        pump();
        return;
    }
    kLogger.info() << "The session could not be renewed";
    complete(waiting, Error::auth(QStringLiteral("The session could not be renewed")));
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

void Client::complete(const QList<CallPtr>& calls, const JsonResult& result) {
    QList<QPointer<Reply>> subscribers;
    for (const CallPtr& pCall : calls) {
        subscribers += pCall->subscribers;
        forget(pCall);
    }
    pump();
    // Nothing of this Client is used from here on.
    for (const QPointer<Reply>& pReply : std::as_const(subscribers)) {
        if (pReply) {
            pReply->finish(result);
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
        --m_inFlight;
    }
    m_calls.erase(std::remove(m_calls.begin(), m_calls.end(), pKept), m_calls.end());
}

} // namespace crate

#include "moc_crateclient.cpp"
