#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <optional>

namespace crate::cache {

/// A row of `crateserver_cache` (design §6): one track of one server.
struct CacheEntry {
    enum class State {
        /// Asked for, or pinned, but not downloading: it may have a part file
        /// to resume.
        Pending,
        Downloading,
        /// The file is complete at `path`.
        Ready,
        /// The last download failed.
        Failed,
    };

    QString serverId;
    QString entityUid;
    /// The final file, relative to the cache directory; empty until known.
    QString path;
    /// The size of the file: the whole file once Ready, the expected size
    /// while downloading (0 if unknown).
    qint64 bytes = 0;
    /// ETag of the stream the part file came from, for If-Range.
    QString etag;
    /// The track's `source_revision` when it was downloaded.
    QString sourceRevision;
    State state = State::Pending;
    /// Downloaded ranges, for progressive playback (D14, F4b). Unused here.
    QString ranges;
    /// Why the entry may not be evicted; none if it may.
    QStringList pinnedReasons;
    /// Milliseconds since the epoch; the eviction order (LRU).
    qint64 lastUsedAtMs = 0;

    bool isPinned() const {
        return !pinnedReasons.isEmpty();
    }
};

/// What the reconciliation at start found and fixed.
struct Reconciliation {
    /// The entries that are left, with their state fixed.
    QList<CacheEntry> entries;
    /// Entries removed because their file was gone.
    int removedEntries = 0;
    /// Part files removed because no entry owned them.
    int removedPartFiles = 0;
};

/// The index of the audio cache: its own SQLite database,
/// `<cache>/index.sqlite`, with the table `crateserver_cache`.
///
/// It is synchronous: crate::AudioCache uses it only on its worker thread,
/// and the connection belongs to the thread that calls open().
class AudioCacheIndex {
  public:
    explicit AudioCacheIndex(const QString& directory);
    ~AudioCacheIndex();

    AudioCacheIndex(const AudioCacheIndex&) = delete;
    AudioCacheIndex& operator=(const AudioCacheIndex&) = delete;

    /// Opens the database and creates the table if it does not exist.
    bool open();
    void close();
    bool isOpen() const;

    /// Matches the index with the files in the cache directory:
    /// - a Ready entry whose file is missing goes, and so does an unpinned
    ///   entry with neither its file nor its part file;
    /// - a pinned entry without a file stays, as Pending;
    /// - an entry that was Downloading when the app stopped becomes Pending
    ///   (its part file stays, to resume);
    /// - a part file of the cache layout that no entry owns is deleted.
    /// Only `<server_id>/<xx>/` folders are looked into, and only part files
    /// are deleted: other files in the directory are never touched.
    Reconciliation reconcile();

    QList<CacheEntry> entries() const;
    std::optional<CacheEntry> entry(const QString& serverId, const QString& entityUid) const;
    bool upsert(const CacheEntry& entry);
    bool remove(const QString& serverId, const QString& entityUid);

  private:
    const QString m_directory;
    const QString m_connectionName;
    QSqlDatabase m_database;
};

QString stateName(CacheEntry::State state);

} // namespace crate::cache
