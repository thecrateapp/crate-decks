#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <chrono>
#include <functional>

#include "crate/client/crateresult.h"
#include "util/db/dbconnectionpool.h"

namespace crate {
class Client;
class Reply;
} // namespace crate

namespace crate::library {

/// Options of crate::library::Search.
struct SearchOptions {
    /// Quiet time after the last keystroke before the server is asked.
    std::chrono::milliseconds debounce{250};
    /// Results asked for; the server gives at most 50.
    int limit = 50;
    /// Attempts of a write while the database is busy.
    int writeAttempts = 4;
    /// Wait before the first retry of a write; it grows with each one.
    std::chrono::milliseconds writeRetryDelay{25};
    /// Called on the worker thread before each answer is parsed. For tests.
    std::function<void()> onParse;
};

/// The DJ search of the Crate section of the library (plan F3.C-PR3, design
/// §7.2 and D9): what the user types goes verbatim to the server's
/// `GET /api/search?fields=dj&q=…&limit=50`, so its filters work
/// (`bpm:120-128 key:8A energy:0.6-0.8 analyzed:yes artist:… album:…`).
///
/// Lives on the main thread. It asks the server once the user stops typing
/// for `debounce`, and every new query cancels the one in flight. The
/// answer is parsed and stored off the main thread (design §9): its tracks
/// join the catalogue, and the results become the tracks, in the server's
/// order, of the server's one ephemeral container of kind `search`
/// (`crateserver_containers`). The sidebar does not list it, a tree sync does
/// not remove it, and each search replaces it.
class Search : public QObject {
    Q_OBJECT
  public:
    using Options = SearchOptions;

    /// The kind of the ephemeral container of the results.
    static const QString kKind;

    Search(const QString& serverId,
            Client* pClient,
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            Options options = Options(),
            QObject* parent = nullptr);
    ~Search() override;

    const QString& serverId() const {
        return m_serverId;
    }

    /// Searches for `query` once the user stops typing. A query of fewer
    /// than two characters, which the server does not search, only cancels
    /// the one pending or in flight.
    void search(const QString& query);
    /// Drops the pending search and the one in flight.
    void cancel();
    /// The last query given to search().
    const QString& query() const {
        return m_query;
    }
    bool isBusy() const;

  signals:
    /// The results of `query` are the tracks of the container `containerId`.
    void resultsReady(qint64 containerId, const QString& query);
    void searchFailed(const QString& query, const crate::Error& error);

  private:
    void send();
    void onReply(Reply* pReply, quint64 generation, const QString& query);

    const QString m_serverId;
    const QPointer<Client> m_pClient;
    const mixxx::DbConnectionPoolPtr m_pDbConnectionPool;
    const Options m_options;
    /// One worker thread: the results are stored in the order they came.
    QThreadPool m_pool;
    QTimer m_debounce;
    QString m_query;
    /// Bumped by every search() and cancel(), so a late answer is dropped.
    quint64 m_generation = 0;
    QPointer<Reply> m_pReply;
    int m_storing = 0;
};

} // namespace crate::library
