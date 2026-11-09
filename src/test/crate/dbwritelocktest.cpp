#include <gtest/gtest.h>

#include <QSemaphore>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <memory>

#include "library/queryutil.h"
#include "test/mixxxtest.h"
#include "util/db/dbconnection.h"
#include "util/db/sqltransaction.h"

// Writes of the GUI thread while another connection writes, as the library
// scanner does during a scan (upstream issue 6631): they wait for SQLite's
// write lock instead of failing with "database is locked".

namespace {

mixxx::DbConnection::Params paramsFor(const QString& filePath) {
    mixxx::DbConnection::Params params;
    params.type = QStringLiteral("QSQLITE");
    params.connectOptions = QStringLiteral("QSQLITE_OPEN_URI");
    params.filePath = QStringLiteral("file://") + filePath;
    return params;
}

/// Another connection, in a thread of its own, that holds the write lock
/// for `lockMs` and frees it for `gapMs`, over and over until stopped: the
/// library scanner writing its tracks.
class LockingWriter : public QThread {
  public:
    LockingWriter(const QString& filePath, int lockMs, int gapMs)
            : m_filePath(filePath),
              m_lockMs(lockMs),
              m_gapMs(gapMs) {
    }

    /// Waits until the writer holds the write lock for the first time
    void waitForLock() {
        m_locked.acquire();
    }

    void stop() {
        m_stop = true;
        wait();
    }

  protected:
    void run() override {
        {
            mixxx::DbConnection connection(
                    paramsFor(m_filePath), QStringLiteral("CrateDbWriteLockTest-writer"));
            ASSERT_TRUE(connection.open());
            QSqlQuery query(connection);
            bool first = true;
            while (!m_stop) {
                ASSERT_TRUE(query.exec(QStringLiteral("BEGIN IMMEDIATE")))
                        << query.lastError().text().toStdString();
                ASSERT_TRUE(query.exec(QStringLiteral("INSERT INTO t (v) VALUES (1)")));
                if (first) {
                    m_locked.release();
                    first = false;
                }
                QThread::msleep(m_lockMs);
                ASSERT_TRUE(query.exec(QStringLiteral("COMMIT")));
                QThread::msleep(m_gapMs);
            }
            query.finish();
            connection.close();
        }
    }

  private:
    const QString m_filePath;
    const int m_lockMs;
    const int m_gapMs;
    QSemaphore m_locked;
    std::atomic<bool> m_stop{false};
};

class CrateDbWriteLockTest : public MixxxTest {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_filePath = m_dir.filePath(QStringLiteral("write-lock.sqlite"));
        m_pConnection = std::make_unique<mixxx::DbConnection>(
                paramsFor(m_filePath), QStringLiteral("CrateDbWriteLockTest-gui"));
        ASSERT_TRUE(m_pConnection->open());
        QSqlQuery query(database());
        ASSERT_TRUE(query.exec(QStringLiteral(
                "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)")));
    }

    void TearDown() override {
        m_pConnection->close();
        m_pConnection.reset();
    }

    QSqlDatabase database() const {
        return *m_pConnection;
    }

    /// What a playlist's creation does (PlaylistDAO::createPlaylist()):
    /// reads, then writes, in one transaction
    template<typename Transaction>
    QString readThenWrite() {
        Transaction transaction(database());
        QSqlQuery query(database());
        if (!query.exec(QStringLiteral("SELECT max(id) FROM t"))) {
            return query.lastError().text();
        }
        query.finish();
        if (!query.exec(QStringLiteral("INSERT INTO t (v) VALUES (2)"))) {
            return query.lastError().text();
        }
        if (!transaction.commit()) {
            return database().lastError().text();
        }
        return QString();
    }

    QTemporaryDir m_dir;
    QString m_filePath;
    std::unique_ptr<mixxx::DbConnection> m_pConnection;
};

TEST_F(CrateDbWriteLockTest, ScopedTransactionWaitsForTheWriteLock) {
    LockingWriter writer(m_filePath, 150, 1000);
    writer.start();
    writer.waitForLock();
    EXPECT_QSTRING_EQ(QString(), readThenWrite<ScopedTransaction>());
    writer.stop();
}

TEST_F(CrateDbWriteLockTest, SqlTransactionWaitsForTheWriteLock) {
    LockingWriter writer(m_filePath, 150, 1000);
    writer.start();
    writer.waitForLock();
    EXPECT_QSTRING_EQ(QString(), readThenWrite<SqlTransaction>());
    writer.stop();
}

} // namespace
