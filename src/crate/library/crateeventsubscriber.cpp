#include "crate/library/crateeventsubscriber.h"

#include <QNetworkReply>
#include <QTimer>
#include <algorithm>

#include "crate/client/crateclient.h"
#include "crate/library/cratelibrarysync.h"
#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::EventSubscriber");

const QString kEventsPath = QStringLiteral("/api/cache/events");

} // namespace

EventSubscriber::EventSubscriber(
        Client* pClient, LibrarySync* pSync, Options options, QObject* parent)
        : QObject(parent),
          m_pClient(pClient),
          m_pSync(pSync),
          m_options(std::move(options)),
          m_delay(m_options.initialDelay) {
    if (!m_options.schedule) {
        m_options.schedule = [this](std::chrono::milliseconds delay,
                                     const std::function<void()>& callback) {
            QTimer::singleShot(delay, this, callback);
        };
    }
}

EventSubscriber::~EventSubscriber() {
    stop();
}

void EventSubscriber::start() {
    if (m_running) {
        return;
    }
    m_running = true;
    m_delay = m_options.initialDelay;
    connectStream();
}

void EventSubscriber::stop() {
    m_running = false;
    ++m_generation;
    if (QNetworkReply* pStream = m_pStream.data()) {
        m_pStream = nullptr;
        disconnect(pStream, nullptr, this, nullptr);
        pStream->abort();
        pStream->deleteLater();
    }
}

std::optional<Invalidation> EventSubscriber::invalidationOf(const QString& scope) {
    using Scope = Invalidation::Scope;
    // The scopes of the server's CacheInvalidationMiddleware
    // (app/crate/api/cache_events.py).
    if (scope.startsWith(QLatin1String("playlist:"))) {
        return Invalidation{Scope::Container, QStringLiteral("playlist"), scope.mid(9)};
    }
    if (scope.startsWith(QLatin1String("crate:"))) {
        return Invalidation{Scope::Container, QStringLiteral("crate"), scope.mid(6)};
    }
    if (scope == QLatin1String("playlists") || scope == QLatin1String("crates")) {
        return Invalidation{Scope::Tree, QString(), QString()};
    }
    if (scope == QLatin1String("likes")) {
        return Invalidation{Scope::Kind, QStringLiteral("likes"), QString()};
    }
    if (scope == QLatin1String("history")) {
        return Invalidation{Scope::Kind, QStringLiteral("recent"), QString()};
    }
    // Tags, scans, imports and crate changes: any track may have changed.
    if (scope == QLatin1String("library") || scope == QLatin1String("*")) {
        return Invalidation{Scope::Everything, QString(), QString()};
    }
    return std::nullopt;
}

void EventSubscriber::connectStream() {
    if (!m_running || !m_pClient) {
        return;
    }
    RequestOptions options;
    if (!m_lastEventId.isEmpty()) {
        options.headers.append({QByteArrayLiteral("Last-Event-ID"), m_lastEventId.toUtf8()});
    }
    QNetworkReply* pStream = m_pClient->openStream(kEventsPath, options);
    if (!pStream) {
        scheduleReconnect();
        return;
    }
    m_pStream = pStream;
    m_buffer.clear();
    m_eventType.clear();
    m_data.clear();
    connect(pStream, &QIODevice::readyRead, this, &EventSubscriber::onReadyRead);
    connect(pStream, &QNetworkReply::finished, this, &EventSubscriber::onFinished);
}

void EventSubscriber::onReadyRead() {
    QNetworkReply* pStream = m_pStream.data();
    if (!pStream) {
        return;
    }
    const int status = pStream->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        // An error page is not an event stream
        pStream->readAll();
        return;
    }
    m_buffer += pStream->readAll();
    qsizetype start = 0;
    for (qsizetype end = m_buffer.indexOf('\n', start); end >= 0;
            end = m_buffer.indexOf('\n', start)) {
        QByteArray line = m_buffer.mid(start, end - start);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        start = end + 1;
        processLine(line);
        if (!m_pStream) {
            return;
        }
    }
    m_buffer.remove(0, start);
}

void EventSubscriber::processLine(const QByteArray& line) {
    if (line.isEmpty()) {
        dispatch();
        return;
    }
    if (line.startsWith(':')) {
        return;
    }
    const qsizetype colon = line.indexOf(':');
    const QByteArray field = colon < 0 ? line : line.left(colon);
    QByteArray value = colon < 0 ? QByteArray() : line.mid(colon + 1);
    if (value.startsWith(' ')) {
        value.remove(0, 1);
    }
    if (field == "event") {
        m_eventType = QString::fromUtf8(value);
    } else if (field == "data") {
        m_data.append(QString::fromUtf8(value));
    } else if (field == "id" && !value.contains('\0')) {
        m_lastEventId = QString::fromUtf8(value);
    }
}

void EventSubscriber::dispatch() {
    const QString type = std::exchange(m_eventType, QString());
    const QStringList scopes = std::exchange(m_data, QStringList());
    if (scopes.isEmpty() || (!type.isEmpty() && type != QLatin1String("message"))) {
        // Heartbeats and events of other types
        return;
    }
    // The server is talking: start the backoff over.
    m_delay = m_options.initialDelay;
    const QPointer<EventSubscriber> self(this);
    emit eventReceived(scopes);
    if (!self || !m_pSync) {
        return;
    }
    for (const QString& scope : scopes) {
        if (const std::optional<Invalidation> invalidation = invalidationOf(scope)) {
            m_pSync->markDirty(*invalidation);
        }
    }
}

void EventSubscriber::onFinished() {
    QNetworkReply* pStream = m_pStream.data();
    if (!pStream) {
        return;
    }
    onReadyRead();
    m_pStream = nullptr;
    const int status = pStream->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (pStream->error() != QNetworkReply::NoError) {
        kLogger.info() << "The Crate event stream ended:" << pStream->errorString()
                       << "HTTP" << status;
    }
    pStream->deleteLater();
    scheduleReconnect();
}

void EventSubscriber::scheduleReconnect() {
    if (!m_running) {
        return;
    }
    const std::chrono::milliseconds delay = m_delay;
    m_delay = std::min(m_delay * 2, m_options.maxDelay);
    const int generation = m_generation;
    const QPointer<EventSubscriber> self(this);
    m_options.schedule(delay, [self, generation] {
        if (self && self->m_running && self->m_generation == generation) {
            self->connectStream();
        }
    });
}

} // namespace crate::library

#include "moc_crateeventsubscriber.cpp"
