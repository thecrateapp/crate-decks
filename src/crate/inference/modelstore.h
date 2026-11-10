#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QUrl>
#include <map>
#include <memory>

#include "util/db/dbconnectionpool.h"

class QNetworkAccessManager;

namespace crate {

/// A model file that Decks downloads on first use (STEM-D7).
struct ModelManifestEntry {
    /// Identifier, e.g. "htdemucs-onnx". Lowercase letters, digits, '.', '_'
    /// and '-', starting with a letter or digit.
    QString name;
    /// The file's version, with the same characters as the name.
    QString version;
    /// Pinned HTTPS address of the file.
    QUrl url;
    /// SHA-256 of the file: 64 hexadecimal digits.
    QByteArray sha256;
    /// Size of the file in bytes.
    qint64 size = 0;
};

/// Downloads the models in its manifest, checks them and keeps them in the
/// `models` directory of the Decks data directory, and nowhere else.
///
/// - A download resumes a partial file with an HTTP Range request.
/// - A file is installed only if its size and SHA-256 match the manifest.
///   The hash is computed on a worker thread.
/// - The installed models are recorded in `crateserver_models`, which
///   crate::db::createCrateServerSchema() creates at start.
///
/// Lives on the main thread.
class ModelStore : public QObject {
    Q_OBJECT
  public:
    struct Dependencies {
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// Not owned.
        QNetworkAccessManager* pNetworkAccessManager = nullptr;
        /// The Decks data directory (the settings directory).
        QString dataDirectory;
    };

    /// The models this build of Decks knows. Empty until the htdemucs export
    /// of ST4.PR2 is published as a release asset with a pinned hash.
    static QList<ModelManifestEntry> builtInManifest();

    ModelStore(const QList<ModelManifestEntry>& manifest,
            const Dependencies& dependencies,
            QObject* parent = nullptr);
    ~ModelStore() override;

    const QList<ModelManifestEntry>& manifest() const {
        return m_manifest;
    }
    /// `<data directory>/models`.
    QString modelsDirectory() const;

    /// The checked file of the model, or empty if the manifest's version is
    /// not installed.
    QString installedPath(const QString& name) const;
    bool isDownloading(const QString& name) const;

    /// Downloads the model, resuming a partial file, unless it is installed
    /// or downloading already. Emits downloadFinished() once, later, also
    /// when the download cannot start.
    void download(const QString& name);
    /// Stops a download and keeps its partial file, to resume it later.
    void cancel(const QString& name);

  signals:
    void downloadProgress(const QString& name, qint64 bytesReceived, qint64 bytesTotal);
    /// `path` is the installed file on success; `error` is empty then.
    void downloadFinished(const QString& name, const QString& path, const QString& error);

  private:
    struct Download;

    const ModelManifestEntry* findEntry(const QString& name) const;
    void onReadyRead(Download* pDownload);
    void onReplyFinished(Download* pDownload);
    void verify(Download* pDownload);
    void install(const ModelManifestEntry& entry, const QString& partPath);
    /// Ends the download and emits downloadFinished() from the event loop.
    void finish(const QString& name, const QString& path, const QString& error);
    QSqlDatabase database() const;

    const QList<ModelManifestEntry> m_manifest;
    const Dependencies m_dependencies;
    std::map<QString, std::unique_ptr<Download>> m_downloads;
    quint64 m_lastDownloadId = 0;
};

} // namespace crate
