#include "crate/cache/audiocacheindex.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include "crate/cache/cachepaths.h"
#include "util/logger.h"

namespace crate::cache {

namespace {

const mixxx::Logger kLogger("crate::cache::AudioCacheIndex");

const QString kFileName = QStringLiteral("index.sqlite");
/// Pin reasons are short identifiers ("deck", "offline-set:12"), never with
/// a newline.
const QChar kReasonSeparator = QLatin1Char('\n');

const char* const kCreateTable =
        "CREATE TABLE IF NOT EXISTS crateserver_cache ("
        " server_id TEXT NOT NULL,"
        " entity_uid TEXT NOT NULL,"
        " path TEXT NOT NULL DEFAULT '',"
        " bytes INTEGER NOT NULL DEFAULT 0,"
        " etag TEXT NOT NULL DEFAULT '',"
        " source_revision TEXT NOT NULL DEFAULT '',"
        " state TEXT NOT NULL DEFAULT 'pending',"
        " ranges TEXT NOT NULL DEFAULT '',"
        " pinned_reason TEXT NOT NULL DEFAULT '',"
        " last_used_at INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY (server_id, entity_uid))";

CacheEntry::State stateOf(const QString& name) {
    if (name == QLatin1String("downloading")) {
        return CacheEntry::State::Downloading;
    }
    if (name == QLatin1String("ready")) {
        return CacheEntry::State::Ready;
    }
    if (name == QLatin1String("failed")) {
        return CacheEntry::State::Failed;
    }
    return CacheEntry::State::Pending;
}

CacheEntry entryOf(const QSqlQuery& query) {
    CacheEntry entry;
    entry.serverId = query.value(0).toString();
    entry.entityUid = query.value(1).toString();
    entry.path = query.value(2).toString();
    entry.bytes = query.value(3).toLongLong();
    entry.etag = query.value(4).toString();
    entry.sourceRevision = query.value(5).toString();
    entry.state = stateOf(query.value(6).toString());
    entry.ranges = query.value(7).toString();
    const QString reasons = query.value(8).toString();
    if (!reasons.isEmpty()) {
        entry.pinnedReasons = reasons.split(kReasonSeparator, Qt::SkipEmptyParts);
    }
    entry.lastUsedAtMs = query.value(9).toLongLong();
    return entry;
}

/// A null QString binds as NULL: the text columns are NOT NULL.
QString text(const QString& value) {
    return value.isNull() ? QStringLiteral("") : value;
}

const char* const kSelect =
        "SELECT server_id, entity_uid, path, bytes, etag, source_revision, state,"
        " ranges, pinned_reason, last_used_at FROM crateserver_cache";

} // namespace

QString stateName(CacheEntry::State state) {
    switch (state) {
    case CacheEntry::State::Pending:
        return QStringLiteral("pending");
    case CacheEntry::State::Downloading:
        return QStringLiteral("downloading");
    case CacheEntry::State::Ready:
        return QStringLiteral("ready");
    case CacheEntry::State::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("pending");
}

AudioCacheIndex::AudioCacheIndex(const QString& directory)
        : m_directory(directory),
          m_connectionName(QStringLiteral("crate-audio-cache-") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces)) {
}

AudioCacheIndex::~AudioCacheIndex() {
    close();
}

bool AudioCacheIndex::open() {
    if (isOpen()) {
        return true;
    }
    if (!QDir().mkpath(m_directory)) {
        kLogger.warning() << "Cannot create the cache directory" << m_directory;
        return false;
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(QDir(m_directory).filePath(kFileName));
    if (!m_database.open()) {
        kLogger.warning() << "Cannot open the cache index:" << m_database.lastError().text();
        close();
        return false;
    }
    QSqlQuery query(m_database);
    if (!query.exec(QString::fromLatin1(kCreateTable))) {
        kLogger.warning() << "Cannot create crateserver_cache:" << query.lastError().text();
        close();
        return false;
    }
    return true;
}

void AudioCacheIndex::close() {
    if (!m_database.isValid()) {
        return;
    }
    m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool AudioCacheIndex::isOpen() const {
    return m_database.isValid() && m_database.isOpen();
}

QList<CacheEntry> AudioCacheIndex::entries() const {
    QList<CacheEntry> entries;
    if (!isOpen()) {
        return entries;
    }
    QSqlQuery query(m_database);
    if (!query.exec(QString::fromLatin1(kSelect))) {
        kLogger.warning() << "Cannot read crateserver_cache:" << query.lastError().text();
        return entries;
    }
    while (query.next()) {
        entries.append(entryOf(query));
    }
    return entries;
}

std::optional<CacheEntry> AudioCacheIndex::entry(
        const QString& serverId, const QString& entityUid) const {
    if (!isOpen()) {
        return std::nullopt;
    }
    QSqlQuery query(m_database);
    query.prepare(QString::fromLatin1(kSelect) +
            QStringLiteral(" WHERE server_id = :server AND entity_uid = :uid"));
    query.bindValue(QStringLiteral(":server"), serverId);
    query.bindValue(QStringLiteral(":uid"), entityUid);
    if (!query.exec() || !query.next()) {
        return std::nullopt;
    }
    return entryOf(query);
}

bool AudioCacheIndex::upsert(const CacheEntry& entry) {
    if (!isOpen()) {
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "INSERT INTO crateserver_cache (server_id, entity_uid, path, bytes, etag,"
            " source_revision, state, ranges, pinned_reason, last_used_at)"
            " VALUES (:server, :uid, :path, :bytes, :etag, :source, :state, :ranges,"
            " :pinned, :used)"
            " ON CONFLICT (server_id, entity_uid) DO UPDATE SET path = excluded.path,"
            " bytes = excluded.bytes, etag = excluded.etag,"
            " source_revision = excluded.source_revision, state = excluded.state,"
            " ranges = excluded.ranges, pinned_reason = excluded.pinned_reason,"
            " last_used_at = excluded.last_used_at"));
    query.bindValue(QStringLiteral(":server"), entry.serverId);
    query.bindValue(QStringLiteral(":uid"), entry.entityUid);
    query.bindValue(QStringLiteral(":path"), text(entry.path));
    query.bindValue(QStringLiteral(":bytes"), entry.bytes);
    query.bindValue(QStringLiteral(":etag"), text(entry.etag));
    query.bindValue(QStringLiteral(":source"), text(entry.sourceRevision));
    query.bindValue(QStringLiteral(":state"), stateName(entry.state));
    query.bindValue(QStringLiteral(":ranges"), text(entry.ranges));
    query.bindValue(QStringLiteral(":pinned"), text(entry.pinnedReasons.join(kReasonSeparator)));
    query.bindValue(QStringLiteral(":used"), entry.lastUsedAtMs);
    if (!query.exec()) {
        kLogger.warning() << "Cannot write crateserver_cache:" << query.lastError().text();
        return false;
    }
    return true;
}

bool AudioCacheIndex::remove(const QString& serverId, const QString& entityUid) {
    if (!isOpen()) {
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "DELETE FROM crateserver_cache WHERE server_id = :server AND entity_uid = :uid"));
    query.bindValue(QStringLiteral(":server"), serverId);
    query.bindValue(QStringLiteral(":uid"), entityUid);
    if (!query.exec()) {
        kLogger.warning() << "Cannot delete from crateserver_cache:" << query.lastError().text();
        return false;
    }
    return true;
}

Reconciliation AudioCacheIndex::reconcile() {
    Reconciliation reconciliation;
    const QDir directory(m_directory);
    QSet<QString> ownedPartFiles;
    for (CacheEntry entry : entries()) {
        const bool hasPath = !entry.path.isEmpty();
        const QString path = hasPath ? directory.filePath(entry.path) : QString();
        const bool hasFile = hasPath && QFileInfo(path).isFile();
        const bool hasPart = hasPath && QFileInfo(partPathOf(path)).isFile();
        bool keep = true;
        bool changed = false;
        if (entry.state == CacheEntry::State::Ready) {
            if (hasFile) {
                const qint64 size = QFileInfo(path).size();
                if (size != entry.bytes) {
                    entry.bytes = size;
                    changed = true;
                }
            } else if (entry.isPinned()) {
                entry.state = CacheEntry::State::Pending;
                entry.bytes = 0;
                changed = true;
            } else {
                keep = false;
            }
        } else if (!hasPart && !entry.isPinned()) {
            keep = false;
        } else if (entry.state == CacheEntry::State::Downloading) {
            entry.state = CacheEntry::State::Pending;
            changed = true;
        }
        if (!keep) {
            remove(entry.serverId, entry.entityUid);
            ++reconciliation.removedEntries;
            continue;
        }
        if (changed) {
            upsert(entry);
        }
        if (hasPart && entry.state != CacheEntry::State::Ready) {
            ownedPartFiles.insert(QFileInfo(partPathOf(path)).absoluteFilePath());
        }
        reconciliation.entries.append(entry);
    }

    // Only <server_id>/<xx>/ folders of the cache layout are looked into.
    const QFileInfoList servers = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& server : servers) {
        if (!isSafePathId(server.fileName())) {
            continue;
        }
        const QFileInfoList buckets =
                QDir(server.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& bucket : buckets) {
            if (bucket.fileName().size() != 2 || !isSafePathId(bucket.fileName())) {
                continue;
            }
            const QFileInfoList files =
                    QDir(bucket.absoluteFilePath()).entryInfoList(QDir::Files | QDir::Hidden);
            for (const QFileInfo& file : files) {
                if (!isPartFile(file.fileName()) ||
                        ownedPartFiles.contains(file.absoluteFilePath())) {
                    continue;
                }
                if (QFile::remove(file.absoluteFilePath())) {
                    ++reconciliation.removedPartFiles;
                }
            }
        }
    }
    if (reconciliation.removedEntries > 0 || reconciliation.removedPartFiles > 0) {
        kLogger.info() << "Reconciled the cache index:" << reconciliation.removedEntries
                       << "entries without a file and" << reconciliation.removedPartFiles
                       << "orphan part files removed";
    }
    return reconciliation;
}

} // namespace crate::cache
