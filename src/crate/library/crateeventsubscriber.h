#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <chrono>
#include <functional>
#include <optional>

#include "crate/library/cratelibrarystore.h"

class QNetworkReply;

namespace crate {
class Client;
} // namespace crate

namespace crate::library {

class LibrarySync;

/// Options of crate::library::EventSubscriber.
struct EventSubscriberOptions {
    /// Delay before the first reconnection; it doubles up to maxDelay.
    std::chrono::milliseconds initialDelay{1000};
    std::chrono::milliseconds maxDelay{60000};
    /// Runs `callback` after `delay`. Defaults to a single-shot QTimer;
    /// tests replace it to control time.
    std::function<void(std::chrono::milliseconds delay, const std::function<void()>& callback)>
            schedule;
};

/// Listens to the server's cache invalidations (`GET /api/cache/events`,
/// server-sent events) and marks the containers they affect as dirty (plan
/// F3.C-PR1). Each event is `id: <n>` and `data: <scope>`; a heartbeat event
/// keeps the connection open. When the stream ends it reconnects with a
/// growing delay and sends the last id as Last-Event-ID, so the server
/// replays what was missed.
///
/// Only started when the server's capabilities announce `library_cursor`.
class EventSubscriber : public QObject {
    Q_OBJECT
  public:
    using Options = EventSubscriberOptions;

    EventSubscriber(Client* pClient,
            LibrarySync* pSync,
            Options options = Options(),
            QObject* parent = nullptr);
    ~EventSubscriber() override;

    void start();
    void stop();
    bool isRunning() const {
        return m_running;
    }

    const QString& lastEventId() const {
        return m_lastEventId;
    }

    /// What a scope of the server's invalidations means for the library, or
    /// nullopt if it does not affect it.
    static std::optional<Invalidation> invalidationOf(const QString& scope);

  signals:
    /// An event arrived with these scopes.
    void eventReceived(const QStringList& scopes);

  private:
    void connectStream();
    void onReadyRead();
    void onFinished();
    void processLine(const QByteArray& line);
    void dispatch();
    void scheduleReconnect();

    const QPointer<Client> m_pClient;
    const QPointer<LibrarySync> m_pSync;
    Options m_options;
    QPointer<QNetworkReply> m_pStream;
    bool m_running = false;
    std::chrono::milliseconds m_delay;
    /// Bumped on stop(), so a reconnection scheduled before does nothing.
    int m_generation = 0;
    QString m_lastEventId;
    QByteArray m_buffer;
    QString m_eventType;
    QStringList m_data;
};

} // namespace crate::library
