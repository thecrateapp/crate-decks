// G3 preparation (design §9 budgets): the budgets measured live against a
// Crate dev stack, with Decks' own components as the application builds
// them: CrateServers signed in through AddServerFlow, the library sync,
// CrateServerFeature and its models, the audio cache, ProfileStore,
// AnalysisImporter, CrateLoadCoordinator, and the real Library,
// PlayerManager and engine (as in crateloadintodecks_test.cpp), offscreen,
// on a fresh settings directory and audio cache for each test.
//
// Skipped unless CRATE_DECKS_LIVE_URL is set, so CI never runs it:
//   CRATE_DECKS_LIVE_URL            the server, e.g. a measuring proxy in
//                                   front of the monorepo's `make dev` stack
//   CRATE_DECKS_LIVE_EMAIL,
//   CRATE_DECKS_LIVE_PASSWORD       an account of that dev stack; never
//                                   production
//   CRATE_DECKS_LIVE_OUT            where the results go: a CSV per test and
//                                   budgets.json, which each test adds its
//                                   part to. The audio cache of each test is
//                                   a temporary directory in there
//   CRATE_DECKS_LIVE_PROXY_SUMMARY  optional: the measuring proxy's summary,
//                                   {"peak": {"api": n, "stream": n, ...}}
//   CRATE_DECKS_LIVE_PROXY_REQUESTS optional: its CSV of requests
//                                   (start_s, end_s, class, ...,
//                                   inflight_at_start)
//
// It reports the budgets, as [ MEASURE  ] lines and in those files, and does
// not assert them: it only asserts what it needs to measure (signed in, and
// the playlists with their rows). The playlists are the G3 ones of the dev
// stack's admin (kPlaylist500 and the rest below). Run it with
// --gtest_filter='CrateG3BudgetsLive.*', one test at a time or all in one
// process.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include "analyzer/analyzerprogress.h"
#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
#include "crate/analysis/analysisimporter.h"
#include "crate/analysis/profilestore.h"
#include "crate/cache/audiocache.h"
#include "crate/crateaddserverflow.h"
#include "crate/crateservers.h"
#include "crate/library/cratelibrarystore.h"
#include "crate/library/crateloadcoordinator.h"
#include "crate/library/cratesearch.h"
#include "crate/session/cratesecretstore.h"
#include "crate/session/cratesession.h"
#include "database/mixxxdb.h"
#include "effects/effectsmanager.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"
#include "library/coverartcache.h"
#include "library/crateserver/crateserverfeature.h"
#include "library/crateserver/crateservertrackmodel.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "mixer/deck.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "test/mixxxdbtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"
#include "track/trackref.h"
#include "waveform/waveform.h"
#include "widget/wtracktableview.h"
#ifdef __RUBBERBAND__
#include "engine/bufferscalers/rubberbandworkerpool.h"
#endif

namespace {

using crate::CrateServers;
using crate::library::CrateServerFeature;
using crate::library::CrateServerTrackModel;
using crate::library::CrateTrackRef;

// The G3 playlists of the dev stack's admin
const QString kPlaylist500 = QStringLiteral("G3 · 500 tracks (open time)");
const QString kOfflineSet = QStringLiteral("G3 · Offline set (50)");
const QString kElectronic = QStringLiteral("G3 · Electronic (Q3 Q5 grids)");
const QString kAutoDj = QStringLiteral("G3 · Auto DJ (industrial EBM)");
const QString kFlacs = QStringLiteral("G3 · Q10 FLAC 30-60 MB");
const QString kMp3s = QStringLiteral("G3 · Q10 MP3");

const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");

constexpr int kNetworkWaitMs = 60000;
constexpr int kLoadWaitMs = 60000;
constexpr int kAnalysisWaitMs = 120000;
constexpr int kAgainWaitMs = 15000;
constexpr int kRuns = 5;
constexpr qint64 kMiB = qint64{1} << 20;
constexpr qint64 kGiB = qint64{1} << 30;

// Design §9
constexpr double kWarmOpenBudgetMs = 150.0;
constexpr double kColdOpenBudgetMs = 1000.0;
constexpr double kUncachedLoadBudgetMs = 1500.0;
constexpr int kApiBudget = 4;
constexpr int kDownloadBudget = 2;
constexpr double kStallBudgetMs = 16.0;

void deleteTrack(Track* pTrack) {
    delete pTrack;
}

double msBetween(qint64 fromNs, qint64 toNs) {
    return static_cast<double>(toNs - fromNs) / 1e6;
}

QString number(double value) {
    return value < 0.0 ? QString() : QString::number(value, 'f', 1);
}

double median(const std::vector<double>& measured) {
    std::vector<double> values = measured;
    values.erase(std::remove_if(values.begin(),
                         values.end(),
                         [](double value) {
                             return value < 0.0;
                         }),
            values.end());
    if (values.empty()) {
        return -1.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

double maximum(const std::vector<double>& values) {
    double result = -1.0;
    for (const double value : values) {
        result = std::max(result, value);
    }
    return result;
}

/// p50 and max of `values` (negative ones are missing), and how many.
QJsonObject stats(const std::vector<double>& values) {
    QJsonArray runs;
    for (const double value : values) {
        runs.append(value < 0.0 ? QJsonValue() : QJsonValue(std::round(value * 10.0) / 10.0));
    }
    return QJsonObject{
            {QStringLiteral("p50"), std::round(median(values) * 10.0) / 10.0},
            {QStringLiteral("max"), std::round(maximum(values) * 10.0) / 10.0},
            {QStringLiteral("runs"), runs},
    };
}

std::string measureLine(const QString& text) {
    return QStringLiteral("[ MEASURE  ] %1\n").arg(text).toStdString();
}

/// The QNetworkAccessManager of the servers' Clients. It counts the requests
/// in flight by class, as the measuring proxy does: `stream` for the audio
/// downloads, `events` for the server-sent events and `api` for the rest.
/// crate::Client does not expose a count of its own.
class CountingNetworkAccessManager : public QNetworkAccessManager {
  public:
    struct Count {
        int inFlight = 0;
        int peak = 0;
        int total = 0;
    };

    ~CountingNetworkAccessManager() override {
        // The base class deletes the replies it still owns once this
        // class's members are gone: they must not reach close() any more
        const QList<QNetworkReply*> replies = findChildren<QNetworkReply*>();
        for (QNetworkReply* pReply : replies) {
            QObject::disconnect(pReply, nullptr, this, nullptr);
        }
    }

    static QString classOf(const QString& path) {
        if (path.startsWith(QLatin1String("/api/tracks/by-entity/")) &&
                path.endsWith(QLatin1String("/stream"))) {
            return QStringLiteral("stream");
        }
        if (path.startsWith(QLatin1String("/api/cache/events"))) {
            return QStringLiteral("events");
        }
        return QStringLiteral("api");
    }

    Count count(const QString& kind) const {
        return m_counts.value(kind);
    }

    /// Starts a new window: each peak is what is in flight now.
    void resetPeaks() {
        for (auto it = m_counts.begin(); it != m_counts.end(); ++it) {
            it->peak = it->inFlight;
        }
    }

  protected:
    QNetworkReply* createRequest(Operation operation,
            const QNetworkRequest& request,
            QIODevice* pOutgoingData) override {
        QNetworkReply* pReply =
                QNetworkAccessManager::createRequest(operation, request, pOutgoingData);
        const QString kind = classOf(request.url().path());
        Count& count = m_counts[kind];
        ++count.inFlight;
        ++count.total;
        count.peak = std::max(count.peak, count.inFlight);
        m_open.insert(pReply, kind);
        connect(pReply, &QNetworkReply::finished, this, [this, pReply] {
            close(pReply);
        });
        connect(pReply, &QObject::destroyed, this, [this, pReply] {
            close(pReply);
        });
        return pReply;
    }

  private:
    void close(QNetworkReply* pReply) {
        const auto it = m_open.constFind(pReply);
        if (it == m_open.cend()) {
            return;
        }
        --m_counts[it.value()].inFlight;
        m_open.erase(it);
    }

    QHash<QString, Count> m_counts;
    QHash<QNetworkReply*, QString> m_open;
};

/// The longest gaps between the ticks of a 1 ms timer on the main thread:
/// how long it was blocked, at most, and how often over 16 ms.
class MainThreadStalls {
  public:
    MainThreadStalls() {
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(1);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] {
            tick();
        });
        m_timer.start();
    }

    /// Stops measuring around the test's own blocking work, such as copying
    /// a file.
    void pause() {
        m_timer.stop();
        m_clock.invalidate();
        m_excludedMs = 0.0;
    }
    void resume() {
        m_timer.start();
    }

    double longestMs() const {
        return m_longestMs;
    }

    /// Leaves `ms` of the test's own work on the main thread out of the
    /// current gap.
    void exclude(double ms) {
        m_excludedMs += ms;
    }

    /// What the test is doing, recorded with each stall over 16 ms.
    void setPhase(const QString& phase) {
        m_phase = phase;
    }

    QJsonObject toJson() const {
        QJsonObject histogram;
        for (std::size_t i = 0; i < m_buckets.size(); ++i) {
            histogram.insert(kBucketNames[i], m_buckets[i]);
        }
        return QJsonObject{
                {QStringLiteral("longest_ms"), std::round(m_longestMs * 10.0) / 10.0},
                {QStringLiteral("over_16ms"), overBudget()},
                {QStringLiteral("histogram_over_16ms"), histogram},
                {QStringLiteral("budget_ms"), kStallBudgetMs},
                {QStringLiteral("within_budget"), m_longestMs <= kStallBudgetMs},
                {QStringLiteral("stalls_over_16ms"), stallsJson()},
        };
    }

    QString summary() const {
        QStringList parts;
        for (std::size_t i = 0; i < m_buckets.size(); ++i) {
            parts.append(QStringLiteral("%1 ms: %2")
                            .arg(kBucketNames[i], QString::number(m_buckets[i])));
        }
        QStringList longest;
        const QJsonArray stalls = stallsJson();
        for (qsizetype i = 0; i < stalls.size() && i < 3; ++i) {
            const QJsonObject stall = stalls[i].toObject();
            longest.append(QStringLiteral("%1 ms in %2")
                            .arg(number(stall.value(QStringLiteral("ms"))
                                                 .toDouble()),
                                    stall.value(QStringLiteral("phase"))
                                            .toString()));
        }
        return QStringLiteral("longest main-thread stall %1 ms; over 16 ms: %2 (%3)%4")
                .arg(number(m_longestMs),
                        QString::number(overBudget()),
                        parts.join(QStringLiteral(", ")),
                        longest.isEmpty() ? QString()
                                          : QStringLiteral("; longest: ") +
                                        longest.join(QStringLiteral(", ")));
    }

  private:
    static inline const std::array<QString, 5> kBucketNames = {
            QStringLiteral("16-33"),
            QStringLiteral("33-50"),
            QStringLiteral("50-100"),
            QStringLiteral("100-250"),
            QStringLiteral("250+"),
    };

    void tick() {
        if (!m_clock.isValid()) {
            m_clock.start();
            return;
        }
        const double gapMs =
                std::max(0.0, static_cast<double>(m_clock.nsecsElapsed()) / 1e6 - m_excludedMs);
        m_excludedMs = 0.0;
        m_clock.restart();
        m_longestMs = std::max(m_longestMs, gapMs);
        if (gapMs <= kStallBudgetMs) {
            return;
        }
        m_stalls.emplace_back(gapMs, m_phase);
        std::size_t bucket = 4;
        if (gapMs <= 33.0) {
            bucket = 0;
        } else if (gapMs <= 50.0) {
            bucket = 1;
        } else if (gapMs <= 100.0) {
            bucket = 2;
        } else if (gapMs <= 250.0) {
            bucket = 3;
        }
        ++m_buckets[bucket];
    }

    /// The 20 longest stalls over 16 ms, with what the test was doing.
    QJsonArray stallsJson() const {
        std::vector<std::pair<double, QString>> stalls = m_stalls;
        std::sort(stalls.begin(), stalls.end(), [](const auto& a, const auto& b) {
            return a.first > b.first;
        });
        QJsonArray result;
        for (std::size_t i = 0; i < stalls.size() && i < 20; ++i) {
            result.append(QJsonObject{
                    {QStringLiteral("ms"), std::round(stalls[i].first * 10.0) / 10.0},
                    {QStringLiteral("phase"), stalls[i].second},
            });
        }
        return result;
    }

    int overBudget() const {
        int count = 0;
        for (const int bucket : m_buckets) {
            count += bucket;
        }
        return count;
    }

    QTimer m_timer;
    QElapsedTimer m_clock;
    QString m_phase;
    double m_excludedMs = 0.0;
    std::vector<std::pair<double, QString>> m_stalls;
    double m_longestMs = 0.0;
    std::array<int, 5> m_buckets{};
};

/// A volume with `initialFreeBytes` free that shrinks with what is written
/// into `directory` (files and part files, against what was there at the
/// start). Keeps the lowest free space it reported or was sampled at.
class ShrinkingStorage : public crate::cache::StorageInfo {
  public:
    ShrinkingStorage(const QString& directory, qint64 initialFreeBytes)
            : m_directory(directory),
              m_initialFreeBytes(initialFreeBytes),
              m_startBytes(bytesIn(directory)),
              m_lowest(initialFreeBytes) {
    }

    qint64 bytesAvailable(const QString& directory) const override {
        Q_UNUSED(directory);
        return sample();
    }

    qint64 sample() const {
        const qint64 freeBytes = m_initialFreeBytes - (bytesIn(m_directory) - m_startBytes);
        qint64 lowest = m_lowest.load();
        while (freeBytes < lowest && !m_lowest.compare_exchange_weak(lowest, freeBytes)) {
        }
        return freeBytes;
    }

    qint64 lowest() const {
        return m_lowest.load();
    }

  private:
    static qint64 bytesIn(const QString& directory) {
        qint64 bytes = 0;
        QDirIterator it(directory,
                QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            bytes += it.fileInfo().size();
        }
        return bytes;
    }

    const QString m_directory;
    const qint64 m_initialFreeBytes;
    const qint64 m_startBytes;
    mutable std::atomic<qint64> m_lowest;
};

/// A CSV file, written at the end of a test so that the measured part does
/// no file I/O of its own.
class Csv {
  public:
    explicit Csv(const QStringList& columns) {
        m_lines.append(columns.join(QLatin1Char(',')));
    }

    void row(const QStringList& cells) {
        QStringList quoted;
        for (const QString& cell : cells) {
            if (cell.contains(QLatin1Char(',')) || cell.contains(QLatin1Char('"'))) {
                QString escaped = cell;
                escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
                quoted.append(QStringLiteral("\"%1\"").arg(escaped));
            } else {
                quoted.append(cell);
            }
        }
        m_lines.append(quoted.join(QLatin1Char(',')));
    }

    bool write(const QString& path) const {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            return false;
        }
        file.write((m_lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
        return true;
    }

  private:
    QStringList m_lines;
};

/// The kind of a failed grid check, from its reason.
QString reasonKind(const QString& reason) {
    if (reason.contains(QLatin1String("not quantised"))) {
        return QStringLiteral("bpm_not_whole_or_half");
    }
    if (reason.contains(QLatin1String("off a constant grid"))) {
        return QStringLiteral("deviation");
    }
    if (reason.contains(QLatin1String("drift"))) {
        return QStringLiteral("drift");
    }
    if (reason.contains(QLatin1String("less than a beat apart"))) {
        return QStringLiteral("beats_too_close");
    }
    if (reason.startsWith(QLatin1String("Only"))) {
        return QStringLiteral("too_few_beats");
    }
    return QStringLiteral("no_bpm");
}

/// What the measuring proxy had seen before a test's window.
struct ProxyMark {
    QJsonObject summary;
    /// Rows of its CSV of requests, the header included.
    int rows = 0;
};

/// One load into a deck, from the "click".
struct LoadTiming {
    QString uid;
    QString title;
    qint64 bytes = -1;
    /// Until the file was in the cache; -1 when it already was.
    double downloadMs = -1.0;
    /// Until the deck's engine had the track (it could play).
    double loadedMs = -1.0;
    /// Until the track got its waveform: the analysis made it
    /// (Track::waveformUpdated, it fills from there) or the track already
    /// had one when it loaded.
    double waveformMs = -1.0;
    bool waveformAtLoad = false;
    /// The analyzer's first progress above 0, and its end.
    double firstProgressMs = -1.0;
    double analysisDoneMs = -1.0;
    TrackPointer pTrack;
    bool loaded = false;
};

/// What one load watches, shared with the analyzer threads.
struct LoadProbe {
    TrackPointer pTrack;
    std::atomic<qint64> waveformNs{-1};
    bool waveformAtLoad = false;
    qint64 firstProgressNs = -1;
    qint64 doneNs = -1;
};

class CrateG3BudgetsLive : public MixxxDbTest, SoundSourceProviderRegistration {
  public:
    CrateG3BudgetsLive()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        m_url = qEnvironmentVariable("CRATE_DECKS_LIVE_URL");
        if (m_url.isEmpty()) {
            GTEST_SKIP() << "Live measurement against a Crate dev stack (G3): set "
                            "CRATE_DECKS_LIVE_URL, CRATE_DECKS_LIVE_EMAIL, "
                            "CRATE_DECKS_LIVE_PASSWORD and CRATE_DECKS_LIVE_OUT";
        }
        m_outDir = qEnvironmentVariable("CRATE_DECKS_LIVE_OUT");
        const QString email = qEnvironmentVariable("CRATE_DECKS_LIVE_EMAIL");
        const QString password = qEnvironmentVariable("CRATE_DECKS_LIVE_PASSWORD");
        ASSERT_FALSE(m_outDir.isEmpty()) << "Set CRATE_DECKS_LIVE_OUT";
        ASSERT_FALSE(email.isEmpty() || password.isEmpty())
                << "Set CRATE_DECKS_LIVE_EMAIL and CRATE_DECKS_LIVE_PASSWORD";
        ASSERT_TRUE(QDir().mkpath(m_outDir));
        m_pWork = std::make_unique<QTemporaryDir>(
                QDir(m_outDir).filePath(QStringLiteral("work-XXXXXX")));
        ASSERT_TRUE(m_pWork->isValid());
        m_clock.start();
        m_enginePump.start();

        // As coreservices.cpp and CrateLoadIntoDecksTest build them
        m_started = true;
        auto pChannelHandleFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager = std::make_shared<EffectsManager>(m_pConfig, pChannelHandleFactory);
        m_pEngine = std::make_shared<EngineMixer>(
                m_pConfig, "[Master]", m_pEffectsManager.get(), pChannelHandleFactory, true);
        m_pSoundManager = std::make_shared<SoundManager>(m_pConfig, m_pEngine.get());
        m_pControlIndicatorTimer = std::make_shared<mixxx::ControlIndicatorTimer>(nullptr);
        m_pEngine->registerNonEngineChannelSoundIO(gsl::make_not_null(m_pSoundManager.get()));
        CoverArtCache::createInstance();
        m_pPlayerManager = std::make_shared<PlayerManager>(
                m_pConfig, m_pSoundManager.get(), m_pEffectsManager.get(), m_pEngine.get());
        m_pPlayerManager->addConfiguredDecks();
        PlayerInfo::create();
        m_pEffectsManager->setup();
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection()));
        m_pTrackCollectionManager = std::make_unique<TrackCollectionManager>(
                nullptr, m_pConfig, dbConnectionPooler(), deleteTrack);
        m_pRecordingManager = std::make_shared<RecordingManager>(m_pConfig, m_pEngine.get());

        // Crate, signed in as a new installation: secrets in memory only
        CrateServers::Dependencies dependencies;
        dependencies.pConfig = m_pConfig;
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.pSecretStore = &m_secrets;
        dependencies.audioCacheDirectory = workPath(QStringLiteral("cache"));
        m_pServers = std::make_unique<CrateServers>(dependencies);
        m_pServers->start();
        ASSERT_NO_FATAL_FAILURE(signIn(email, password));

        // The application's Library without its own Crate section: this
        // test adds one on the servers above
        m_pConfig->setValue(ConfigKey(QStringLiteral("[Library]"),
                                    QStringLiteral("ShowCrateServer")),
                false);
        m_pLibrary = std::make_shared<Library>(nullptr,
                m_pConfig,
                dbConnectionPooler(),
                m_pTrackCollectionManager.get(),
                m_pPlayerManager.get(),
                m_pRecordingManager.get());
        m_pPlayerManager->bindToLibrary(m_pLibrary.get());
#ifdef __RUBBERBAND__
        RubberBandWorkerPool::createInstance();
        m_rubberBand = true;
#endif
        // Owned by the Library, which wires its loads to the PlayerManager
        m_pFeature = new CrateServerFeature(m_pLibrary.get(), m_pConfig, featureDependencies());
        m_pLibrary->addFeature(m_pFeature);
        ASSERT_NO_FATAL_FAILURE(waitForTree(m_pFeature));
        ASSERT_NE(nullptr, m_pFeature->loadCoordinator());
    }

    void TearDown() override {
        m_pStalls = nullptr;
        m_pView.reset();
        m_pOwnCache.reset();
        m_standalone.clear();
        if (m_started) {
            CoverArtCache::destroy();
        }
#ifdef __RUBBERBAND__
        if (m_rubberBand) {
            RubberBandWorkerPool::destroy();
        }
#endif
    }

    ~CrateG3BudgetsLive() override {
        if (m_started) {
            m_pSoundManager.reset();
            m_pPlayerManager.reset();
            PlayerInfo::destroy();
            m_pLibrary.reset();
            m_pServers.reset();
            m_pRecordingManager.reset();
            m_pEngine.reset();
            m_pEffectsManager.reset();
            m_pTrackCollectionManager.reset();
            m_pControlIndicatorTimer.reset();
        }
        m_pWork.reset();
    }

  protected:
    void setPhase(const QString& phase) {
        if (m_pStalls) {
            m_pStalls->setPhase(phase);
        }
    }

    QString workPath(const QString& name) const {
        return m_pWork->filePath(name);
    }

    CrateServerFeature::Dependencies featureDependencies() const {
        CrateServerFeature::Dependencies dependencies;
        dependencies.pTrackCollectionManager = m_pTrackCollectionManager.get();
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pServers = m_pServers.get();
        return dependencies;
    }

    /// Runs the event loop until `done` or `timeoutMs`, never sleeping on the
    /// main thread, and drives the engine as a sound card would while a deck
    /// loads (m_driveEngine).
    bool wait(const std::function<bool()>& done, int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        // Wakes the loop even when nothing else happens
        QTimer wake;
        wake.setInterval(2);
        wake.start();
        while (!done()) {
            if (timer.elapsed() > timeoutMs) {
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (m_driveEngine && m_enginePump.elapsed() >= 5) {
                m_enginePump.restart();
                // The engine runs on its own thread in the application: its
                // time here is not a main-thread stall
                QElapsedTimer engine;
                engine.start();
                m_pEngine->process(512);
                const double engineMs = static_cast<double>(engine.nsecsElapsed()) / 1e6;
                m_longestEngineMs = std::max(m_longestEngineMs, engineMs);
                if (m_pStalls) {
                    m_pStalls->exclude(engineMs);
                }
            }
        }
        return true;
    }

    /// Waits until no API request has been in flight for `quietMs`.
    bool waitForApiQuiet(int quietMs, int timeoutMs) {
        QElapsedTimer quiet;
        return wait(
                [&] {
                    if (m_network.count(QStringLiteral("api")).inFlight > 0) {
                        quiet.invalidate();
                        return false;
                    }
                    if (!quiet.isValid()) {
                        quiet.start();
                    }
                    return quiet.elapsed() >= quietMs;
                },
                timeoutMs);
    }

    void signIn(const QString& email, const QString& password) {
        using Step = crate::AddServerFlow::Step;
        crate::AddServerFlow flow(m_pServers.get());
        flow.setAddress(m_url);
        if (flow.step() == Step::ConfirmLocalNetwork) {
            flow.confirmLocalNetwork();
        }
        ASSERT_TRUE(wait(
                [&flow] {
                    return flow.step() == Step::SignIn || flow.step() == Step::EnterAddress;
                },
                kNetworkWaitMs));
        ASSERT_EQ(Step::SignIn, flow.step()) << flow.error().toStdString();
        flow.loginWithPassword(email, password);
        ASSERT_TRUE(wait(
                [&flow] {
                    return flow.step() != Step::SigningIn;
                },
                kNetworkWaitMs));
        ASSERT_EQ(Step::Done, flow.step()) << flow.error().toStdString();
        const QList<crate::Session*> sessions = m_pServers->sessions();
        ASSERT_EQ(1, sessions.size());
        m_serverId = sessions.first()->profile().id;
        ASSERT_EQ(crate::Session::State::SignedIn, sessions.first()->state());
    }

    static TreeItem* childLabelled(const TreeItem* pItem, const QString& prefix) {
        for (TreeItem* pChild : pItem->children()) {
            if (pChild->getLabel().startsWith(prefix)) {
                return pChild;
            }
        }
        return nullptr;
    }

    static QModelIndex indexOf(CrateServerFeature* pFeature, TreeItem* pItem) {
        QList<int> rows;
        for (TreeItem* pCurrent = pItem; pCurrent && pCurrent->hasParent();
                pCurrent = pCurrent->parent()) {
            rows.prepend(pCurrent->parentRow());
        }
        QModelIndex index;
        for (const int row : std::as_const(rows)) {
            index = pFeature->sidebarModel()->index(row, 0, index);
        }
        return index;
    }

    /// The sidebar item of a container.
    TreeItem* itemOf(CrateServerFeature* pFeature, qint64 containerId) const {
        const QString data = QStringLiteral("container\n%1\n%2").arg(m_serverId).arg(containerId);
        QList<TreeItem*> pending{pFeature->sidebarModel()->getRootItem()};
        while (!pending.isEmpty()) {
            TreeItem* pItem = pending.takeLast();
            if (pItem->getData().toString() == data) {
                return pItem;
            }
            for (TreeItem* pChild : pItem->children()) {
                pending.append(pChild);
            }
        }
        return nullptr;
    }

    /// The id of the playlist `name`; 0 until the tree has it.
    qint64 containerId(const QString& name) const {
        QSqlQuery query(dbConnection());
        query.prepare(QStringLiteral(
                "SELECT id FROM crateserver_containers WHERE server_id = :server "
                "AND kind = 'playlist' AND name = :name"));
        query.bindValue(QStringLiteral(":server"), m_serverId);
        query.bindValue(QStringLiteral(":name"), name);
        return query.exec() && query.next() ? query.value(0).toLongLong() : 0;
    }

    /// Expands the server's playlists: its capabilities, then its tree.
    void waitForTree(CrateServerFeature* pFeature) {
        // The sidebar is read off the main thread
        ASSERT_TRUE(wait(
                [pFeature] {
                    return childLabelled(pFeature->sidebarModel()->getRootItem(),
                                   QStringLiteral("Playlists")) != nullptr;
                },
                kNetworkWaitMs));
        TreeItem* pPlaylists = childLabelled(
                pFeature->sidebarModel()->getRootItem(), QStringLiteral("Playlists"));
        pFeature->onLazyChildExpandation(indexOf(pFeature, pPlaylists));
        ASSERT_TRUE(wait(
                [&] {
                    const qint64 id = containerId(kPlaylist500);
                    return id > 0 && itemOf(pFeature, id) != nullptr;
                },
                kNetworkWaitMs))
                << "The dev stack has no playlist " << kPlaylist500.toStdString();
        ASSERT_TRUE(waitForApiQuiet(300, kNetworkWaitMs));
    }

    /// Activates a container in the sidebar, as a click does; its model.
    CrateServerTrackModel* activate(CrateServerFeature* pFeature, qint64 containerId) {
        TreeItem* pItem = itemOf(pFeature, containerId);
        if (!pItem) {
            return nullptr;
        }
        QSignalSpy shown(pFeature, &LibraryFeature::showTrackModel);
        pFeature->activateChild(indexOf(pFeature, pItem));
        if (shown.isEmpty()) {
            return nullptr;
        }
        return dynamic_cast<CrateServerTrackModel*>(
                shown.last().first().value<QAbstractItemModel*>());
    }

    /// Shows a playlist in a track table, its rows synced; nullptr if not.
    CrateServerTrackModel* showInTable(const QString& name, int rows) {
        const qint64 id = containerId(name);
        CrateServerTrackModel* pModel = id > 0 ? activate(m_pFeature, id) : nullptr;
        if (!pModel ||
                !wait(
                        [pModel, rows] {
                            return pModel->rowCount() == rows;
                        },
                        kNetworkWaitMs)) {
            return nullptr;
        }
        m_pView = std::make_unique<WTrackTableView>(nullptr, m_pConfig, m_pLibrary.get(), 1.0);
        // As Library::bindLibraryWidget() wires the library's track table:
        // cached tracks load through it
        QObject::connect(m_pView.get(),
                &WTrackTableView::loadTrack,
                m_pLibrary.get(),
                &Library::slotLoadTrack);
        QObject::connect(m_pView.get(),
                &WTrackTableView::loadTrackToPlayer,
                m_pLibrary.get(),
                &Library::slotLoadTrackToPlayer);
        m_pView->loadTrackModel(pModel);
        return pModel;
    }

    int rowOf(CrateServerTrackModel* pModel, const QString& uid) const {
        for (int row = 0; row < pModel->rowCount(); ++row) {
            if (pModel->entityUid(row) == uid) {
                return row;
            }
        }
        return -1;
    }

    struct TrackInfo {
        QString title;
        QString artist;
        QString format;
        qint64 bytes = -1;
        double bpm = 0.0;
        QString profileQuality;
        bool hasProfile = false;
    };
    TrackInfo trackInfo(const QString& uid) {
        TrackInfo info;
        for (int attempt = 0; attempt < 20; ++attempt) {
            QSqlQuery query(dbConnection());
            query.prepare(QStringLiteral(
                    "SELECT title, artist, format, byte_length, bpm, profile_quality, "
                    "profile_revision FROM crateserver_tracks "
                    "WHERE server_id = :server AND entity_uid = :uid"));
            query.bindValue(QStringLiteral(":server"), m_serverId);
            query.bindValue(QStringLiteral(":uid"), uid);
            if (!query.exec()) {
                wait(
                        [] {
                            return false;
                        },
                        25);
                continue;
            }
            if (query.next()) {
                info.title = query.value(0).toString();
                info.artist = query.value(1).toString();
                info.format = query.value(2).toString();
                info.bytes = query.value(3).isNull() ? -1 : query.value(3).toLongLong();
                info.bpm = query.value(4).toDouble();
                info.profileQuality = query.value(5).toString();
                info.hasProfile = !query.value(6).toString().isEmpty();
            }
            break;
        }
        return info;
    }

    /// The tracks of a container with a profile, in order.
    QStringList uidsWithProfile(qint64 containerId) const {
        QSqlQuery query(dbConnection());
        query.prepare(QStringLiteral(
                "SELECT ct.entity_uid FROM crateserver_container_tracks ct "
                "JOIN crateserver_tracks t ON t.server_id = :server "
                "AND t.entity_uid = ct.entity_uid "
                "WHERE ct.container_id = :container AND t.profile_revision IS NOT NULL "
                "AND t.profile_revision != '' ORDER BY ct.position"));
        query.bindValue(QStringLiteral(":server"), m_serverId);
        query.bindValue(QStringLiteral(":container"), containerId);
        QStringList uids;
        if (query.exec()) {
            while (query.next()) {
                uids.append(query.value(0).toString());
            }
        }
        return uids;
    }

    crate::cache::AudioCache* cache() const {
        return m_pServers->audioCache();
    }

    /// Loads into a deck with `click` and measures it: until the deck's
    /// engine has a track that `isExpected`, then the analysis, until it ends
    /// or `analysisWaitMs`. `uid` is the Crate track's, empty for a local one.
    LoadTiming measureLoad(int deckIndex,
            const std::function<void()>& click,
            const std::function<bool(const TrackPointer&)>& isExpected,
            const QString& uid,
            int analysisWaitMs) {
        LoadTiming timing;
        timing.uid = uid;
        Deck* pDeck = m_pPlayerManager->getDeck(deckIndex);
        EngineBuffer* pBuffer = pDeck->getEngineDeck()->getEngineBuffer();
        auto pProbe = std::make_shared<LoadProbe>();
        QObject context;
        QObject* pContext = &context;
        const QElapsedTimer* pClock = &m_clock;
        QObject::connect(pDeck,
                &BaseTrackPlayer::loadingTrack,
                pContext,
                [pProbe, pClock, pContext](const TrackPointer& pNew, const TrackPointer& pOld) {
                    Q_UNUSED(pOld);
                    if (pProbe->pTrack || !pNew) {
                        return;
                    }
                    pProbe->pTrack = pNew;
                    if (pNew->getWaveform()) {
                        pProbe->waveformAtLoad = true;
                        pProbe->waveformNs = pClock->nsecsElapsed();
                    }
                    // From the analyzer's thread
                    QObject::connect(
                            pNew.get(),
                            &Track::waveformUpdated,
                            pContext,
                            [pProbe, pClock] {
                                qint64 none = -1;
                                pProbe->waveformNs.compare_exchange_strong(
                                        none, pClock->nsecsElapsed());
                            },
                            Qt::DirectConnection);
                });
        QObject::connect(m_pPlayerManager.get(),
                &PlayerManager::trackAnalyzerProgress,
                pContext,
                [pProbe, pClock](const TrackId& trackId, AnalyzerProgress progress) {
                    if (!pProbe->pTrack || trackId != pProbe->pTrack->getId()) {
                        return;
                    }
                    const qint64 now = pClock->nsecsElapsed();
                    if (pProbe->firstProgressNs < 0 && progress > kAnalyzerProgressNone) {
                        pProbe->firstProgressNs = now;
                    }
                    if (progress >= kAnalyzerProgressDone) {
                        pProbe->doneNs = now;
                    }
                });

        crate::cache::AudioCache* pCache = cache();
        const bool cachedAtClick =
                uid.isEmpty() || pCache->cachedPath(m_serverId, uid).has_value();
        m_driveEngine = true;
        // Two loads into a deck within 0.5 s clone the other deck instead
        // ([Controls],CloneDeckOnLoadDoubleTap)
        const qint64 lastClickNs = m_lastClickNs.value(deckIndex, -1);
        if (lastClickNs >= 0) {
            wait(
                    [this, lastClickNs] {
                        return msBetween(lastClickNs, m_clock.nsecsElapsed()) > 700.0;
                    },
                    1000);
        }
        const QString name = uid.isEmpty() ? QStringLiteral("local track") : uid;
        setPhase(QStringLiteral("load %1: until loaded").arg(name));
        const qint64 clickNs = m_clock.nsecsElapsed();
        m_lastClickNs.insert(deckIndex, clickNs);
        click();
        timing.loaded = wait(
                [&] {
                    if (!cachedAtClick && timing.downloadMs < 0.0 &&
                            pCache->cachedPath(m_serverId, uid)) {
                        timing.downloadMs = msBetween(clickNs, m_clock.nsecsElapsed());
                    }
                    const TrackPointer pLoaded = pBuffer->getLoadedTrack();
                    return pLoaded && isExpected(pLoaded) && pBuffer->isTrackLoaded();
                },
                kLoadWaitMs);
        timing.loadedMs = msBetween(clickNs, m_clock.nsecsElapsed());
        if (timing.loaded) {
            timing.pTrack = pBuffer->getLoadedTrack();
            setPhase(QStringLiteral("load %1: analysis").arg(name));
            wait(
                    [pProbe] {
                        return pProbe->doneNs >= 0;
                    },
                    analysisWaitMs);
        }
        m_driveEngine = false;
        const qint64 waveformNs = pProbe->waveformNs.load();
        timing.waveformMs = waveformNs >= 0 ? std::max(0.0, msBetween(clickNs, waveformNs)) : -1.0;
        timing.waveformAtLoad = pProbe->waveformAtLoad;
        timing.firstProgressMs =
                pProbe->firstProgressNs >= 0 ? msBetween(clickNs, pProbe->firstProgressNs) : -1.0;
        timing.analysisDoneMs = pProbe->doneNs >= 0 ? msBetween(clickNs, pProbe->doneNs) : -1.0;
        const TrackPointer pInEngine = pBuffer->getLoadedTrack();
        std::cout << QStringLiteral(
                "[ LOAD     ] %1 into deck %2: %3 after %4 ms (deck loading %5; engine has %6)\n")
                             .arg(uid.isEmpty() ? QStringLiteral("local track") : uid,
                                     QString::number(deckIndex + 1),
                                     timing.loaded ? QStringLiteral("loaded")
                                                   : QStringLiteral("NOT loaded"),
                                     number(timing.loadedMs),
                                     pProbe->pTrack ? pProbe->pTrack->getLocation()
                                                    : QStringLiteral("nothing"),
                                     pInEngine ? pInEngine->getLocation()
                                               : QStringLiteral("nothing"))
                             .toStdString();
        return timing;
    }

    /// The "click" on a Crate row: LoadSelectedTrack into `group`, as the
    /// keyboard, a controller or a double click into that deck do.
    LoadTiming loadRow(CrateServerTrackModel* pModel,
            const QString& uid,
            const QString& group,
            int analysisWaitMs) {
        const int row = rowOf(pModel, uid);
        if (row < 0) {
            LoadTiming missing;
            missing.uid = uid;
            return missing;
        }
        WTrackTableView* pView = m_pView.get();
        crate::cache::AudioCache* pCache = cache();
        const QString serverId = m_serverId;
        return measureLoad(
                group == kDeck1 ? 0 : 1,
                [pView, pModel, row, group] {
                    pView->selectionModel()->select(pModel->index(row, 0),
                            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                    pView->loadSelectedTrackToGroup(group,
#ifdef __STEM__
                            mixxx::StemChannelSelection(),
#endif
                            false);
                },
                [pCache, serverId, uid](const TrackPointer& pTrack) {
                    const auto key = pCache->trackOfFile(pTrack->getLocation());
                    return key && key->serverId == serverId && key->entityUid == uid;
                },
                uid,
                analysisWaitMs);
    }

    /// A local track of the library (not Crate's) into `group`, as the
    /// library's own track table loads it.
    LoadTiming loadLocal(const QString& path, const QString& group, int analysisWaitMs) {
        setPhase(QStringLiteral("local track: taken from the library"));
        const TrackPointer pTrack =
                m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(path));
        if (!pTrack) {
            return LoadTiming();
        }
        PlayerManager* pPlayers = m_pPlayerManager.get();
        return measureLoad(
                group == kDeck1 ? 0 : 1,
                [pPlayers, pTrack, group] {
                    pPlayers->slotLoadTrackToPlayer(pTrack,
                            group,
#ifdef __STEM__
                            mixxx::StemChannelSelection(),
#endif
                            false);
                },
                [path](const TrackPointer& pLoaded) {
                    return QFileInfo(pLoaded->getLocation()).canonicalFilePath() ==
                            QFileInfo(path).canonicalFilePath();
                },
                QString(),
                analysisWaitMs);
    }

    /// F0 Q10 for the playlist in `pModel`: every track with an empty cache,
    /// then again with all of them cached. Its format and its results.
    std::pair<QString, QJsonObject> measureQ10(
            CrateServerTrackModel* pModel, const QString& name, Csv* pCsv) {
        QStringList uids;
        for (int row = 0; row < pModel->rowCount(); ++row) {
            uids.append(pModel->entityUid(row));
        }
        const QString format = uids.isEmpty() ? QString() : trackInfo(uids.first()).format;
        std::vector<double> downloadMs;
        std::vector<double> sizesMb;
        std::vector<double> rates;
        std::vector<double> emptyCache;
        std::vector<double> fullCache;
        for (const bool first : {true, false}) {
            const QString pass = first ? QStringLiteral("empty-cache")
                                       : QStringLiteral("full-cache");
            for (int i = 0; i < uids.size(); ++i) {
                const QString& uid = uids[i];
                const TrackInfo info = trackInfo(uid);
                const LoadTiming timing = loadRow(pModel,
                        uid,
                        i % 2 == 0 ? kDeck1 : kDeck2,
                        first ? kAnalysisWaitMs : kAgainWaitMs);
                EXPECT_TRUE(timing.loaded) << uid.toStdString();
                (first ? emptyCache : fullCache)
                        .push_back(timing.loaded ? timing.loadedMs : -1.0);
                double rate = -1.0;
                if (first) {
                    downloadMs.push_back(timing.downloadMs);
                    sizesMb.push_back(static_cast<double>(info.bytes) / 1e6);
                    if (timing.downloadMs > 0.0) {
                        rate = static_cast<double>(info.bytes) / 1e6 / (timing.downloadMs / 1000.0);
                        rates.push_back(rate);
                    }
                }
                pCsv->row({format,
                        pass,
                        uid,
                        info.title,
                        QString::number(info.bytes),
                        number(timing.downloadMs),
                        number(rate),
                        timing.loaded ? number(timing.loadedMs) : QStringLiteral("failed"),
                        number(timing.waveformMs)});
            }
        }
        std::cout << measureLine(
                QStringLiteral("Q10 %1 (%2 tracks): empty cache download p50 %3 ms (%4 MB, "
                               "%5 MB/s), click to loaded p50 %6 ms; full cache click to "
                               "loaded p50 %7 ms")
                        .arg(format,
                                QString::number(uids.size()),
                                number(median(downloadMs)),
                                number(median(sizesMb)),
                                number(median(rates)),
                                number(median(emptyCache)),
                                number(median(fullCache))));
        return {format,
                QJsonObject{
                        {QStringLiteral("playlist"), name},
                        {QStringLiteral("tracks"), static_cast<int>(uids.size())},
                        {QStringLiteral("size_mb_p50"), std::round(median(sizesMb) * 10.0) / 10.0},
                        {QStringLiteral("download_ms"), stats(downloadMs)},
                        {QStringLiteral("download_mb_per_s_p50"),
                                std::round(median(rates) * 10.0) / 10.0},
                        {QStringLiteral("empty_cache_loaded_ms"), stats(emptyCache)},
                        {QStringLiteral("full_cache_loaded_ms"), stats(fullCache)},
                }};
    }

    ProxyMark proxyMark() const {
        ProxyMark mark;
        QFile summary(qEnvironmentVariable("CRATE_DECKS_LIVE_PROXY_SUMMARY"));
        if (!summary.fileName().isEmpty() && summary.open(QIODevice::ReadOnly)) {
            mark.summary = QJsonDocument::fromJson(summary.readAll()).object();
        }
        QFile requests(qEnvironmentVariable("CRATE_DECKS_LIVE_PROXY_REQUESTS"));
        if (!requests.fileName().isEmpty() && requests.open(QIODevice::ReadOnly)) {
            mark.rows = static_cast<int>(requests.readAll().count('\n'));
        }
        return mark;
    }

    /// The highest number of requests in flight at once, by class, among the
    /// requests the proxy logged after `mark` (its CSV).
    QJsonObject proxyWindowPeaks(const ProxyMark& mark) const {
        QFile requests(qEnvironmentVariable("CRATE_DECKS_LIVE_PROXY_REQUESTS"));
        if (requests.fileName().isEmpty() || !requests.open(QIODevice::ReadOnly)) {
            return QJsonObject();
        }
        const QList<QByteArray> lines = requests.readAll().split('\n');
        // Each request's interval, by class
        QHash<QString, std::vector<std::pair<double, int>>> edges;
        QHash<QString, int> atStart;
        for (int i = std::max(1, mark.rows); i < lines.size(); ++i) {
            const QList<QByteArray> cells = lines[i].split(',');
            if (cells.size() < 8) {
                continue;
            }
            const QString kind = QString::fromUtf8(cells[2]);
            edges[kind].emplace_back(cells[0].toDouble(), 1);
            edges[kind].emplace_back(cells[1].toDouble(), -1);
            atStart[kind] = std::max(atStart.value(kind), cells.last().trimmed().toInt());
        }
        QJsonObject peaks;
        for (auto it = edges.begin(); it != edges.end(); ++it) {
            std::vector<std::pair<double, int>>& kindEdges = it.value();
            // An end before a start at the same time
            std::sort(kindEdges.begin(), kindEdges.end());
            int inFlight = 0;
            int peak = 0;
            for (const auto& edge : kindEdges) {
                inFlight += edge.second;
                peak = std::max(peak, inFlight);
            }
            peaks.insert(it.key(),
                    QJsonObject{
                            {QStringLiteral("overlap_peak"), peak},
                            {QStringLiteral("inflight_at_start_max"), atStart.value(it.key())},
                    });
        }
        return peaks;
    }

    /// Adds `section` as the part of `test` in budgets.json.
    void record(const QString& test, const QJsonObject& section) const {
        QFile file(QDir(m_outDir).filePath(QStringLiteral("budgets.json")));
        QJsonObject root;
        if (file.open(QIODevice::ReadOnly)) {
            root = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
        }
        QJsonObject tests = root.value(QStringLiteral("tests")).toObject();
        QJsonObject entry = section;
        entry.insert(QStringLiteral("measured_at"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        tests.insert(test, entry);
        root.insert(QStringLiteral("tests"), tests);
        root.insert(QStringLiteral("server"), m_url);
        root.insert(QStringLiteral("design"), QStringLiteral("crate-decks design §9"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
    }

    /// The stalls of a test, with the engine calls it left out of them.
    QJsonObject mainThread(const MainThreadStalls& stalls) const {
        QJsonObject json = stalls.toJson();
        json.insert(QStringLiteral("engine_callback_longest_ms"),
                std::round(m_longestEngineMs * 10.0) / 10.0);
        json.insert(QStringLiteral("engine_callbacks"),
                QStringLiteral("left out: the test drives the engine on the main thread, as a "
                               "sound card does on the engine's own thread"));
        return json;
    }

    QString outPath(const QString& name) const {
        return QDir(m_outDir).filePath(name);
    }

    QString m_url;
    QString m_outDir;
    QString m_serverId;
    std::unique_ptr<QTemporaryDir> m_pWork;
    QElapsedTimer m_clock;
    QElapsedTimer m_enginePump;
    bool m_driveEngine = false;
    /// The test's stall detector, for the phases.
    MainThreadStalls* m_pStalls = nullptr;
    /// The longest call of the engine that the test makes as a sound card.
    double m_longestEngineMs = 0.0;
    /// When each deck was last loaded, by deck index.
    QHash<int, qint64> m_lastClickNs;
    bool m_started = false;
    bool m_rubberBand = false;
    /// What the main window's timer drives; the track table reads it.
    ControlObject m_guiTick{ConfigKey(
            QStringLiteral("[App]"), QStringLiteral("gui_tick_50ms_period_s"))};
    CountingNetworkAccessManager m_network;
    crate::SessionOnlySecretStore m_secrets;
    std::shared_ptr<EffectsManager> m_pEffectsManager;
    std::shared_ptr<mixxx::ControlIndicatorTimer> m_pControlIndicatorTimer;
    std::shared_ptr<EngineMixer> m_pEngine;
    std::shared_ptr<SoundManager> m_pSoundManager;
    std::shared_ptr<PlayerManager> m_pPlayerManager;
    std::unique_ptr<TrackCollectionManager> m_pTrackCollectionManager;
    std::shared_ptr<RecordingManager> m_pRecordingManager;
    std::unique_ptr<CrateServers> m_pServers;
    std::shared_ptr<Library> m_pLibrary;
    CrateServerFeature* m_pFeature = nullptr;
    std::unique_ptr<WTrackTableView> m_pView;
    /// Features made by a test besides the Library's.
    std::vector<std::unique_ptr<CrateServerFeature>> m_standalone;
    /// An audio cache a test makes besides the servers'.
    std::unique_ptr<crate::cache::AudioCache> m_pOwnCache;
};

// 1. Opening the 500-track playlist (§9 "Barra lateral"): from the network
// (cold: its tracks are not in the tables, the server's capabilities and tree
// already read) and from the synced tables (warm), five times each, from the
// click in the sidebar to the rows in the model.
TEST_F(CrateG3BudgetsLive, OpenContainer) {
    const qint64 playlist = containerId(kPlaylist500);
    const qint64 other = containerId(kAutoDj);
    ASSERT_GT(playlist, 0);
    ASSERT_GT(other, 0);
    MainThreadStalls stalls;
    m_pStalls = &stalls;
    Csv csv({QStringLiteral("kind"),
            QStringLiteral("run"),
            QStringLiteral("ms"),
            QStringLiteral("rows"),
            QStringLiteral("api_requests")});

    std::vector<double> cold;
    CrateServerFeature* pFeature = nullptr;
    for (int run = 0; run < kRuns; ++run) {
        setPhase(QStringLiteral("cold open %1: a new feature and its tree").arg(run + 1));
        // A new feature each time: a new run of the application that opens
        // the playlist for the first time
        m_standalone.push_back(std::make_unique<CrateServerFeature>(
                nullptr, m_pConfig, featureDependencies()));
        pFeature = m_standalone.back().get();
        ASSERT_NO_FATAL_FAILURE(waitForTree(pFeature));
        ASSERT_TRUE(wait(
                [&] {
                    return crate::library::replaceContainerTracks(dbConnection(),
                                   playlist,
                                   {},
                                   crate::library::SyncRecord()) ==
                            crate::library::WriteStatus::Ok;
                },
                kNetworkWaitMs));
        const int requestsBefore = m_network.count(QStringLiteral("api")).total;
        setPhase(QStringLiteral("cold open %1").arg(run + 1));
        const qint64 clickNs = m_clock.nsecsElapsed();
        CrateServerTrackModel* pModel = activate(pFeature, playlist);
        ASSERT_NE(nullptr, pModel);
        ASSERT_TRUE(wait(
                [pModel] {
                    return pModel->rowCount() == 500;
                },
                kNetworkWaitMs))
                << "rows: " << pModel->rowCount();
        cold.push_back(msBetween(clickNs, m_clock.nsecsElapsed()));
        ASSERT_TRUE(waitForApiQuiet(300, kNetworkWaitMs));
        csv.row({QStringLiteral("cold"),
                QString::number(run + 1),
                number(cold.back()),
                QString::number(pModel->rowCount()),
                QString::number(m_network.count(QStringLiteral("api")).total - requestsBefore)});
        if (run + 1 < kRuns) {
            m_standalone.pop_back();
        }
    }

    // Synced: back from another playlist, in the same run
    std::vector<double> warm;
    for (int run = 0; run < kRuns; ++run) {
        setPhase(QStringLiteral("warm open %1: another playlist").arg(run + 1));
        CrateServerTrackModel* pOther = activate(pFeature, other);
        ASSERT_NE(nullptr, pOther);
        ASSERT_TRUE(wait(
                [pOther] {
                    return pOther->rowCount() > 0 && pOther->rowCount() < 500;
                },
                kNetworkWaitMs));
        ASSERT_TRUE(waitForApiQuiet(300, kNetworkWaitMs));
        const int requestsBefore = m_network.count(QStringLiteral("api")).total;
        setPhase(QStringLiteral("warm open %1").arg(run + 1));
        const qint64 clickNs = m_clock.nsecsElapsed();
        CrateServerTrackModel* pModel = activate(pFeature, playlist);
        ASSERT_NE(nullptr, pModel);
        ASSERT_TRUE(wait(
                [pModel] {
                    return pModel->rowCount() == 500;
                },
                kNetworkWaitMs));
        warm.push_back(msBetween(clickNs, m_clock.nsecsElapsed()));
        ASSERT_TRUE(waitForApiQuiet(300, kNetworkWaitMs));
        csv.row({QStringLiteral("warm"),
                QString::number(run + 1),
                number(warm.back()),
                QString::number(pModel->rowCount()),
                QString::number(m_network.count(QStringLiteral("api")).total - requestsBefore)});
    }
    stalls.pause();

    std::cout << measureLine(QStringLiteral(
            "open 500-track playlist, cold (network): p50 %1 ms, "
            "max %2 ms (budget < %3 ms)")
                    .arg(number(median(cold)),
                            number(maximum(cold)),
                            number(kColdOpenBudgetMs)));
    std::cout << measureLine(QStringLiteral(
            "open 500-track playlist, warm (tables): p50 %1 ms, "
            "max %2 ms (budget < %3 ms)")
                    .arg(number(median(warm)),
                            number(maximum(warm)),
                            number(kWarmOpenBudgetMs)));
    std::cout << measureLine(QStringLiteral("OpenContainer: ") + stalls.summary());
    ASSERT_TRUE(csv.write(outPath(QStringLiteral("open-container.csv"))));
    record(QStringLiteral("OpenContainer"),
            QJsonObject{
                    {QStringLiteral("playlist"), kPlaylist500},
                    {QStringLiteral("cold_ms"), stats(cold)},
                    {QStringLiteral("warm_ms"), stats(warm)},
                    {QStringLiteral("budget_ms"),
                            QJsonObject{{QStringLiteral("cold"), kColdOpenBudgetMs},
                                    {QStringLiteral("warm"), kWarmOpenBudgetMs}}},
                    {QStringLiteral("within_budget"),
                            QJsonObject{{QStringLiteral("cold"), maximum(cold) < kColdOpenBudgetMs},
                                    {QStringLiteral("warm"),
                                            maximum(warm) < kWarmOpenBudgetMs}}},
                    {QStringLiteral("main_thread"), mainThread(stalls)},
            });
}

// 2. Loading into a deck (§9 "Carga en deck"): ten FLACs of about 40 MB
// from the click to the deck's engine having the track and to its waveform,
// uncached and then cached; and the same files as local tracks of the
// library, the first time and again.
TEST_F(CrateG3BudgetsLive, LoadIntoDeck) {
    CrateServerTrackModel* pModel = showInTable(kFlacs, 20);
    ASSERT_NE(nullptr, pModel);
    // The ten largest FLACs up to 50 MB
    std::vector<std::pair<qint64, QString>> sized;
    for (int row = 0; row < pModel->rowCount(); ++row) {
        const QString uid = pModel->entityUid(row);
        const TrackInfo info = trackInfo(uid);
        if (info.format == QLatin1String("flac") && info.bytes > 0 && info.bytes <= 50000000) {
            sized.emplace_back(info.bytes, uid);
        }
    }
    std::sort(sized.begin(), sized.end(), std::greater<>());
    ASSERT_GE(sized.size(), 10u);
    sized.resize(10);

    MainThreadStalls stalls;
    m_pStalls = &stalls;
    Csv csv({QStringLiteral("pass"),
            QStringLiteral("uid"),
            QStringLiteral("title"),
            QStringLiteral("bytes"),
            QStringLiteral("download_ms"),
            QStringLiteral("loaded_ms"),
            QStringLiteral("waveform_ms"),
            QStringLiteral("waveform_at_load"),
            QStringLiteral("first_progress_ms"),
            QStringLiteral("analysis_done_ms")});
    const auto addRow = [&csv](const QString& pass, const LoadTiming& timing) {
        csv.row({pass,
                timing.uid,
                timing.title,
                QString::number(timing.bytes),
                number(timing.downloadMs),
                timing.loaded ? number(timing.loadedMs) : QStringLiteral("failed"),
                number(timing.waveformMs),
                timing.waveformAtLoad ? QStringLiteral("yes") : QStringLiteral("no"),
                number(timing.firstProgressMs),
                number(timing.analysisDoneMs)});
    };
    QHash<QString, std::vector<double>> loaded;
    QHash<QString, std::vector<double>> waveform;
    QHash<QString, std::vector<double>> firstProgress;
    std::vector<double> downloads;
    const auto collect = [&](const QString& pass, const LoadTiming& timing) {
        loaded[pass].push_back(timing.loaded ? timing.loadedMs : -1.0);
        waveform[pass].push_back(timing.waveformMs);
        firstProgress[pass].push_back(timing.firstProgressMs);
        addRow(pass, timing);
    };

    for (const auto& [bytes, uid] : sized) {
        LoadTiming timing = loadRow(pModel, uid, kDeck1, kAnalysisWaitMs);
        timing.bytes = bytes;
        timing.title = trackInfo(uid).title;
        EXPECT_TRUE(timing.loaded) << uid.toStdString();
        downloads.push_back(timing.downloadMs);
        collect(QStringLiteral("uncached"), timing);
    }
    for (const auto& [bytes, uid] : sized) {
        LoadTiming timing = loadRow(pModel, uid, kDeck2, kAgainWaitMs);
        timing.bytes = bytes;
        timing.title = trackInfo(uid).title;
        collect(QStringLiteral("cached"), timing);
    }

    // The same files as local tracks: copies outside the cache
    ASSERT_TRUE(QDir().mkpath(workPath(QStringLiteral("local"))));
    QStringList copies;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto cached = cache()->cachedPath(m_serverId, sized[i].second);
        ASSERT_TRUE(cached.has_value());
        const QString copy = workPath(QStringLiteral("local/%1.flac").arg(i + 1));
        stalls.pause();
        ASSERT_TRUE(QFile::copy(*cached, copy));
        // In the library, as the library scanner adds it on its own thread
        // in the application: not the main thread's time
        ASSERT_NE(nullptr, m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(copy)));
        stalls.resume();
        copies.append(copy);
    }
    for (int i = 0; i < copies.size(); ++i) {
        LoadTiming timing = loadLocal(copies[i], kDeck1, kAnalysisWaitMs);
        timing.uid = QStringLiteral("local-%1").arg(i + 1);
        timing.bytes = sized[static_cast<std::size_t>(i)].first;
        collect(QStringLiteral("local-first"), timing);
    }
    for (int i = 0; i < copies.size(); ++i) {
        LoadTiming timing = loadLocal(copies[i], kDeck2, kAgainWaitMs);
        timing.uid = QStringLiteral("local-%1").arg(i + 1);
        timing.bytes = sized[static_cast<std::size_t>(i)].first;
        collect(QStringLiteral("local-again"), timing);
    }
    stalls.pause();

    QJsonObject passes;
    for (const QString& pass : {QStringLiteral("uncached"),
                 QStringLiteral("cached"),
                 QStringLiteral("local-first"),
                 QStringLiteral("local-again")}) {
        passes.insert(pass,
                QJsonObject{
                        {QStringLiteral("loaded_ms"), stats(loaded.value(pass))},
                        {QStringLiteral("waveform_ms"), stats(waveform.value(pass))},
                        {QStringLiteral("first_progress_ms"), stats(firstProgress.value(pass))},
                });
        std::cout << measureLine(QStringLiteral(
                "load %1: click to loaded p50 %2 ms, max %3 ms; "
                "to waveform p50 %4 ms, max %5 ms")
                        .arg(pass,
                                number(median(loaded.value(pass))),
                                number(maximum(loaded.value(pass))),
                                number(median(waveform.value(pass))),
                                number(maximum(waveform.value(pass)))));
    }
    passes.insert(QStringLiteral("uncached_download_ms"), stats(downloads));
    std::cout << measureLine(QStringLiteral("LoadIntoDeck: ") + stalls.summary());
    ASSERT_TRUE(csv.write(outPath(QStringLiteral("load-into-deck.csv"))));
    const double uncachedWaveform = maximum(waveform.value(QStringLiteral("uncached")));
    record(QStringLiteral("LoadIntoDeck"),
            QJsonObject{
                    {QStringLiteral("playlist"), kFlacs},
                    {QStringLiteral("tracks"), static_cast<int>(sized.size())},
                    {QStringLiteral("bytes_min"), static_cast<double>(sized.back().first)},
                    {QStringLiteral("bytes_max"), static_cast<double>(sized.front().first)},
                    {QStringLiteral("waveform_signal"),
                            QStringLiteral("first Track::waveformUpdated (the analysis made the "
                                           "waveform and fills it from there), or the track "
                                           "had one when it loaded")},
                    {QStringLiteral("passes"), passes},
                    {QStringLiteral("budget_ms"),
                            QJsonObject{{QStringLiteral("uncached_click_to_waveform"),
                                    kUncachedLoadBudgetMs}}},
                    {QStringLiteral("within_budget"),
                            QJsonObject{{QStringLiteral("uncached"),
                                    uncachedWaveform >= 0.0 &&
                                            uncachedWaveform < kUncachedLoadBudgetMs}}},
                    {QStringLiteral("main_thread"), mainThread(stalls)},
            });
}

// 3. F0 Q10 on the LAN: 20 FLACs and 19 MP3s, with an empty cache and then
// with all of them cached: the download, the size and the click to loaded.
TEST_F(CrateG3BudgetsLive, Q10) {
    MainThreadStalls stalls;
    m_pStalls = &stalls;
    Csv csv({QStringLiteral("format"),
            QStringLiteral("pass"),
            QStringLiteral("uid"),
            QStringLiteral("title"),
            QStringLiteral("bytes"),
            QStringLiteral("download_ms"),
            QStringLiteral("download_mb_per_s"),
            QStringLiteral("loaded_ms"),
            QStringLiteral("waveform_ms")});
    QJsonObject formats;
    for (const auto& [name, rows] :
            {std::pair<QString, int>{kFlacs, 20}, std::pair<QString, int>{kMp3s, 19}}) {
        stalls.pause();
        CrateServerTrackModel* pModel = showInTable(name, rows);
        stalls.resume();
        ASSERT_NE(nullptr, pModel) << name.toStdString();
        const auto [format, result] = measureQ10(pModel, name, &csv);
        formats.insert(format, result);
    }
    stalls.pause();
    std::cout << measureLine(QStringLiteral("Q10: ") + stalls.summary());
    ASSERT_TRUE(csv.write(outPath(QStringLiteral("q10.csv"))));
    record(QStringLiteral("Q10"),
            QJsonObject{
                    {QStringLiteral("formats"), formats},
                    {QStringLiteral("main_thread"), mainThread(stalls)},
            });
}

// 4. The network limits (§9 "Red"): "Download for offline use" of the
// 50-track set while the user opens playlists, runs DJ searches and the
// profiles are fetched. Counted by the client (its QNetworkAccessManager)
// and by the measuring proxy, if there is one.
TEST_F(CrateG3BudgetsLive, Concurrency) {
    const qint64 offline = containerId(kOfflineSet);
    ASSERT_GT(offline, 0);
    CrateServerTrackModel* pOffline = activate(m_pFeature, offline);
    ASSERT_NE(nullptr, pOffline);
    ASSERT_TRUE(wait(
            [pOffline] {
                return pOffline->rowCount() == 50;
            },
            kNetworkWaitMs));
    ASSERT_TRUE(waitForApiQuiet(300, kNetworkWaitMs));
    std::optional<CrateServerFeature::OfflineSet> set;
    auto* pSet = &set;
    m_pFeature->readOfflineSet(m_serverId,
            offline,
            [pSet](const CrateServerFeature::OfflineSet& read) {
                *pSet = read;
            });
    ASSERT_TRUE(wait(
            [&set] {
                return set.has_value();
            },
            kNetworkWaitMs));
    ASSERT_EQ(50, set->tracks.size());
    // The 500-track playlist synced, for the profiles of its tracks
    CrateServerTrackModel* p500 = activate(m_pFeature, containerId(kPlaylist500));
    ASSERT_NE(nullptr, p500);
    ASSERT_TRUE(wait(
            [p500] {
                return p500->rowCount() == 500;
            },
            kNetworkWaitMs));
    ASSERT_TRUE(waitForApiQuiet(300, kNetworkWaitMs));
    QStringList withProfile;
    ASSERT_TRUE(wait(
            [&] {
                withProfile = uidsWithProfile(containerId(kPlaylist500));
                return !withProfile.isEmpty();
            },
            kNetworkWaitMs));

    const ProxyMark before = proxyMark();
    m_network.resetPeaks();
    MainThreadStalls stalls;
    m_pStalls = &stalls;
    // A sample of what is in flight every 50 ms
    Csv timeline({QStringLiteral("t_ms"),
            QStringLiteral("api_in_flight"),
            QStringLiteral("stream_in_flight"),
            QStringLiteral("events_in_flight"),
            QStringLiteral("cache_active_downloads"),
            QStringLiteral("cache_queued_downloads"),
            QStringLiteral("cache_ready_files")});
    const qint64 startNs = m_clock.nsecsElapsed();
    QTimer sampler;
    sampler.setInterval(50);
    auto* pTimeline = &timeline;
    QObject::connect(&sampler, &QTimer::timeout, &sampler, [this, pTimeline, startNs] {
        const crate::cache::Usage usage = cache()->usage();
        pTimeline->row({number(msBetween(startNs, m_clock.nsecsElapsed())),
                QString::number(m_network.count(QStringLiteral("api")).inFlight),
                QString::number(m_network.count(QStringLiteral("stream")).inFlight),
                QString::number(m_network.count(QStringLiteral("events")).inFlight),
                QString::number(usage.activeDownloads),
                QString::number(usage.queuedDownloads),
                QString::number(usage.readyFiles)});
    });
    sampler.start();

    // The burst
    setPhase(QStringLiteral("burst: download for offline use"));
    m_pFeature->downloadOfflineSet(offline, *set);
    setPhase(QStringLiteral("burst: DJ searches"));
    std::vector<std::unique_ptr<crate::library::Search>> searches;
    int searchesDone = 0;
    int* pSearchesDone = &searchesDone;
    crate::library::SearchOptions searchOptions;
    searchOptions.debounce = std::chrono::milliseconds(0);
    for (const QString& query : {QStringLiteral("techno"),
                 QStringLiteral("bpm:120-128"),
                 QStringLiteral("key:8A energy:0.6-0.9"),
                 QStringLiteral("analyzed:yes industrial")}) {
        searches.push_back(std::make_unique<crate::library::Search>(m_serverId,
                m_pServers->client(m_serverId),
                dbConnectionPooler(),
                searchOptions));
        crate::library::Search* pSearch = searches.back().get();
        QObject::connect(pSearch,
                &crate::library::Search::resultsReady,
                pSearch,
                [pSearchesDone](qint64 containerId, const QString& query) {
                    Q_UNUSED(containerId);
                    Q_UNUSED(query);
                    ++*pSearchesDone;
                });
        QObject::connect(pSearch,
                &crate::library::Search::searchFailed,
                pSearch,
                [pSearchesDone](const QString& query, const crate::Error& error) {
                    Q_UNUSED(query);
                    Q_UNUSED(error);
                    ++*pSearchesDone;
                });
        pSearch->search(query);
    }
    setPhase(QStringLiteral("burst: profile fetches"));
    // The profiles of the 500-track playlist: the S4 batch, and single
    // requests as without it
    int fetchesDone = 0;
    int* pFetchesDone = &fetchesDone;
    crate::analysis::ProfileStore* pBatchStore = m_pServers->profileStore(m_serverId);
    ASSERT_NE(nullptr, pBatchStore);
    crate::analysis::ProfileStore singleStore(
            m_serverId, m_pServers->client(m_serverId), dbConnectionPooler());
    singleStore.setBatchAvailable(false);
    for (const auto& [pStore, uids] :
            {std::pair<crate::analysis::ProfileStore*, QStringList>{pBatchStore, withProfile},
                    std::pair<crate::analysis::ProfileStore*, QStringList>{
                            &singleStore, withProfile.mid(0, 40)}}) {
        crate::analysis::ProfileFetch* pFetch = pStore->get(uids);
        QObject::connect(pFetch,
                &crate::analysis::ProfileFetch::finished,
                pStore,
                [pFetchesDone](const crate::analysis::ProfileResults& results) {
                    Q_UNUSED(results);
                    ++*pFetchesDone;
                });
    }

    // The other G3 playlists, one after the other, as a user browsing
    int opened = 0;
    for (const QString& name : {kElectronic, kFlacs, kMp3s, kAutoDj, kPlaylist500}) {
        setPhase(QStringLiteral("burst: open %1").arg(name));
        CrateServerTrackModel* pModel = activate(m_pFeature, containerId(name));
        ASSERT_NE(nullptr, pModel) << name.toStdString();
        if (wait(
                    [pModel] {
                        return pModel->rowCount() > 0;
                    },
                    kNetworkWaitMs)) {
            ++opened;
        }
    }
    setPhase(QStringLiteral("burst: until the set is down"));
    // Until the set is down and the rest answered
    QStringList setUids;
    for (const CrateTrackRef& track : std::as_const(set->tracks)) {
        setUids.append(track.entityUid);
    }
    crate::cache::AudioCache* pCache = cache();
    const QString serverId = m_serverId;
    const bool finished = wait(
            [&] {
                if (searchesDone < static_cast<int>(searches.size()) || fetchesDone < 2) {
                    return false;
                }
                for (const QString& uid : std::as_const(setUids)) {
                    const auto state = pCache->trackState(serverId, uid).kind;
                    if (state != crate::cache::TrackState::Kind::Ready &&
                            state != crate::cache::TrackState::Kind::Failed) {
                        return false;
                    }
                }
                return true;
            },
            30 * 60 * 1000);
    const double totalMs = msBetween(startNs, m_clock.nsecsElapsed());
    sampler.stop();
    stalls.pause();
    EXPECT_TRUE(finished);
    int ready = 0;
    for (const QString& uid : std::as_const(setUids)) {
        if (pCache->cachedPath(serverId, uid)) {
            ++ready;
        }
    }
    // The proxy writes a request's row when it ends
    wait(
            [] {
                return false;
            },
            500);
    const ProxyMark after = proxyMark();

    const auto client = [this](const QString& kind) {
        const CountingNetworkAccessManager::Count count = m_network.count(kind);
        return QJsonObject{{QStringLiteral("peak"), count.peak},
                {QStringLiteral("total_since_start"), count.total}};
    };
    const int apiPeak = m_network.count(QStringLiteral("api")).peak;
    const int streamPeak = m_network.count(QStringLiteral("stream")).peak;
    const QJsonObject windowPeaks = proxyWindowPeaks(before);
    std::cout << measureLine(QStringLiteral(
            "concurrency, client: api peak %1 (budget %2), "
            "downloads peak %3 (budget %4); offline set of 50: "
            "%5 ready in %6 s; %7 playlists opened, %8 searches, "
            "%9 profile fetches")
                    .arg(apiPeak)
                    .arg(kApiBudget)
                    .arg(streamPeak)
                    .arg(kDownloadBudget)
                    .arg(ready)
                    .arg(number(totalMs / 1000.0))
                    .arg(opened)
                    .arg(searchesDone)
                    .arg(fetchesDone));
    if (!windowPeaks.isEmpty()) {
        std::cout << measureLine(QStringLiteral("concurrency, proxy window: ") +
                QString::fromUtf8(QJsonDocument(windowPeaks).toJson(QJsonDocument::Compact)));
    }
    std::cout << measureLine(QStringLiteral("Concurrency: ") + stalls.summary());
    ASSERT_TRUE(timeline.write(outPath(QStringLiteral("concurrency-timeline.csv"))));
    record(QStringLiteral("Concurrency"),
            QJsonObject{
                    {QStringLiteral("client"),
                            QJsonObject{{QStringLiteral("api"),
                                                client(QStringLiteral("api"))},
                                    {QStringLiteral("stream"),
                                            client(QStringLiteral("stream"))},
                                    {QStringLiteral("events"),
                                            client(QStringLiteral("events"))}}},
                    {QStringLiteral("proxy_summary_before"), before.summary},
                    {QStringLiteral("proxy_summary_after"), after.summary},
                    {QStringLiteral("proxy_window"), windowPeaks},
                    {QStringLiteral("offline_set"),
                            QJsonObject{
                                    {QStringLiteral("tracks"),
                                            static_cast<int>(setUids.size())},
                                    {QStringLiteral("ready"), ready},
                                    {QStringLiteral("seconds"),
                                            std::round(totalMs / 100.0) /
                                                    10.0}}},
                    {QStringLiteral("burst"),
                            QJsonObject{{QStringLiteral("playlists_opened"),
                                                opened},
                                    {QStringLiteral("searches"), searchesDone},
                                    {QStringLiteral("profile_fetches"),
                                            fetchesDone},
                                    {QStringLiteral("profiles_asked"),
                                            static_cast<int>(
                                                    withProfile.size())}}},
                    {QStringLiteral("budget"),
                            QJsonObject{{QStringLiteral("api"), kApiBudget},
                                    {QStringLiteral("downloads"),
                                            kDownloadBudget}}},
                    {QStringLiteral("within_budget"),
                            QJsonObject{{QStringLiteral("api"),
                                                apiPeak <= kApiBudget},
                                    {QStringLiteral("downloads"),
                                            streamPeak <= kDownloadBudget}}},
                    {QStringLiteral("main_thread"), mainThread(stalls)},
            });
}

// 6. The free space (§9 "Disco"): the offline set into a cache whose volume
// has 2 GiB + 600 MiB free, shrinking with what the cache writes. The free
// space must never go below 2 GiB: the downloads that do not fit fail with
// Storage.
TEST_F(CrateG3BudgetsLive, KeepsTwoGiBFree) {
    const qint64 offline = containerId(kOfflineSet);
    ASSERT_GT(offline, 0);
    CrateServerTrackModel* pOffline = activate(m_pFeature, offline);
    ASSERT_NE(nullptr, pOffline);
    ASSERT_TRUE(wait(
            [pOffline] {
                return pOffline->rowCount() == 50;
            },
            kNetworkWaitMs));
    std::optional<CrateServerFeature::OfflineSet> set;
    auto* pSet = &set;
    m_pFeature->readOfflineSet(m_serverId,
            offline,
            [pSet](const CrateServerFeature::OfflineSet& read) {
                *pSet = read;
            });
    ASSERT_TRUE(wait(
            [&set] {
                return set.has_value();
            },
            kNetworkWaitMs));
    ASSERT_EQ(50, set->tracks.size());

    const QString directory = workPath(QStringLiteral("cache-2gib"));
    ASSERT_TRUE(QDir().mkpath(directory));
    const qint64 initialFree = 2 * kGiB + 600 * kMiB;
    auto pStorage = std::make_shared<ShrinkingStorage>(directory, initialFree);
    crate::cache::AudioCache::Options options;
    options.directory = directory;
    CrateServers* pServers = m_pServers.get();
    options.clientFor = [pServers](const QString& serverId) {
        return pServers->client(serverId);
    };
    options.pStorageInfo = pStorage;
    m_pOwnCache = std::make_unique<crate::cache::AudioCache>(options);
    crate::cache::AudioCache* pCache = m_pOwnCache.get();
    ASSERT_TRUE(wait(
            [pCache] {
                return pCache->isLoaded();
            },
            kNetworkWaitMs));

    MainThreadStalls stalls;
    m_pStalls = &stalls;
    struct Outcome {
        QString uid;
        qint64 bytes = -1;
        QString result;
        QString errorKind;
        QString error;
        qint64 freeAfter = 0;
        double atMs = -1.0;
    };
    auto pOutcomes = std::make_shared<QHash<QString, Outcome>>();
    const qint64 startNs = m_clock.nsecsElapsed();
    const QElapsedTimer* pClock = &m_clock;
    QTimer sampler;
    sampler.setInterval(20);
    QObject::connect(&sampler, &QTimer::timeout, &sampler, [pStorage] {
        pStorage->sample();
    });
    sampler.start();
    setPhase(QStringLiteral("2 GiB: queue the set"));
    const QString reason = QStringLiteral("offline-set:playlist:g3");
    for (const CrateTrackRef& track : std::as_const(set->tracks)) {
        pCache->pin(track.serverId, track.entityUid, reason);
        Outcome outcome;
        outcome.uid = track.entityUid;
        outcome.bytes = track.hints.byteLength.value_or(-1);
        pOutcomes->insert(track.entityUid, outcome);
        crate::cache::CacheTicket* pTicket = pCache->ensure(track.serverId,
                track.entityUid,
                crate::cache::Priority::OfflineSet,
                track.hints);
        const QString uid = track.entityUid;
        QObject::connect(pTicket,
                &crate::cache::CacheTicket::ready,
                pCache,
                [pOutcomes, pStorage, pClock, startNs, uid](const QString& path) {
                    Q_UNUSED(path);
                    Outcome& outcome = (*pOutcomes)[uid];
                    outcome.result = QStringLiteral("ready");
                    outcome.freeAfter = pStorage->sample();
                    outcome.atMs = msBetween(startNs, pClock->nsecsElapsed());
                });
        QObject::connect(pTicket,
                &crate::cache::CacheTicket::failed,
                pCache,
                [pOutcomes, pStorage, pClock, startNs, uid](const crate::Error& error) {
                    Outcome& outcome = (*pOutcomes)[uid];
                    outcome.result = QStringLiteral("failed");
                    outcome.errorKind = error.kind == crate::Error::Kind::Storage
                            ? QStringLiteral("Storage")
                            : QStringLiteral("other");
                    outcome.error = error.message;
                    outcome.freeAfter = pStorage->sample();
                    outcome.atMs = msBetween(startNs, pClock->nsecsElapsed());
                });
    }
    setPhase(QStringLiteral("2 GiB: downloading"));
    const bool finished = wait(
            [pOutcomes] {
                for (const Outcome& outcome : std::as_const(*pOutcomes)) {
                    if (outcome.result.isEmpty()) {
                        return false;
                    }
                }
                return true;
            },
            30 * 60 * 1000);
    sampler.stop();
    stalls.pause();
    pStorage->sample();
    EXPECT_TRUE(finished);

    std::vector<Outcome> outcomes;
    outcomes.reserve(static_cast<std::size_t>(pOutcomes->size()));
    for (auto it = pOutcomes->cbegin(); it != pOutcomes->cend(); ++it) {
        outcomes.push_back(it.value());
    }
    std::sort(outcomes.begin(), outcomes.end(), [](const Outcome& a, const Outcome& b) {
        return a.atMs < b.atMs;
    });
    Csv csv({QStringLiteral("uid"),
            QStringLiteral("bytes"),
            QStringLiteral("result"),
            QStringLiteral("error_kind"),
            QStringLiteral("error"),
            QStringLiteral("free_after_bytes"),
            QStringLiteral("free_after_minus_2gib_mb"),
            QStringLiteral("at_ms")});
    int ready = 0;
    int storage = 0;
    int other = 0;
    QJsonArray storageUids;
    for (const Outcome& outcome : outcomes) {
        if (outcome.result == QLatin1String("ready")) {
            ++ready;
        } else if (outcome.errorKind == QLatin1String("Storage")) {
            ++storage;
            storageUids.append(outcome.uid);
        } else {
            ++other;
        }
        csv.row({outcome.uid,
                QString::number(outcome.bytes),
                outcome.result,
                outcome.errorKind,
                outcome.error,
                QString::number(outcome.freeAfter),
                number(static_cast<double>(outcome.freeAfter - 2 * kGiB) / 1e6),
                number(outcome.atMs)});
    }
    const qint64 lowest = pStorage->lowest();
    std::cout << measureLine(QStringLiteral(
            "2 GiB free: lowest free %1 MB above 2 GiB "
            "(negative: below); %2 ready, %3 failed with "
            "Storage, %4 failed otherwise")
                    .arg(number(static_cast<double>(lowest - 2 * kGiB) / 1e6))
                    .arg(ready)
                    .arg(storage)
                    .arg(other));
    std::cout << measureLine(QStringLiteral("KeepsTwoGiBFree: ") + stalls.summary());
    ASSERT_TRUE(csv.write(outPath(QStringLiteral("keeps-two-gib-free.csv"))));
    record(QStringLiteral("KeepsTwoGiBFree"),
            QJsonObject{
                    {QStringLiteral("initial_free_bytes"), static_cast<double>(initialFree)},
                    {QStringLiteral("lowest_free_bytes"), static_cast<double>(lowest)},
                    {QStringLiteral("lowest_free_minus_2gib_bytes"),
                            static_cast<double>(lowest - 2 * kGiB)},
                    {QStringLiteral("ready"), ready},
                    {QStringLiteral("failed_storage"), storage},
                    {QStringLiteral("failed_other"), other},
                    {QStringLiteral("failed_storage_uids"), storageUids},
                    {QStringLiteral("budget"), QStringLiteral("never below 2 GiB free")},
                    {QStringLiteral("within_budget"), lowest >= 2 * kGiB},
                    {QStringLiteral("main_thread"), mainThread(stalls)},
            });
}

// 7. For F0 Q3 and Q5 at G3: each track of the Electronic playlist with its
// profile's quality and BPM, the verdict of the grid check of
// AnalysisImporter and why, and the BPM of the local analysis after loading
// it, in a CSV to read next to the waveform.
TEST_F(CrateG3BudgetsLive, GridQualityReport) {
    CrateServerTrackModel* pModel = showInTable(kElectronic, 22);
    ASSERT_NE(nullptr, pModel);
    QStringList uids;
    for (int row = 0; row < pModel->rowCount(); ++row) {
        uids.append(pModel->entityUid(row).toLower());
    }
    crate::analysis::ProfileStore* pStore = m_pServers->profileStore(m_serverId);
    ASSERT_NE(nullptr, pStore);
    QSignalSpy fetched(pStore->get(uids), &crate::analysis::ProfileFetch::finished);
    ASSERT_TRUE(fetched.wait(kNetworkWaitMs));
    const auto results = fetched.first().first().value<crate::analysis::ProfileResults>();

    MainThreadStalls stalls;
    m_pStalls = &stalls;
    Csv csv({QStringLiteral("row"),
            QStringLiteral("uid"),
            QStringLiteral("artist"),
            QStringLiteral("title"),
            QStringLiteral("quality"),
            QStringLiteral("analyzer_version"),
            QStringLiteral("profile_bpm"),
            QStringLiteral("tempo_stability"),
            QStringLiteral("beats"),
            QStringLiteral("grid_check"),
            QStringLiteral("reason"),
            QStringLiteral("quantised_bpm"),
            QStringLiteral("max_deviation_ms"),
            QStringLiteral("drift_ms"),
            QStringLiteral("first_beat_ms"),
            QStringLiteral("camelot"),
            QStringLiteral("local_bpm"),
            QStringLiteral("local_minus_profile_bpm"),
            QStringLiteral("local_to_profile_ratio"),
            QStringLiteral("local_key"),
            QStringLiteral("local_analysis_ms")});
    int full = 0;
    int passed = 0;
    int localMatches = 0;
    int localOctaves = 0;
    int localAnalysed = 0;
    QJsonObject reasons;
    QStringList cells;
    for (int i = 0; i < uids.size(); ++i) {
        const QString& uid = uids[i];
        const TrackInfo info = trackInfo(uid);
        const auto it = results.profiles.constFind(uid);
        cells = QStringList{QString::number(i + 1), uid, info.artist, info.title};
        double profileBpm = -1.0;
        if (it == results.profiles.cend()) {
            cells << QStringLiteral("none") << QString() << QString() << QString() << QString()
                  << QString() << QString() << QString() << QString() << QString() << QString()
                  << QString();
        } else {
            const crate::analysis::MixProfile& profile = it->profile;
            const crate::analysis::GridCheck grid = crate::analysis::checkBeatGrid(profile);
            const bool isFull = profile.isFull();
            full += isFull ? 1 : 0;
            passed += isFull && grid.passed ? 1 : 0;
            if (isFull && !grid.passed) {
                const QString kind = reasonKind(grid.reason);
                reasons.insert(kind, reasons.value(kind).toInt() + 1);
            }
            profileBpm = profile.bpm.value_or(-1.0);
            cells << (isFull ? QStringLiteral("full")
                             : (profile.quality == crate::analysis::MixProfile::Quality::Partial
                                               ? QStringLiteral("partial")
                                               : QStringLiteral("other")))
                  << profile.analyzerVersion
                  << (profile.bpm ? QString::number(*profile.bpm, 'f', 5) : QString())
                  << (profile.tempoStability ? QString::number(*profile.tempoStability, 'f', 4)
                                             : QString())
                  << QString::number(profile.beatGridMs.size())
                  << (grid.passed ? QStringLiteral("pass") : QStringLiteral("fail"))
                  << grid.reason << QString::number(grid.bpm, 'f', 1)
                  << QString::number(grid.maxDeviationMs, 'f', 2)
                  << QString::number(grid.driftMs, 'f', 2)
                  << QString::number(grid.firstBeatMs, 'f', 1) << profile.camelot;
        }
        // Mixxx's own analysis, the default policy: load it into a deck
        const LoadTiming timing = loadRow(pModel, uid, kDeck1, kAnalysisWaitMs);
        if (timing.loaded && timing.pTrack && timing.analysisDoneMs >= 0.0) {
            ++localAnalysed;
            const double localBpm = timing.pTrack->getBpm();
            const double delta = profileBpm > 0.0 ? localBpm - profileBpm : 0.0;
            const double ratio = profileBpm > 0.0 ? localBpm / profileBpm : 0.0;
            if (profileBpm > 0.0 && std::abs(delta) < 0.05) {
                ++localMatches;
            } else if (profileBpm > 0.0 &&
                    (std::abs(localBpm * 2.0 - profileBpm) < 0.1 ||
                            std::abs(localBpm - profileBpm * 2.0) < 0.1)) {
                ++localOctaves;
            }
            cells << QString::number(localBpm, 'f', 3)
                  << (profileBpm > 0.0 ? QString::number(delta, 'f', 3) : QString())
                  << (profileBpm > 0.0 ? QString::number(ratio, 'f', 3) : QString())
                  << timing.pTrack->getKeyText() << number(timing.analysisDoneMs);
        } else {
            cells << QString() << QString() << QString() << QString()
                  << QStringLiteral("not analysed");
        }
        csv.row(cells);
    }
    stalls.pause();
    std::cout << measureLine(QStringLiteral(
            "grids of %1 tracks: %2 full, %3 pass the check; "
            "%4 analysed locally, %5 within 0.05 BPM of Crate, %6 at half or double")
                    .arg(uids.size())
                    .arg(full)
                    .arg(passed)
                    .arg(localAnalysed)
                    .arg(localMatches)
                    .arg(localOctaves));
    std::cout << measureLine(QStringLiteral("GridQualityReport: ") + stalls.summary());
    ASSERT_TRUE(csv.write(outPath(QStringLiteral("grid-quality.csv"))));
    record(QStringLiteral("GridQualityReport"),
            QJsonObject{
                    {QStringLiteral("playlist"), kElectronic},
                    {QStringLiteral("tracks"), static_cast<int>(uids.size())},
                    {QStringLiteral("full"), full},
                    {QStringLiteral("passed"), passed},
                    {QStringLiteral("failed_by_reason"), reasons},
                    {QStringLiteral("analysed_locally"), localAnalysed},
                    {QStringLiteral("local_within_0_05_bpm"), localMatches},
                    {QStringLiteral("local_at_half_or_double"), localOctaves},
                    {QStringLiteral("limits"),
                            QJsonObject{{QStringLiteral("min_beats"),
                                                crate::analysis::kGridMinBeats},
                                    {QStringLiteral("max_deviation_ms"),
                                            crate::analysis::
                                                    kGridMaxDeviationMs},
                                    {QStringLiteral("max_drift_ms"),
                                            crate::analysis::kGridMaxDriftMs}}},
                    {QStringLiteral("main_thread"), mainThread(stalls)},
            });
}

} // namespace
