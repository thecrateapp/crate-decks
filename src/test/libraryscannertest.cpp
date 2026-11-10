#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QEventLoop>
#include <QFile>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>

#include "library/scanner/libraryscanner.h"
#include "test/librarytest.h"
#include "track/track.h"

class LibraryScannerTest : public LibraryTest {
  protected:
    LibraryScannerTest()
            : m_libraryScanner(dbConnectionPooler(), config()) {
    }
    LibraryScanner m_libraryScanner;
};

namespace {

const QString kProbeConnectionName = QStringLiteral("LibraryScannerTest-probe");

// Writes to the database through a connection of its own that does not wait
// for locks: the write fails while another connection holds a write
// transaction open. Returns the error, or an empty string on success.
QString writeFromAnotherConnection(const QString& databaseName) {
    QString error;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
                QStringLiteral("QSQLITE"), kProbeConnectionName);
        database.setConnectOptions(
                QStringLiteral("QSQLITE_OPEN_URI;QSQLITE_BUSY_TIMEOUT=0"));
        database.setDatabaseName(databaseName);
        if (database.open()) {
            QSqlQuery query(database);
            if (!query.exec(QStringLiteral(
                        "INSERT OR REPLACE INTO settings (name, value) "
                        "VALUES ('LibraryScannerTest.probe', 'written')"))) {
                error = query.lastError().text();
            }
        } else {
            error = database.lastError().text();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(kProbeConnectionName);
    return error;
}

} // namespace

TEST_F(LibraryScannerTest, ScannerRoundtrip) {
    // Normal flow:
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::IDLE);
    m_libraryScanner.changeScannerState(LibraryScanner::STARTING);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::STARTING);
    m_libraryScanner.changeScannerState(LibraryScanner::SCANNING);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::SCANNING);
    m_libraryScanner.changeScannerState(LibraryScanner::FINISHED);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::IDLE);

    // No Tracks:
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::IDLE);
    m_libraryScanner.changeScannerState(LibraryScanner::STARTING);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::STARTING);
    m_libraryScanner.changeScannerState(LibraryScanner::IDLE);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::IDLE);

    // Cancel during scanning:
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::IDLE);
    m_libraryScanner.changeScannerState(LibraryScanner::STARTING);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::STARTING);
    m_libraryScanner.changeScannerState(LibraryScanner::SCANNING);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::SCANNING);
    m_libraryScanner.changeScannerState(LibraryScanner::CANCELING);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::CANCELING);
    m_libraryScanner.changeScannerState(LibraryScanner::FINISHED);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::IDLE);
    m_libraryScanner.changeScannerState(LibraryScanner::IDLE);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::IDLE);

    // restart during canceling :
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::IDLE);
    m_libraryScanner.changeScannerState(LibraryScanner::CANCELING);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::CANCELING);
    m_libraryScanner.changeScannerState(LibraryScanner::STARTING);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::CANCELING);
    m_libraryScanner.changeScannerState(LibraryScanner::IDLE);
    EXPECT_EQ(m_libraryScanner.m_state, LibraryScanner::IDLE);
}

// https://github.com/mixxxdj/mixxx/issues/6631
// The scanner must not keep the database locked for the whole scan: other
// connections (e.g. the GUI thread's) need to write while it runs.
TEST_F(LibraryScannerTest, OtherConnectionsCanWriteDuringScan) {
    const QTemporaryDir libraryDir;
    ASSERT_TRUE(libraryDir.isValid());
    const QStringList suffixes{
            QStringLiteral("-jpg.mp3"),
            QStringLiteral("-png.mp3"),
            QStringLiteral("-vbr.mp3"),
            QStringLiteral(".flac")};
    for (const auto& suffix : suffixes) {
        ASSERT_TRUE(QFile::copy(getTestFile(suffix),
                libraryDir.filePath(QStringLiteral("track") + suffix)));
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok,
            internalCollection()->getDirectoryDAO().addDirectory(
                    mixxx::FileInfo(libraryDir.path())));

    const QString databaseName = dbConnection().databaseName();
    // Only modified in the scanner thread and read after the scan
    int numWrites = 0;
    QStringList writeErrors;
    // Dropping the last reference evicts a track. Keep them until the scan
    // has finished, so that they are evicted in this thread.
    QList<TrackPointer> addedTracks;
    // Direct connections: the probes run in the scanner thread, right after
    // it has added a track or saved a directory hash.
    const auto probe = [&]() {
        ++numWrites;
        const QString error = writeFromAnotherConnection(databaseName);
        if (!error.isEmpty()) {
            writeErrors.append(error);
        }
    };
    QObject::connect(
            &m_libraryScanner,
            &LibraryScanner::trackAdded,
            &m_libraryScanner,
            [pAddedTracks = &addedTracks, pProbe = &probe](TrackPointer pTrack) {
                pAddedTracks->append(pTrack);
                (*pProbe)();
            },
            Qt::DirectConnection);
    QObject::connect(&m_libraryScanner,
            &LibraryScanner::progressHashing,
            &m_libraryScanner,
            probe,
            Qt::DirectConnection);
    // Stop the scanner thread before the variables that the probes capture
    // go out of scope, also if the scan does not finish in time.
    const auto stopScanner = qScopeGuard([this]() {
        m_libraryScanner.slotCancel();
        m_libraryScanner.quit();
        m_libraryScanner.wait();
    });

    QEventLoop loop;
    bool scanFinished = false;
    QObject::connect(
            &m_libraryScanner,
            &LibraryScanner::scanFinished,
            &loop,
            [&]() {
                scanFinished = true;
                loop.quit();
            },
            Qt::QueuedConnection);
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    m_libraryScanner.start();
    m_libraryScanner.scan();
    loop.exec();
    ASSERT_TRUE(scanFinished);

    EXPECT_EQ(suffixes.size(), addedTracks.size());
    // One probe per added track and one for the directory hash
    EXPECT_EQ(suffixes.size() + 1, numWrites);
    EXPECT_TRUE(writeErrors.isEmpty())
            << writeErrors.size() << " blocked writes: "
            << writeErrors.join(QStringLiteral("; ")).toStdString();
    EXPECT_EQ(suffixes.size(),
            internalCollection()->getTrackDAO().getAllTrackLocations().size());
}

// The scanner commits its new tracks in short batches instead of one by one,
// which costs a commit each, or all at once, which keeps the database locked
TEST_F(LibraryScannerTest, ScanCommitsInShortBatches) {
    const QTemporaryDir libraryDir;
    ASSERT_TRUE(libraryDir.isValid());
    const QStringList suffixes{
            QStringLiteral("-jpg.mp3"),
            QStringLiteral("-png.mp3"),
            QStringLiteral("-vbr.mp3"),
            QStringLiteral(".flac")};
    int numFiles = 0;
    for (int copy = 0; copy < 10; ++copy) {
        for (const auto& suffix : suffixes) {
            ASSERT_TRUE(QFile::copy(getTestFile(suffix),
                    libraryDir.filePath(QStringLiteral("track%1").arg(copy) + suffix)));
            ++numFiles;
        }
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok,
            internalCollection()->getDirectoryDAO().addDirectory(
                    mixxx::FileInfo(libraryDir.path())));

    // Only modified in the scanner thread and read after the scan
    QList<int> batches;
    QObject::connect(
            &m_libraryScanner.m_trackDao,
            &TrackDAO::tracksAdded,
            &m_libraryScanner,
            [pBatches = &batches](const QSet<TrackId>& trackIds) {
                if (!trackIds.isEmpty()) {
                    pBatches->append(static_cast<int>(trackIds.size()));
                }
            },
            Qt::DirectConnection);
    // Keep the tracks until the scan has finished, so that they are evicted
    // in this thread
    QList<TrackPointer> addedTracks;
    QObject::connect(
            &m_libraryScanner,
            &LibraryScanner::trackAdded,
            &m_libraryScanner,
            [pAddedTracks = &addedTracks](TrackPointer pTrack) {
                pAddedTracks->append(pTrack);
            },
            Qt::DirectConnection);
    const auto stopScanner = qScopeGuard([this]() {
        m_libraryScanner.slotCancel();
        m_libraryScanner.quit();
        m_libraryScanner.wait();
    });

    QEventLoop loop;
    bool scanFinished = false;
    QObject::connect(
            &m_libraryScanner,
            &LibraryScanner::scanFinished,
            &loop,
            [&]() {
                scanFinished = true;
                loop.quit();
            },
            Qt::QueuedConnection);
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    m_libraryScanner.start();
    m_libraryScanner.scan();
    loop.exec();
    ASSERT_TRUE(scanFinished);

    EXPECT_EQ(numFiles, addedTracks.size());
    int committed = 0;
    for (const int batch : std::as_const(batches)) {
        committed += batch;
        EXPECT_LE(batch, LibraryScanner::kScanBatchMaxTracks);
    }
    EXPECT_EQ(numFiles, committed);
    EXPECT_LT(batches.size(), numFiles);
}
