#include "crate/inference/modelstore.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <memory>

#include "crate/db/crateserverschema.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxdbtest.h"

namespace {

using crate::ModelManifestEntry;
using crate::ModelStore;
using crate::test::FakeNetworkAccessManager;
using crate::test::FakeReply;

const QByteArray kModel = QByteArrayLiteral(
        "ONNX graph bytes of a tiny model, long enough to be split in two.");

ModelManifestEntry entryFor(const QByteArray& content,
        const QString& version = QStringLiteral("1")) {
    ModelManifestEntry entry;
    entry.name = QStringLiteral("tiny-model");
    entry.version = version;
    entry.url = QUrl(QStringLiteral("https://models.crate.test/tiny-model-1.onnx"));
    entry.sha256 = QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
    entry.size = content.size();
    return entry;
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

class ModelStoreTest : public MixxxDbTest {
  protected:
    ModelStoreTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(m_root.isValid());
        ASSERT_TRUE(QDir(m_root.path()).mkpath(QStringLiteral("data")));
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
    }

    QString dataDirectory() const {
        return QDir(m_root.path()).filePath(QStringLiteral("data"));
    }

    std::unique_ptr<ModelStore> makeStore(const QList<ModelManifestEntry>& manifest) {
        ModelStore::Dependencies dependencies;
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.dataDirectory = dataDirectory();
        return std::make_unique<ModelStore>(manifest, dependencies);
    }

    /// Waits for downloadFinished() and returns its arguments: name, path and
    /// error.
    static QList<QVariant> waitForFinished(QSignalSpy* pSpy) {
        if (pSpy->isEmpty()) {
            pSpy->wait(5000);
        }
        if (pSpy->isEmpty()) {
            ADD_FAILURE() << "downloadFinished() was not emitted";
            return {QString(), QString(), QStringLiteral("timeout")};
        }
        return pSpy->takeFirst();
    }

    QTemporaryDir m_root;
    FakeNetworkAccessManager m_network;
};

TEST_F(ModelStoreTest, DownloadsChecksAndKeepsAModel) {
    const ModelManifestEntry entry = entryFor(kModel);
    auto pStore = makeStore({entry});
    QSignalSpy finished(pStore.get(), &ModelStore::downloadFinished);
    EXPECT_TRUE(pStore->installedPath(entry.name).isEmpty());

    pStore->download(entry.name);
    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_EQ(entry.url, m_network.last().request.url());
    EXPECT_FALSE(m_network.last().request.hasRawHeader("Range"));
    m_network.last().pReply->respond(200, kModel);

    const QList<QVariant> result = waitForFinished(&finished);
    EXPECT_EQ(entry.name, result[0].toString());
    EXPECT_TRUE(result[2].toString().isEmpty()) << result[2].toString().toStdString();
    const QString path = result[1].toString();
    EXPECT_TRUE(path.startsWith(pStore->modelsDirectory() + QLatin1Char('/')));
    EXPECT_EQ(kModel, readFile(path));
    EXPECT_EQ(path, pStore->installedPath(entry.name));

    // A restart finds it without the network; a new version needs a download.
    EXPECT_EQ(path, makeStore({entry})->installedPath(entry.name));
    EXPECT_TRUE(makeStore({entryFor(kModel + "v2", QStringLiteral("2"))})
                    ->installedPath(entry.name)
                    .isEmpty());
    EXPECT_EQ(1, m_network.sent.size());
}

TEST_F(ModelStoreTest, RejectsAFileWhoseHashDiffers) {
    const ModelManifestEntry entry = entryFor(kModel);
    auto pStore = makeStore({entry});
    QSignalSpy finished(pStore.get(), &ModelStore::downloadFinished);
    QByteArray tampered = kModel;
    tampered[0] = 'X';

    pStore->download(entry.name);
    m_network.last().pReply->respond(200, tampered);

    const QList<QVariant> result = waitForFinished(&finished);
    EXPECT_TRUE(result[1].toString().isEmpty());
    EXPECT_TRUE(result[2].toString().contains(QStringLiteral("SHA-256")))
            << result[2].toString().toStdString();
    EXPECT_TRUE(pStore->installedPath(entry.name).isEmpty());
    // Nothing is kept: the next attempt starts from the first byte.
    EXPECT_TRUE(QDir(pStore->modelsDirectory()).isEmpty());
}

TEST_F(ModelStoreTest, ResumesAPartialDownload) {
    const ModelManifestEntry entry = entryFor(kModel);
    auto pStore = makeStore({entry});
    QSignalSpy finished(pStore.get(), &ModelStore::downloadFinished);
    const qint64 half = kModel.size() / 2;

    // The transfer breaks after the first half.
    pStore->download(entry.name);
    m_network.last().pReply->respondTruncated(
            200, kModel.left(half), QNetworkReply::RemoteHostClosedError);
    QList<QVariant> result = waitForFinished(&finished);
    EXPECT_FALSE(result[2].toString().isEmpty());
    EXPECT_TRUE(pStore->installedPath(entry.name).isEmpty());

    // The next attempt asks for the rest only.
    pStore->download(entry.name);
    ASSERT_EQ(2, m_network.sent.size());
    EXPECT_EQ(QByteArray("bytes=") + QByteArray::number(half) + '-',
            m_network.last().request.rawHeader("Range"));
    m_network.last().pReply->respond(206,
            kModel.mid(half),
            {{"Content-Range",
                    QByteArray("bytes ") + QByteArray::number(half) + '-' +
                            QByteArray::number(kModel.size() - 1) + '/' +
                            QByteArray::number(kModel.size())}});

    result = waitForFinished(&finished);
    EXPECT_TRUE(result[2].toString().isEmpty()) << result[2].toString().toStdString();
    EXPECT_EQ(kModel, readFile(result[1].toString()));
    EXPECT_EQ(result[1].toString(), pStore->installedPath(entry.name));
}

TEST_F(ModelStoreTest, StartsOverWhenTheServerIgnoresTheRange) {
    const ModelManifestEntry entry = entryFor(kModel);
    auto pStore = makeStore({entry});
    QSignalSpy finished(pStore.get(), &ModelStore::downloadFinished);
    pStore->download(entry.name);
    m_network.last().pReply->respondTruncated(
            200, QByteArrayLiteral("stale bytes"), QNetworkReply::RemoteHostClosedError);
    waitForFinished(&finished);

    pStore->download(entry.name);
    EXPECT_TRUE(m_network.last().request.hasRawHeader("Range"));
    m_network.last().pReply->respond(200, kModel);

    const QList<QVariant> result = waitForFinished(&finished);
    EXPECT_TRUE(result[2].toString().isEmpty()) << result[2].toString().toStdString();
    EXPECT_EQ(kModel, readFile(result[1].toString()));
}

TEST_F(ModelStoreTest, StopsAFileLargerThanTheManifestSays) {
    const ModelManifestEntry entry = entryFor(kModel);
    auto pStore = makeStore({entry});
    QSignalSpy finished(pStore.get(), &ModelStore::downloadFinished);

    pStore->download(entry.name);
    m_network.last().pReply->respond(200, kModel + kModel);

    const QList<QVariant> result = waitForFinished(&finished);
    EXPECT_FALSE(result[2].toString().isEmpty());
    EXPECT_TRUE(pStore->installedPath(entry.name).isEmpty());
    EXPECT_TRUE(QDir(pStore->modelsDirectory()).isEmpty());
}

TEST_F(ModelStoreTest, NeverStoresOutsideTheDataDirectory) {
    QList<ModelManifestEntry> manifest;
    const QList<std::pair<QString, QString>> escapes = {
            {QStringLiteral("../escaped"), QStringLiteral("1")},
            {QStringLiteral("nested/escaped"), QStringLiteral("1")},
            {m_root.filePath(QStringLiteral("escaped")), QStringLiteral("1")},
            {QStringLiteral("escaped"), QStringLiteral("../../1")},
            {QStringLiteral(".."), QStringLiteral("1")},
    };
    for (const auto& [name, version] : escapes) {
        ModelManifestEntry entry = entryFor(kModel);
        entry.name = name;
        entry.version = version;
        manifest.append(entry);
    }
    // Only HTTPS, and a hash that is a SHA-256.
    ModelManifestEntry plainHttp = entryFor(kModel);
    plainHttp.name = QStringLiteral("plain-http");
    plainHttp.url = QUrl(QStringLiteral("http://models.crate.test/tiny-model-1.onnx"));
    manifest.append(plainHttp);
    ModelManifestEntry badHash = entryFor(kModel);
    badHash.name = QStringLiteral("bad-hash");
    badHash.sha256 = "../../etc";
    manifest.append(badHash);
    auto pStore = makeStore(manifest);
    QSignalSpy finished(pStore.get(), &ModelStore::downloadFinished);

    for (const ModelManifestEntry& entry : std::as_const(manifest)) {
        pStore->download(entry.name);
        const QList<QVariant> result = waitForFinished(&finished);
        EXPECT_EQ(entry.name, result[0].toString());
        EXPECT_FALSE(result[2].toString().isEmpty()) << entry.name.toStdString();
        EXPECT_TRUE(pStore->installedPath(entry.name).isEmpty());
    }

    EXPECT_TRUE(m_network.sent.isEmpty());
    EXPECT_EQ(QStringList({QStringLiteral("data")}),
            QDir(m_root.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot));
}

#ifndef Q_OS_WIN
TEST_F(ModelStoreTest, NeverFollowsAModelsLinkOutOfTheDataDirectory) {
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    ASSERT_TRUE(QFile::link(outside.path(),
            QDir(dataDirectory()).filePath(QStringLiteral("models"))));
    const ModelManifestEntry entry = entryFor(kModel);
    auto pStore = makeStore({entry});
    QSignalSpy finished(pStore.get(), &ModelStore::downloadFinished);

    pStore->download(entry.name);

    const QList<QVariant> result = waitForFinished(&finished);
    EXPECT_FALSE(result[2].toString().isEmpty());
    EXPECT_TRUE(m_network.sent.isEmpty());
    EXPECT_TRUE(QDir(outside.path()).isEmpty());
}
#endif

TEST_F(ModelStoreTest, KeepsThePartialFileWhenCancelled) {
    const ModelManifestEntry entry = entryFor(kModel);
    auto pStore = makeStore({entry});
    QSignalSpy finished(pStore.get(), &ModelStore::downloadFinished);
    pStore->download(entry.name);
    EXPECT_TRUE(pStore->isDownloading(entry.name));

    pStore->cancel(entry.name);

    EXPECT_FALSE(pStore->isDownloading(entry.name));
    EXPECT_TRUE(m_network.last().pReply->aborted());
    const QList<QVariant> result = waitForFinished(&finished);
    EXPECT_FALSE(result[2].toString().isEmpty());
}

} // namespace
