#include "crate/inference/modelstore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QtConcurrentRun>
#include <optional>

#include "util/db/dbconnectionpooled.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::ModelStore");

const QString kModelsDirectoryName = QStringLiteral("models");
const QString kPartSuffix = QStringLiteral(".part");
// Names and versions end up in file names: no separators, no leading dot.
const QRegularExpression kSafeName(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
const QRegularExpression kSafeFileName(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,150}$"));
const QRegularExpression kSafeSuffix(QStringLiteral("^[a-z0-9]{1,8}$"));
const QRegularExpression kSha256(QStringLiteral("^[0-9a-f]{64}$"));
const QRegularExpression kContentRange(QStringLiteral("^bytes ([0-9]+)-([0-9]+)/([0-9]+|\\*)$"));
constexpr int kTransferTimeoutMs = 30000;
constexpr qint64 kHashChunkBytes = 1024 * 1024;

/// Why the entry cannot be downloaded, or empty if it can.
QString validationError(const ModelManifestEntry& entry) {
    if (!kSafeName.match(entry.name).hasMatch()) {
        return QStringLiteral("Invalid model name");
    }
    if (!kSafeName.match(entry.version).hasMatch()) {
        return QStringLiteral("Invalid model version");
    }
    if (!entry.url.isValid() || entry.url.scheme() != QLatin1String("https") ||
            entry.url.host().isEmpty()) {
        return QStringLiteral("The model address is not HTTPS");
    }
    if (!kSha256.match(QString::fromLatin1(entry.sha256).toLower()).hasMatch()) {
        return QStringLiteral("Invalid model SHA-256");
    }
    if (entry.size <= 0) {
        return QStringLiteral("Invalid model size");
    }
    return QString();
}

/// `<name>-<version>` plus the suffix of the address, e.g.
/// "htdemucs-onnx-1.onnx". Only for a valid entry.
QString fileNameFor(const ModelManifestEntry& entry) {
    const QString suffix = QFileInfo(entry.url.path()).suffix().toLower();
    QString fileName = entry.name + QLatin1Char('-') + entry.version;
    if (kSafeSuffix.match(suffix).hasMatch()) {
        fileName += QLatin1Char('.') + suffix;
    }
    return fileName;
}

/// The SHA-256 of the file in lowercase hexadecimal, or empty if it cannot
/// be read. Runs on a worker thread.
QByteArray sha256OfFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kHashChunkBytes);
        if (chunk.isEmpty()) {
            return QByteArray();
        }
        hash.addData(chunk);
    }
    return hash.result().toHex();
}

struct InstalledModel {
    QString version;
    QString fileName;
    QByteArray sha256;
    qint64 bytes = 0;
};

std::optional<InstalledModel> loadInstalledModel(
        const QSqlDatabase& database, const QString& name) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "SELECT version, file_name, sha256, bytes FROM crateserver_models WHERE name = :name"));
    query.bindValue(QStringLiteral(":name"), name);
    if (!query.exec()) {
        kLogger.warning() << "Cannot read the installed models:" << query.lastError().text();
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    InstalledModel model;
    model.version = query.value(0).toString();
    model.fileName = query.value(1).toString();
    model.sha256 = query.value(2).toString().toLatin1();
    model.bytes = query.value(3).toLongLong();
    return model;
}

bool saveInstalledModel(const QSqlDatabase& database,
        const ModelManifestEntry& entry,
        const QString& fileName) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO crateserver_models "
            "(name, version, file_name, sha256, bytes, installed_at) "
            "VALUES (:name, :version, :file_name, :sha256, :bytes, :installed_at)"));
    query.bindValue(QStringLiteral(":name"), entry.name);
    query.bindValue(QStringLiteral(":version"), entry.version);
    query.bindValue(QStringLiteral(":file_name"), fileName);
    query.bindValue(QStringLiteral(":sha256"), QString::fromLatin1(entry.sha256).toLower());
    query.bindValue(QStringLiteral(":bytes"), entry.size);
    query.bindValue(QStringLiteral(":installed_at"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (!query.exec()) {
        kLogger.warning() << "Cannot record the model" << entry.name << ':'
                          << query.lastError().text();
        return false;
    }
    return true;
}

} // namespace

struct ModelStore::Download {
    /// Tells this download from a later one of the same model.
    quint64 id = 0;
    ModelManifestEntry entry;
    QString partPath;
    QFile file;
    QPointer<QNetworkReply> pReply;
    /// Bytes of the partial file before this transfer.
    qint64 offset = 0;
    qint64 received = 0;
    bool statusChecked = false;
    /// The answer is an HTTP error: its body is not the model.
    bool ignoreBody = false;
    /// The partial file is wrong and goes away with the failure.
    bool discardPart = false;
    QString error;
};

QList<ModelManifestEntry> ModelStore::builtInManifest() {
    // TODO(ST4.PR2): the htdemucs ONNX export, once it is a release asset
    // with a pinned URL, SHA-256 and size.
    return {};
}

ModelStore::ModelStore(const QList<ModelManifestEntry>& manifest,
        const Dependencies& dependencies,
        QObject* parent)
        : QObject(parent),
          m_manifest(manifest),
          m_dependencies(dependencies) {
}

ModelStore::~ModelStore() {
    for (const auto& [name, pDownload] : m_downloads) {
        if (pDownload->pReply) {
            pDownload->pReply->disconnect(this);
            pDownload->pReply->abort();
            pDownload->pReply->deleteLater();
        }
    }
}

QString ModelStore::modelsDirectory() const {
    return QDir::cleanPath(QDir(m_dependencies.dataDirectory).filePath(kModelsDirectoryName));
}

QString ModelStore::installedPath(const QString& name) const {
    const ModelManifestEntry* pEntry = findEntry(name);
    if (!pEntry || !validationError(*pEntry).isEmpty()) {
        return QString();
    }
    const std::optional<InstalledModel> installed = loadInstalledModel(database(), name);
    const QString fileName = fileNameFor(*pEntry);
    // The stored file name is never trusted: it must be the one the manifest
    // gives.
    if (!installed || installed->version != pEntry->version ||
            installed->sha256 != pEntry->sha256.toLower() || installed->fileName != fileName ||
            installed->bytes != pEntry->size) {
        return QString();
    }
    const QString path = QDir(modelsDirectory()).filePath(fileName);
    // The hash was checked when it was installed; reading 170 MB again on
    // every start is too slow. The size still catches a truncated file.
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() != pEntry->size) {
        return QString();
    }
    return path;
}

bool ModelStore::isDownloading(const QString& name) const {
    return m_downloads.find(name) != m_downloads.end();
}

void ModelStore::download(const QString& name) {
    if (isDownloading(name)) {
        return;
    }
    const ModelManifestEntry* pEntry = findEntry(name);
    if (!pEntry) {
        finish(name, QString(), QStringLiteral("Unknown model"));
        return;
    }
    const QString invalid = validationError(*pEntry);
    if (!invalid.isEmpty()) {
        finish(name, QString(), invalid);
        return;
    }
    const QString installed = installedPath(name);
    if (!installed.isEmpty()) {
        finish(name, installed, QString());
        return;
    }
    // Files go only to the models directory of the data directory, which
    // must not lead elsewhere through a link.
    const QString dataDirectory = QFileInfo(m_dependencies.dataDirectory).canonicalFilePath();
    if (m_dependencies.dataDirectory.isEmpty() || !QDir().mkpath(modelsDirectory()) ||
            dataDirectory.isEmpty() ||
            QFileInfo(modelsDirectory()).canonicalFilePath() !=
                    QDir(dataDirectory).filePath(kModelsDirectoryName)) {
        kLogger.warning() << "Refusing to store models in" << modelsDirectory();
        finish(name,
                QString(),
                QStringLiteral(
                        "The models directory is not in the data directory"));
        return;
    }

    auto pDownload = std::make_unique<Download>();
    pDownload->id = ++m_lastDownloadId;
    pDownload->entry = *pEntry;
    pDownload->partPath = QDir(modelsDirectory()).filePath(fileNameFor(*pEntry) + kPartSuffix);
    const QFileInfo part(pDownload->partPath);
    if (part.isSymLink() || (part.exists() && !part.isFile())) {
        QFile::remove(pDownload->partPath);
    }
    pDownload->offset = QFileInfo(pDownload->partPath).exists()
            ? QFileInfo(pDownload->partPath).size()
            : 0;
    if (pDownload->offset > pEntry->size) {
        pDownload->offset = 0;
    }
    pDownload->file.setFileName(pDownload->partPath);
    if (!pDownload->file.open(pDownload->offset > 0 ? QIODevice::WriteOnly | QIODevice::Append
                                                    : QIODevice::WriteOnly | QIODevice::Truncate)) {
        finish(name,
                QString(),
                QStringLiteral("Cannot write %1: %2")
                        .arg(pDownload->partPath, pDownload->file.errorString()));
        return;
    }
    Download* const pStarted = pDownload.get();
    m_downloads.emplace(name, std::move(pDownload));
    if (pStarted->offset == pEntry->size) {
        // Downloaded in an earlier run, not checked yet.
        pStarted->file.close();
        verify(pStarted);
        return;
    }

    QNetworkRequest request(pEntry->url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTransferTimeoutMs);
    if (pStarted->offset > 0) {
        request.setRawHeader("Range",
                QByteArrayLiteral("bytes=") + QByteArray::number(pStarted->offset) + '-');
    }
    kLogger.info() << "Downloading the model" << name << pEntry->version << "from"
                   << pEntry->url.toDisplayString() << "at byte" << pStarted->offset;
    pStarted->pReply = m_dependencies.pNetworkAccessManager->get(request);
    connect(pStarted->pReply.data(), &QNetworkReply::readyRead, this, [this, pStarted] {
        onReadyRead(pStarted);
    });
    connect(pStarted->pReply.data(), &QNetworkReply::finished, this, [this, pStarted] {
        onReplyFinished(pStarted);
    });
}

void ModelStore::cancel(const QString& name) {
    const auto it = m_downloads.find(name);
    if (it == m_downloads.end()) {
        return;
    }
    QNetworkReply* const pReply = it->second->pReply;
    if (pReply) {
        pReply->disconnect(this);
        pReply->abort();
        pReply->deleteLater();
    }
    finish(name, QString(), QStringLiteral("Cancelled"));
}

const ModelManifestEntry* ModelStore::findEntry(const QString& name) const {
    for (const ModelManifestEntry& entry : m_manifest) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

void ModelStore::onReadyRead(Download* pDownload) {
    QNetworkReply* const pReply = pDownload->pReply;
    if (!pDownload->statusChecked) {
        pDownload->statusChecked = true;
        const int status = pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 206) {
            const QRegularExpressionMatch range = kContentRange.match(
                    QString::fromLatin1(pReply->rawHeader("Content-Range")));
            const bool totalMatches = range.hasMatch() &&
                    (range.captured(3) == QLatin1String("*") ||
                            range.captured(3).toLongLong() == pDownload->entry.size);
            if (!range.hasMatch() || range.captured(1).toLongLong() != pDownload->offset ||
                    !totalMatches) {
                pDownload->error = QStringLiteral("The server sent another part of the file");
                pDownload->discardPart = true;
                pReply->abort();
                return;
            }
        } else if (status == 200) {
            if (pDownload->offset > 0) {
                // The server ignored the Range: the whole file comes again.
                pDownload->file.resize(0);
                pDownload->offset = 0;
            }
        } else {
            pDownload->ignoreBody = true;
        }
    }
    const QByteArray data = pReply->readAll();
    if (pDownload->ignoreBody) {
        return;
    }
    if (pDownload->offset + pDownload->received + data.size() > pDownload->entry.size) {
        pDownload->error = QStringLiteral("The file is larger than the manifest says");
        pDownload->discardPart = true;
        pReply->abort();
        return;
    }
    if (pDownload->file.write(data) != data.size()) {
        pDownload->error = QStringLiteral("Cannot write %1: %2")
                                   .arg(pDownload->partPath, pDownload->file.errorString());
        pReply->abort();
        return;
    }
    pDownload->received += data.size();
    emit downloadProgress(pDownload->entry.name,
            pDownload->offset + pDownload->received,
            pDownload->entry.size);
}

void ModelStore::onReplyFinished(Download* pDownload) {
    QNetworkReply* const pReply = pDownload->pReply;
    // Nothing more from this reply, even if it reports finished twice.
    pReply->disconnect(this);
    pReply->deleteLater();
    if (pReply->bytesAvailable() > 0 && pDownload->error.isEmpty()) {
        onReadyRead(pDownload);
    }
    pDownload->file.close();

    const int status = pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const qint64 bytes = pDownload->offset + pDownload->received;
    QString error = pDownload->error;
    if (error.isEmpty() && status == 416) {
        // The partial file does not fit the file on the server.
        pDownload->discardPart = true;
        error = QStringLiteral("The server refused to resume the download");
    } else if (error.isEmpty() && pReply->error() != QNetworkReply::NoError) {
        error = QStringLiteral("The download failed: %1").arg(pReply->errorString());
    } else if (error.isEmpty() && status != 200 && status != 206) {
        error = QStringLiteral("The download failed with HTTP %1").arg(status);
    } else if (error.isEmpty() && bytes != pDownload->entry.size) {
        error = QStringLiteral("The download stopped at %1 of %2 bytes")
                        .arg(bytes)
                        .arg(pDownload->entry.size);
    }
    if (!error.isEmpty()) {
        if (pDownload->discardPart) {
            QFile::remove(pDownload->partPath);
        }
        // Otherwise the partial file stays for the next attempt to resume.
        finish(pDownload->entry.name, QString(), error);
        return;
    }
    verify(pDownload);
}

void ModelStore::verify(Download* pDownload) {
    const QString name = pDownload->entry.name;
    const quint64 id = pDownload->id;
    auto* const pWatcher = new QFutureWatcher<QByteArray>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher, name, id] {
        pWatcher->deleteLater();
        const auto it = m_downloads.find(name);
        if (it == m_downloads.end() || it->second->id != id) {
            // Cancelled while hashing.
            return;
        }
        const Download* const pChecked = it->second.get();
        const QByteArray hash = pWatcher->result();
        if (hash.isEmpty()) {
            finish(name, QString(), QStringLiteral("Cannot read %1").arg(pChecked->partPath));
            return;
        }
        if (hash != pChecked->entry.sha256.toLower()) {
            kLogger.warning() << "The model" << name << "has the SHA-256" << hash << "and not"
                              << pChecked->entry.sha256;
            QFile::remove(pChecked->partPath);
            finish(name, QString(), QStringLiteral("The SHA-256 of the file does not match"));
            return;
        }
        install(pChecked->entry, pChecked->partPath);
    });
    pWatcher->setFuture(QtConcurrent::run(&sha256OfFile, pDownload->partPath));
}

void ModelStore::install(const ModelManifestEntry& entry, const QString& partPath) {
    const QString fileName = fileNameFor(entry);
    const QString path = QDir(modelsDirectory()).filePath(fileName);
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        QFile::remove(path);
    }
    if (!QFile::rename(partPath, path)) {
        finish(entry.name, QString(), QStringLiteral("Cannot move the model to %1").arg(path));
        return;
    }
    const std::optional<InstalledModel> previous = loadInstalledModel(database(), entry.name);
    if (!saveInstalledModel(database(), entry, fileName)) {
        finish(entry.name, QString(), QStringLiteral("Cannot record the model"));
        return;
    }
    if (previous && previous->fileName != fileName &&
            kSafeFileName.match(previous->fileName).hasMatch()) {
        // The version this one replaces.
        QFile::remove(QDir(modelsDirectory()).filePath(previous->fileName));
    }
    kLogger.info() << "Installed the model" << entry.name << entry.version << "at" << path;
    finish(entry.name, path, QString());
}

void ModelStore::finish(const QString& name, const QString& path, const QString& error) {
    const auto it = m_downloads.find(name);
    if (it != m_downloads.end()) {
        // The callers may still use the Download (and `name` may be its
        // entry's): it is deleted from the event loop.
        std::shared_ptr<Download> pFinished = std::move(it->second);
        m_downloads.erase(it);
        QTimer::singleShot(0, this, [pFinished]() mutable {
            pFinished.reset();
        });
    }
    if (!error.isEmpty()) {
        kLogger.warning() << "Model" << name << ':' << error;
    }
    QTimer::singleShot(0, this, [this, name, path, error] {
        emit downloadFinished(name, path, error);
    });
}

QSqlDatabase ModelStore::database() const {
    return mixxx::DbConnectionPooled(m_dependencies.pDbConnectionPool);
}

} // namespace crate

#include "moc_modelstore.cpp"
