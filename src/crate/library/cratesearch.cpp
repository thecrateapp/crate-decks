#include "crate/library/cratesearch.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QThread>
#include <QUrlQuery>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

#include "crate/client/crateclient.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarystore.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::Search");

/// The server searches nothing shorter (`api_search`).
constexpr int kMinimumQueryLength = 2;

struct Stored {
    std::optional<Error> error;
    qint64 containerId = 0;
};

} // namespace

const QString Search::kKind = kSearchContainerKind;

Search::Search(const QString& serverId,
        Client* pClient,
        mixxx::DbConnectionPoolPtr pDbConnectionPool,
        Options options,
        QObject* parent)
        : QObject(parent),
          m_serverId(serverId),
          m_pClient(pClient),
          m_pDbConnectionPool(std::move(pDbConnectionPool)),
          m_options(std::move(options)) {
    m_pool.setMaxThreadCount(1);
    m_debounce.setSingleShot(true);
    m_debounce.setTimerType(Qt::PreciseTimer);
    m_debounce.setInterval(m_options.debounce);
    connect(&m_debounce, &QTimer::timeout, this, &Search::send);
}

Search::~Search() {
    cancel();
    m_pool.waitForDone();
}

void Search::search(const QString& query) {
    cancel();
    m_query = query;
    if (query.trimmed().size() < kMinimumQueryLength) {
        return;
    }
    m_debounce.start();
}

void Search::cancel() {
    ++m_generation;
    m_debounce.stop();
    if (Reply* pReply = m_pReply.data()) {
        m_pReply = nullptr;
        // Deleting the reply aborts its request without a signal
        pReply->disconnect(this);
        pReply->cancel();
    }
}

bool Search::isBusy() const {
    return m_debounce.isActive() || m_pReply || m_storing > 0;
}

void Search::send() {
    if (!m_pClient) {
        emit searchFailed(m_query, Error::network(QStringLiteral("The Crate server is gone")));
        return;
    }
    RequestOptions options;
    // Verbatim: the DJ grammar (bpm:120-128 key:8A) is the server's
    options.query.addQueryItem(QStringLiteral("fields"), QStringLiteral("dj"));
    options.query.addQueryItem(QStringLiteral("q"),
            QString::fromLatin1(QUrl::toPercentEncoding(m_query)));
    options.query.addQueryItem(QStringLiteral("limit"), QString::number(m_options.limit));
    options.rawBody = true;
    Reply* pReply = m_pClient->get(searchPath(), options);
    m_pReply = pReply;
    const quint64 generation = m_generation;
    const QString query = m_query;
    connect(pReply,
            &Reply::finished,
            this,
            [this, pReply, generation, query](const JsonResult& result) {
                if (m_pReply == pReply) {
                    m_pReply = nullptr;
                }
                if (generation != m_generation) {
                    return;
                }
                if (!result.ok()) {
                    kLogger.warning() << "The Crate search failed:" << result.error().message;
                    emit searchFailed(query, result.error());
                    return;
                }
                onReply(pReply, generation, query);
            });
}

void Search::onReply(Reply* pReply, quint64 generation, const QString& query) {
    ++m_storing;
    auto* pWatcher = new QFutureWatcher<Stored>(this);
    connect(pWatcher,
            &QFutureWatcherBase::finished,
            this,
            [this, pWatcher, generation, query] {
                pWatcher->deleteLater();
                --m_storing;
                const Stored stored = pWatcher->result();
                if (generation != m_generation) {
                    return;
                }
                if (stored.error) {
                    kLogger.warning() << "Cannot store the Crate search results:"
                                      << stored.error->message;
                    emit searchFailed(query, *stored.error);
                    return;
                }
                emit resultsReady(stored.containerId, query);
            });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_pDbConnectionPool,
                    body = pReply->body(),
                    serverId = m_serverId,
                    query,
                    options = m_options]() -> Stored {
                if (options.onParse) {
                    options.onParse();
                }
                const Result<QList<RemoteTrack>> tracks = parseSearchTracks(body);
                if (!tracks.ok()) {
                    return {tracks.error(), 0};
                }
                const mixxx::DbConnectionPooler pooler(pPool);
                const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
                const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                Stored stored;
                WriteStatus status = WriteStatus::Busy;
                for (int attempt = 1; attempt <= std::max(1, options.writeAttempts); ++attempt) {
                    status = storeSearchResults(
                            database, serverId, query, tracks.value(), now, &stored.containerId);
                    if (status != WriteStatus::Busy) {
                        break;
                    }
                    QThread::msleep(static_cast<unsigned long>(
                            options.writeRetryDelay.count() * attempt));
                }
                if (status != WriteStatus::Ok) {
                    stored.error = Error::storage(QStringLiteral(
                            "Cannot store the search results in the database"));
                }
                return stored;
            }));
}

} // namespace crate::library

#include "moc_cratesearch.cpp"
