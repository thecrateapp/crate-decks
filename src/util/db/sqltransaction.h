#pragma once

#include <QSqlDatabase>

/// Begins a transaction that takes SQLite's write lock at once (BEGIN
/// IMMEDIATE), so that it waits for the busy handler while another
/// connection writes. A deferred transaction that reads before it writes
/// gets "database is locked" at once instead when it tries to write, as
/// SQLite avoids a deadlock (upstream issue 6631). Other drivers begin an
/// ordinary transaction. Every transaction of SqlTransaction and
/// ScopedTransaction begins here: they all write.
bool beginImmediateTransaction(const QSqlDatabase& database);

class SqlTransaction final {
  public:
    explicit SqlTransaction(
            const QSqlDatabase& database);
    SqlTransaction(SqlTransaction&& other);
    ~SqlTransaction();

    operator bool() const {
        return m_active;
    }

    bool commit();
    bool rollback();

    void release();

    // Disable copy construction and copy/move assignment
    SqlTransaction(const SqlTransaction&) = delete;
    SqlTransaction& operator=(const SqlTransaction&) = delete;
    SqlTransaction& operator=(SqlTransaction&&) = delete;

  private:
    QSqlDatabase m_database;
    bool m_active;
};
