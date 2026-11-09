#pragma once

#include <gtest/gtest_prod.h>

#include <QElapsedTimer>
#include <QList>
#include <QScopedPointer>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>

#include "library/dao/analysisdao.h"
#include "library/dao/cuedao.h"
#include "library/dao/directorydao.h"
#include "library/dao/libraryhashdao.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackdao.h"
#include "library/scanner/scannerglobal.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class ScannerTask;
class LibraryScannerDlg;
class QString;
struct LibraryScanResultSummary;

class LibraryScanner : public QThread {
    FRIEND_TEST(LibraryScannerTest, ScannerRoundtrip);
    FRIEND_TEST(LibraryScannerTest, ScanCommitsInShortBatches);
    Q_OBJECT
  public:
    // A scan commits its new tracks in batches of at most this many tracks
    // and this long, so that other connections, e.g. the GUI thread's, get
    // SQLite's write lock between them (upstream issue 6631)
    static constexpr int kScanBatchMaxTracks = 64;
    static constexpr int kScanBatchMaxMs = 40;

    LibraryScanner(
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            const UserSettingsPointer& pConfig);
    ~LibraryScanner() override;

  public slots:
    // Call from any thread to start a scan. Does nothing if a scan is already
    // in progress.
    // The autoscan flag is used for the summary report. Receivers of scanSummary()
    // can use this to decide whether to show the summary dialog, for example hide
    // it for the automatic scan during startup.
    void scan(bool autoscan = false);

    // Call from any thread to cancel the scan.
    void slotCancel();

  signals:
    void scanStarted();
    void scanFinished();
    void scanSummary(const LibraryScanResultSummary& result);
    void progressHashing(const QString&);
    void progressLoading(const QString& path);
    void progressCoverArt(const QString& file);
    void trackAdded(TrackPointer pTrack);
    void tracksChanged(const QSet<TrackId>& changedTrackIds);
    void tracksRelocated(const QList<RelocatedTrack>& relocatedTracks);

    // Emitted by scan() to invoke slotStartScan in the scanner thread's event
    // loop.
    void startScan();

  protected:
    void run() override;

  public slots:
    void queueTask(ScannerTask* pTask);

  private slots:
    void slotStartScan();
    void slotFinishHashedScan();
    void slotFinishUnhashedScan();

    // ScannerTask signal handlers.
    void slotDirectoryHashedAndScanned(const QString& directoryPath,
                                   bool newDirectory, mixxx::cache_key_t hash);
    void slotDirectoryUnchanged(const QString& directoryPath,
            const QStringList& presentTrackLocations);
    void slotTrackExists(const QString& trackPath);
    void slotAddNewTrack(const QString& trackPath);

  private:
    enum ScannerState {
        IDLE,
        STARTING,
        SCANNING,
        CANCELING,
        FINISHED
    };

    void cancelAndQuit();
    void cancel();

    // Allowed State transitions:
    // IDLE -> STARTING
    // STARTING -> IDLE
    // STARTING -> SCANNING
    // SCANNING -> FINISHED
    // FINISHED -> IDLE
    // every state can change to CANCELING
    // CANCELING -> IDLE
    bool changeScannerState(LibraryScanner::ScannerState newState);

    void cleanUpScan();

    /// Begins a batch of new tracks: a transaction that the scan commits
    /// after kScanBatchMaxTracks tracks or kScanBatchMaxMs, whichever comes
    /// first, as soon as another connection waits for the write lock, or
    /// when it stops adding tracks. Lets a waiting connection go first.
    void beginScanBatch();
    /// Commits the open batch, if any, and announces its tracks
    void commitScanBatch();

    mixxx::DbConnectionPoolPtr m_pDbConnectionPool;

    // The pool of threads used for worker tasks.
    QThreadPool m_pool;

    // The library scanner thread's DAOs.
    LibraryHashDAO m_libraryHashDao;
    CueDAO m_cueDao;
    PlaylistDAO m_playlistDao;
    DirectoryDAO m_directoryDao;
    AnalysisDao m_analysisDao;
    TrackDAO m_trackDao;

    // The open batch of new tracks (beginScanBatch()), announced once
    // committed, when other connections can read them
    bool m_scanBatchOpen = false;
    quint64 m_scanBatchId = 0;
    QElapsedTimer m_scanBatchTimer;
    QList<TrackPointer> m_scanBatchTracks;

    // Global scanner state for scan currently in progress.
    ScannerGlobalPointer m_scannerGlobal;

    // The Semaphore guards the state transitions queued to the
    // Qt even Queue in the way, that you cannot start a
    // new scan while the old one is canceled
    QSemaphore m_stateSema;
    // this is accessed main and LibraryScanner thread
    volatile ScannerState m_state;

    QSet<QString> m_previouslyMissingTracks;
    int m_numRelocatedTracks;

    QList<mixxx::FileInfo> m_libraryRootDirs;
    std::unique_ptr<LibraryScannerDlg> m_pProgressDlg;

    bool m_canceled;
    bool m_manualScan;
};
