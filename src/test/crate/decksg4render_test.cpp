// Gates G4 and G4b of the Crate Decks plan: Auto DJ's Crate Smart Mix
// transitions rendered offline as Decks plays them, for the automatic metrics
// of the report (docs/plans/2026-11-19-decks-g4-g4b-report.md).
//
// Everything that sounds is the app's own code: the real engine (EngineMixer
// with two Decks, keylock, the default mixing EQ in the decks' racks, the
// crossfader with the app's default curve and ReplayGain as the app sets it),
// the real AutoDJProcessor in TransitionMode::CrateSmartMix, the real
// AutoDJSmartMixAdapter with a real PlanClient signed in to a Crate dev stack,
// and the real SmartMixDirector. The tracks get what the app gives a Crate
// track before it plays: AnalysisImporter with the app's default policy, then
// upstream's beat, ReplayGain and silence analyzers. Only the clock is
// simulated. The engine runs on its own thread, one callback of the app's
// default buffer (1024 frames, about 23 ms) at a time, as fast as the caching
// reader allows, so Auto DJ hears the decks through queued signals as in the
// app. Between two callbacks the main thread runs the director's 16 ms timer
// (and its start timer, at a 1 ms resolution) on the simulated clock: like the
// app's, its ticks see the engine position of the last callback, and what they
// set reaches the engine at the next one.
//
// Each pair and variant runs twice with the same inputs: once recording the
// main output, once with the decks' EQs off recording each deck's channel
// with its pregain divided out (each deck alone at unity gain on the same
// clock, the stems of listen_metrics.py). Per run it writes mix.wav or
// outgoing.wav and incoming.wav (32-bit float), trace.csv (one row per
// buffer) and report.json, and deviation.csv (SmartMixDirector::Report).
//
// Skipped unless CRATE_DECKS_G4_PAIRS is set, so CI never runs it:
//   CRATE_DECKS_G4_PAIRS        pairs.json of select_pairs.py (monorepo
//                               tools/smart-mix-eval/decks_g4)
//   CRATE_DECKS_G4_OUT          output directory
//   CRATE_DECKS_G4_VARIANTS     optional: v3,v2,v3-norg (default all)
//   CRATE_DECKS_G4_ONLY         optional: pair ids, comma separated
//   CRATE_DECKS_LIVE_URL, _EMAIL, _PASSWORD   the dev stack, as the live
//                               suites (loopback or private address only)
// LimitCheckAgreement also needs CRATE_DECKS_G4_LIMIT_CASES (limit_cases.py).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMetaEnum>
#include <QModelIndex>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QtEndian>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "analyzer/analyzer.h"
#include "analyzer/analyzerbeats.h"
#include "analyzer/analyzerebur128.h"
#include "analyzer/analyzersilence.h"
#include "analyzer/analyzertrack.h"
#include "analyzer/constants.h"
#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "crate/analysis/analysisimporter.h"
#include "crate/analysis/mixprofile.h"
#include "crate/client/cratecapabilities.h"
#include "crate/client/crateclient.h"
#include "crate/crateaddserverflow.h"
#include "crate/crateservers.h"
#include "crate/session/cratesecretstore.h"
#include "crate/session/cratesession.h"
#include "crate/smartmix/autodjsmartmixadapter.h"
#include "crate/smartmix/eqmap.h"
#include "crate/smartmix/planclient.h"
#include "crate/smartmix/smartmixdirector.h"
#include "crate/smartmix/transitionplan.h"
#include "effects/effectsmanager.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"
#include "engine/enginexfader.h"
#include "library/autodj/autodjprocessor.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/playlisttablemodel.h"
#include "mixer/deck.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "sources/audiosourcestereoproxy.h"
#include "sources/soundsourceproxy.h"
#include "test/librarytest.h"
#include "test/signalpathtest.h"
#include "track/track.h"
#include "util/samplebuffer.h"
#include "waveform/visualplayposition.h"
#ifdef __RUBBERBAND__
#include "engine/bufferscalers/rubberbandworkerpool.h"
#endif

namespace crate_decks_g4 {

const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");
const QString kMainGroup = QStringLiteral("[Master]");
constexpr int kSampleRate = 44100;
// SoundManagerConfig's default buffer at 44.1 kHz (Size20xms)
constexpr SINT kBufferFrames = 1024;
constexpr double kBufferMs = 1000.0 * kBufferFrames / kSampleRate;
constexpr double kTickMs = crate::SmartMixDirector::kTickMs;
// The outgoing deck starts where pairs.json says (renderStartMs): 25 s before
// the earliest of Auto DJ's estimated handover, the plans' start and its
// Outro, so Auto DJ computes its fade as in a set (select_pairs.py).
// Recorded around the transition
constexpr double kKeepBeforeMs = 20000.0;
constexpr double kKeepAfterMs = 16000.0;
constexpr int kNetworkWaitMs = 60000;
// Real time given to the caching reader per buffer
constexpr unsigned long kPacingMicroseconds = 1000;
// The director's start timer, emulated at this resolution while it prepares
constexpr double kStartTimerStepMs = 1.0;
// AutoDJSmartMixAdapter's server id here
const QString kServerId = QStringLiteral("g4");

/// Sets a control as a widget or a controller would: through a ControlProxy,
/// so the control's owner hears of it (a ControlObject does not signal its own
/// changes, and an effect slot only updates the engine on that signal).
void setControl(const ConfigKey& key, double value) {
    ControlProxy proxy(key);
    proxy.set(value);
}

void measure(const std::string& line) {
    std::cout << "[ MEASURE  ] " << line << std::endl;
}

bool isDevStack(const QUrl& url) {
    const QString host = url.host();
    if (host == QLatin1String("localhost")) {
        return true;
    }
    const QHostAddress address(host);
    if (address.isNull()) {
        return false;
    }
    if (address.isLoopback() || address.isLinkLocal()) {
        return true;
    }
    for (const char* subnet : {"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "fc00::/7"}) {
        if (address.isInSubnet(QHostAddress::parseSubnet(QString::fromLatin1(subnet)))) {
            return true;
        }
    }
    return false;
}

/// A 32-bit float stereo WAV.
bool writeFloatWav(const QString& path, const std::vector<float>& interleaved) {
    QByteArray header;
    const auto put32 = [&header](quint32 value) {
        char bytes[4];
        qToLittleEndian(value, bytes);
        header.append(bytes, 4);
    };
    const auto put16 = [&header](quint16 value) {
        char bytes[2];
        qToLittleEndian(value, bytes);
        header.append(bytes, 2);
    };
    const auto dataBytes = static_cast<quint32>(interleaved.size() * sizeof(float));
    header.append("RIFF");
    put32(36 + dataBytes);
    header.append("WAVEfmt ");
    put32(16);
    put16(3); // IEEE float
    put16(2);
    put32(kSampleRate);
    put32(kSampleRate * 8);
    put16(8);
    put16(32);
    header.append("data");
    put32(dataBytes);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(header);
    QByteArray data(static_cast<qsizetype>(dataBytes), Qt::Uninitialized);
    for (std::size_t i = 0; i < interleaved.size(); ++i) {
        qToLittleEndian(interleaved[i], data.data() + i * sizeof(float));
    }
    file.write(data);
    return true;
}

/// The director's clock is the engine's: the time of the buffers processed.
class EngineClockEnvironment : public crate::SmartMixEnvironment {
  public:
    explicit EngineClockEnvironment(const double* pNowMs)
            : m_pNowMs(pNowMs) {
    }

    double nowMs() const override {
        return *m_pNowMs;
    }

    double enginePlayPosition(const QString& group) const override {
        return VisualPlayPosition::getVisualPlayPosition(group)->getEnginePlayPos();
    }

    QString trackKey(const QString& group) const override {
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
        if (!pTrack) {
            return {};
        }
        return QStringLiteral("%1:%2").arg(
                QString::number(reinterpret_cast<quintptr>(pTrack.get()), 16),
                pTrack->getId().toString());
    }

  private:
    const double* const m_pNowMs;
};

class TwoDecks : public PlayerManagerInterface {
  public:
    TwoDecks(Deck* pDeck1, Deck* pDeck2)
            : m_pDeck1(pDeck1),
              m_pDeck2(pDeck2) {
    }
    BaseTrackPlayer* getPlayer(const QString& group) const override {
        if (group == kDeck1) {
            return m_pDeck1;
        }
        return group == kDeck2 ? m_pDeck2 : nullptr;
    }
    BaseTrackPlayer* getPlayer(const ChannelHandle&) const override {
        return nullptr;
    }
    BaseTrackPlayer* getDeckBase(int deckIndex) const override {
        if (deckIndex == 0) {
            return m_pDeck1;
        }
        return deckIndex == 1 ? m_pDeck2 : nullptr;
    }
    int numberOfDecks() const override {
        return 2;
    }
    PreviewDeck* getPreviewDeck(int) const override {
        return nullptr;
    }
    int numberOfPreviewDecks() const override {
        return 0;
    }
    Sampler* getSampler(int) const override {
        return nullptr;
    }
    int numberOfSamplers() const override {
        return 0;
    }

  private:
    Deck* m_pDeck1;
    Deck* m_pDeck2;
};

/// Auto DJ whose loads go to the decks of the engine.
class EngineAutoDJ : public AutoDJProcessor {
  public:
    EngineAutoDJ(UserSettingsPointer pConfig,
            PlayerManagerInterface* pPlayers,
            TrackCollectionManager* pTracks,
            int playlistId)
            : AutoDJProcessor(nullptr, pConfig, pPlayers, pTracks, playlistId),
              m_pPlayers(pPlayers) {
    }

  protected:
    void emitLoadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play) override {
        if (BaseTrackPlayer* pPlayer = m_pPlayers->getPlayer(group)) {
            pPlayer->slotLoadTrack(pTrack,
#ifdef __STEM__
                    mixxx::StemChannelSelection(),
#endif
                    play);
        }
    }

  private:
    PlayerManagerInterface* m_pPlayers;
};

struct Variant {
    QString name;
    QString plannerVersion;
    bool replayGain = true;
};

struct Pair {
    QString id;
    QString outgoingUid;
    QString incomingUid;
    QString outgoingPath;
    QString incomingPath;
    /// Where the outgoing deck starts playing, in its own time.
    double startMs = 0.0;
};

/// The pregain EnginePregain applied to a deck's last buffer, to divide it
/// out of the deck's channel (stems).
struct DeckGain {
    double previous = 1.0;
};

/// One buffer of a run.
struct TraceRow {
    double tMs = 0.0;
    double positionMs[2] = {0.0, 0.0};
    double play[2] = {0.0, 0.0};
    double rate[2] = {1.0, 1.0};
    double pregain[2] = {1.0, 1.0};
    double totalGain[2] = {1.0, 1.0};
    double appliedGain[2] = {1.0, 1.0};
    double lowCutDb[2] = {0.0, 0.0};
    double crossfader = -1.0;
    double crossfaderGain[2] = {1.0, 0.0};
    double keylock[2] = {0.0, 0.0};
    int directorState = 0;
    int autoDjState = 0;
};

struct StateEvent {
    QString state;
    double tMs = 0.0;
    double outgoingMs = 0.0;
    double incomingMs = 0.0;
};

QJsonObject planJson(const crate::TransitionPlan& plan) {
    QJsonObject json{
            {QStringLiteral("plannerVersion"), crate::TransitionPlan::kPlannerVersion},
            {QStringLiteral("outgoingTrackEntityUid"), plan.outgoingTrackEntityUid},
            {QStringLiteral("incomingTrackEntityUid"), plan.incomingTrackEntityUid},
            {QStringLiteral("mode"), crate::toString(plan.mode)},
            {QStringLiteral("durationMs"), plan.durationMs},
            {QStringLiteral("outgoingCueMs"), plan.outgoingCueMs},
            {QStringLiteral("incomingCueMs"), plan.incomingCueMs},
            {QStringLiteral("incomingTempoRatio"), plan.incomingTempoRatio},
            {QStringLiteral("beatPhaseOffsetMs"), plan.beatPhaseOffsetMs},
            {QStringLiteral("handoffProgress"), plan.handoffProgress},
            {QStringLiteral("outgoingGainDb"), plan.outgoingGainDb},
            {QStringLiteral("incomingGainDb"), plan.incomingGainDb},
            {QStringLiteral("curve"),
                    plan.curve == crate::TransitionPlan::Curve::DjBlend
                            ? QStringLiteral("dj-blend")
                            : QStringLiteral("equal-power")},
            {QStringLiteral("bassHandoff"),
                    plan.hasBassHandoff() ? QStringLiteral("balanced") : QStringLiteral("none")},
            {QStringLiteral("confidence"), plan.confidence},
            {QStringLiteral("startIncomingCueMs"), plan.startIncomingCueMs()},
            {QStringLiteral("candidateIndex"), plan.candidateIndex},
            {QStringLiteral("candidates"), static_cast<int>(plan.candidates.size())},
    };
    json.insert(QStringLiteral("fallbackReason"),
            plan.fallbackReason ? QJsonValue(crate::toString(*plan.fallbackReason))
                                : QJsonValue());
    return json;
}

class CrateDecksG4Render : public LibraryTest {
  protected:
    void SetUp() override {
        const QString pairsPath = qEnvironmentVariable("CRATE_DECKS_G4_PAIRS");
        if (pairsPath.isEmpty()) {
            GTEST_SKIP() << "Decks G4 renders: set CRATE_DECKS_G4_PAIRS, CRATE_DECKS_G4_OUT "
                            "and the CRATE_DECKS_LIVE_* variables of a dev stack";
        }
        QFile pairs(pairsPath);
        ASSERT_TRUE(pairs.open(QIODevice::ReadOnly)) << pairsPath.toStdString();
        m_pairs = QJsonDocument::fromJson(pairs.readAll()).object();
        m_outDir = qEnvironmentVariable("CRATE_DECKS_G4_OUT");
        ASSERT_FALSE(m_outDir.isEmpty()) << "Set CRATE_DECKS_G4_OUT";
        ASSERT_TRUE(QDir().mkpath(m_outDir));
        signIn();
    }

    void TearDown() override {
        m_pServers.reset();
    }

    void signIn() {
        const QString url = qEnvironmentVariable("CRATE_DECKS_LIVE_URL");
        ASSERT_FALSE(url.isEmpty()) << "Set CRATE_DECKS_LIVE_URL";
        ASSERT_TRUE(isDevStack(QUrl(url))) << "Only a dev stack: " << url.toStdString();
        crate::CrateServers::Dependencies dependencies;
        dependencies.pConfig = config();
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.pSecretStore = &m_secrets;
        dependencies.audioCacheDirectory = m_cacheDir.path();
        m_pServers = std::make_unique<crate::CrateServers>(dependencies);
        m_pServers->start();
        using Step = crate::AddServerFlow::Step;
        crate::AddServerFlow flow(m_pServers.get());
        flow.setAddress(url);
        if (flow.step() == Step::ConfirmLocalNetwork) {
            flow.confirmLocalNetwork();
        }
        ASSERT_TRUE(QTest::qWaitFor(
                [&flow] {
                    return flow.step() == Step::SignIn || flow.step() == Step::EnterAddress;
                },
                kNetworkWaitMs));
        ASSERT_EQ(Step::SignIn, flow.step()) << flow.error().toStdString();
        flow.loginWithPassword(qEnvironmentVariable("CRATE_DECKS_LIVE_EMAIL"),
                qEnvironmentVariable("CRATE_DECKS_LIVE_PASSWORD"));
        ASSERT_TRUE(QTest::qWaitFor(
                [&flow] {
                    return flow.step() != Step::SigningIn;
                },
                kNetworkWaitMs));
        ASSERT_EQ(Step::Done, flow.step()) << flow.error().toStdString();
        m_serverId = m_pServers->sessions().constFirst()->profile().id;
        m_pClient = m_pServers->client(m_serverId);
        ASSERT_NE(nullptr, m_pClient);
        m_capabilities = m_pServers->lastCapabilities(m_serverId);
    }

    std::optional<QJsonObject> get(const QString& path) {
        crate::Reply* pReply = m_pClient->get(path);
        auto pAnswer = std::make_shared<std::optional<crate::JsonResult>>();
        QObject::connect(pReply,
                &crate::Reply::finished,
                [pAnswer](const crate::JsonResult& result) {
                    *pAnswer = result;
                });
        EXPECT_TRUE(QTest::qWaitFor(
                [pAnswer] {
                    return pAnswer->has_value();
                },
                kNetworkWaitMs));
        if (!pAnswer->has_value() || !(*pAnswer)->ok()) {
            return std::nullopt;
        }
        return (*pAnswer)->value().object();
    }

    /// The capabilities Decks keeps; with `v2Only`, those of a server that
    /// offers no `smart-mix-v3` (PlanContext::forServer() then asks v2).
    std::optional<crate::Capabilities> capabilities(bool v2Only) const {
        QJsonObject root = QJsonDocument::fromJson(m_capabilities).object();
        if (v2Only) {
            QJsonObject smartMix = root.value(QStringLiteral("smart_mix")).toObject();
            smartMix.insert(QStringLiteral("planner_versions"),
                    QJsonArray{QStringLiteral("smart-mix-v2")});
            smartMix.insert(QStringLiteral("planner_version"), QStringLiteral("smart-mix-v2"));
            root.insert(QStringLiteral("smart_mix"), smartMix);
        }
        const auto parsed = crate::Capabilities::fromJson(QJsonDocument(root));
        if (!parsed.ok()) {
            return std::nullopt;
        }
        return parsed.value();
    }

    QJsonObject m_pairs;
    QString m_outDir;
    QTemporaryDir m_cacheDir;
    QNetworkAccessManager m_network;
    crate::SessionOnlySecretStore m_secrets;
    std::unique_ptr<crate::CrateServers> m_pServers;
    QString m_serverId;
    crate::Client* m_pClient = nullptr;
    QByteArray m_capabilities;
};

/// Runs the engine's callbacks on a thread of their own, as the app's sound
/// API does: signals the engine emits reach main-thread objects queued.
class EngineThread {
  public:
    explicit EngineThread(std::function<void()> work)
            : m_work(std::move(work)),
              m_thread([this] {
                  loop();
              }) {
    }
    ~EngineThread() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }
    /// One callback; returns when it is done.
    void run() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_pending = true;
        m_wake.notify_all();
        m_wake.wait(lock, [this] {
            return !m_pending;
        });
    }

  private:
    void loop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            m_wake.wait(lock, [this] {
                return m_pending || m_quit;
            });
            if (m_quit) {
                return;
            }
            lock.unlock();
            m_work();
            lock.lock();
            m_pending = false;
            m_wake.notify_all();
        }
    }

    std::function<void()> m_work;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_pending = false;
    bool m_quit = false;
    std::thread m_thread;
};

/// The engine, as BaseSignalPathTest builds it, with decks 1 and 2 on the
/// crossfader's left and right as Auto DJ needs them and the default mixing EQ
/// in their racks.
class Engine {
  public:
    explicit Engine(UserSettingsPointer pConfig)
            : m_pConfig(pConfig),
              m_pIndicatorTimer(std::make_unique<mixxx::ControlIndicatorTimer>()),
              m_pChannelHandleFactory(std::make_shared<ChannelHandleFactory>()),
              m_numDecks(ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_decks"))),
              m_numSamplers(ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_samplers"))),
              m_numPreviewDecks(
                      ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_preview_decks"))) {
#ifdef __RUBBERBAND__
        RubberBandWorkerPool::createInstance();
#endif
        m_pEffects = std::make_unique<EffectsManager>(pConfig, m_pChannelHandleFactory);
        m_pMixer = std::make_unique<TestEngineMixer>(
                pConfig, kMainGroup, m_pEffects.get(), m_pChannelHandleFactory, false);
        ControlObject::set(ConfigKey(kMainGroup, QStringLiteral("samplerate")), kSampleRate);
        m_pDeck1 = std::make_unique<Deck>(nullptr,
                pConfig,
                m_pMixer.get(),
                m_pEffects.get(),
                EngineChannel::LEFT,
                m_pMixer->registerChannelGroup(kDeck1));
        m_pDeck2 = std::make_unique<Deck>(nullptr,
                pConfig,
                m_pMixer.get(),
                m_pEffects.get(),
                EngineChannel::RIGHT,
                m_pMixer->registerChannelGroup(kDeck2));
        for (Deck* pDeck : {m_pDeck1.get(), m_pDeck2.get()}) {
            const QString group = pDeck->getGroup();
            ControlObject::set(ConfigKey(group, QStringLiteral("main_mix")), 1.0);
            // DlgPrefDeck's default range, 8 %
            ControlObject::set(ConfigKey(group, QStringLiteral("rate_dir")), 1.0);
            ControlObject::set(ConfigKey(group, QStringLiteral("rateRange")), 0.08);
            m_numDecks.set(m_numDecks.get() + 1);
            m_pEffects->addDeck(ChannelHandleAndGroup(
                    pDeck->getEngineDeck()->getHandle(), pDeck->getGroup()));
        }
        // The app's crossfader (DlgPrefMixer's defaults) and EQ crossovers
        ControlObject::set(ConfigKey(QString(EngineXfader::kXfaderConfigKey),
                                   QStringLiteral("xFaderMode")),
                MIXXX_XFADER_ADDITIVE);
        ControlObject::set(ConfigKey(QString(EngineXfader::kXfaderConfigKey),
                                   QStringLiteral("xFaderCurve")),
                EngineXfader::kTransformDefault);
        ControlObject::set(ConfigKey(QString(EngineXfader::kXfaderConfigKey),
                                   QStringLiteral("xFaderCalibration")),
                EngineXfader::getPowerCalibration(EngineXfader::kTransformDefault));
        ControlObject::set(ConfigKey(QString(EngineXfader::kXfaderConfigKey),
                                   QStringLiteral("LoEQFrequency")),
                246.0);
        ControlObject::set(ConfigKey(QString(EngineXfader::kXfaderConfigKey),
                                   QStringLiteral("HiEQFrequency")),
                2484.0);
        m_pEffects->setup();
        ControlObject::set(ConfigKey(kMainGroup, QStringLiteral("enabled")), 1.0);
        PlayerInfo::create();
        m_pPlayers = std::make_unique<TwoDecks>(m_pDeck1.get(), m_pDeck2.get());
        m_pThread = std::make_unique<EngineThread>([this] {
            m_pMixer->process(kBufferFrames * 2);
        });
    }

    ~Engine() {
        m_pThread.reset();
        m_pPlayers.reset();
        m_pDeck2.reset();
        m_pDeck1.reset();
        m_pMixer.reset();
        m_pEffects.reset();
        PlayerInfo::destroy();
#ifdef __RUBBERBAND__
        RubberBandWorkerPool::destroy();
#endif
    }

    Deck* deck(int index) const {
        return index == 0 ? m_pDeck1.get() : m_pDeck2.get();
    }

    /// One callback, on the engine's thread; keeps what EnginePregain applied
    /// to each deck.
    void process() {
        m_pThread->run();
        for (int index = 0; index < 2; ++index) {
            EngineDeck* pChannel = deck(index)->getEngineDeck();
            m_lastGain[index].previous = m_lastGain[index].current;
            m_lastPostGain[index].previous = m_lastPostGain[index].current;
            if (pChannel->getEngineBuffer()->isTrackLoaded()) {
                m_lastGain[index].current = appliedGain(index);
                m_lastPostGain[index].current = postFaderGain(index);
            }
        }
    }

    /// The gain the main mix applies to the deck's channel buffer, in place
    /// (EngineMixer's OrientationVolumeGainCalculator): its volume fader times
    /// the crossfader's gain for its side.
    double postFaderGain(int index) const {
        const QString xfader = QString(EngineXfader::kXfaderConfigKey);
        CSAMPLE_GAIN left = 1.0f;
        CSAMPLE_GAIN right = 1.0f;
        EngineXfader::getXfadeGains(
                ControlObject::get(ConfigKey(kMainGroup, QStringLiteral("crossfader"))),
                ControlObject::get(ConfigKey(xfader, QStringLiteral("xFaderCurve"))),
                ControlObject::get(ConfigKey(xfader, QStringLiteral("xFaderCalibration"))),
                ControlObject::get(ConfigKey(xfader, QStringLiteral("xFaderMode"))),
                ControlObject::get(ConfigKey(xfader, QStringLiteral("xFaderReverse"))) > 0.0,
                &left,
                &right);
        const double volume = ControlObject::get(
                ConfigKey(deck(index)->getGroup(), QStringLiteral("volume")));
        // Deck 1 is on the left, deck 2 on the right
        return volume * (index == 0 ? left : right);
    }

    /// The gain EnginePregain applied to the deck's last buffer: its
    /// total_gain (pregain and ReplayGain) with the vinyl speed gain.
    double appliedGain(int index) const {
        const QString group = deck(index)->getGroup();
        const double total = ControlObject::get(ConfigKey(group, QStringLiteral("total_gain")));
        const double speed = std::abs(deck(index)->getEngineDeck()->getEngineBuffer()->getSpeed());
        // EnginePregain's kSpeedGainMultiplier, kSpeedOneDiv and kMaxTotalGainBySpeed
        constexpr double kMultiplier = 4.0;
        const double oneDiv = std::log10(kMultiplier + 1.0);
        double speedGain = std::log10(speed * kMultiplier + 1.0) / oneDiv;
        if (total > 0.9) {
            speedGain = std::min(1.0, speedGain);
        } else if (total > 0.0) {
            speedGain = std::min(0.9 / total, speedGain);
        }
        return total * speedGain;
    }

    /// The deck's channel with its pregain and its post-fader gain divided
    /// out, appended to `out`: both ramp over the buffer from their last value
    /// (SampleUtil::applyRampingGain).
    void appendStem(int index, std::vector<float>* pOut) const {
        const auto buffer = m_pMixer->getChannelBuffer(deck(index)->getGroup());
        const auto ramp = [](const LastGain& gain, SINT frame) {
            if (gain.previous == gain.current) {
                return gain.current;
            }
            return gain.previous +
                    (gain.current - gain.previous) * static_cast<double>(frame + 1) /
                    static_cast<double>(kBufferFrames);
        };
        for (SINT frame = 0; frame < kBufferFrames; ++frame) {
            const double gain = ramp(m_lastGain[index], frame) * ramp(m_lastPostGain[index], frame);
            const double scale = std::abs(gain) > 1e-4 ? 1.0 / gain : 0.0;
            pOut->push_back(static_cast<float>(buffer[2 * frame] * scale));
            pOut->push_back(static_cast<float>(buffer[2 * frame + 1] * scale));
        }
    }

    void appendMain(std::vector<float>* pOut) const {
        const auto buffer = m_pMixer->getMainBuffer();
        pOut->insert(pOut->end(), buffer.begin(), buffer.begin() + kBufferFrames * 2);
    }

    /// Ejects both decks and puts every control the runs touch back.
    void reset(bool eqOn, bool replayGain) {
        for (int index = 0; index < 2; ++index) {
            const QString group = deck(index)->getGroup();
            setControl(ConfigKey(group, QStringLiteral("play")), 0.0);
            deck(index)->slotEjectTrack(1.0);
            setControl(ConfigKey(group, QStringLiteral("pregain")), 1.0);
            setControl(ConfigKey(group, QStringLiteral("volume")), 1.0);
            setControl(ConfigKey(group, QStringLiteral("rate")), 0.0);
            setControl(ConfigKey(group, QStringLiteral("rate_ratio")), 1.0);
            setControl(ConfigKey(group, QStringLiteral("keylock")), 0.0);
            setControl(ConfigKey(group, QStringLiteral("quantize")), 0.0);
            setControl(ConfigKey(group, QStringLiteral("sync_enabled")), 0.0);
            const QString eq = crate::eqmap::effectGroup(group);
            for (const QString& item : {crate::eqmap::kLowGain,
                         crate::eqmap::kMidGain,
                         crate::eqmap::kHighGain}) {
                setControl(ConfigKey(eq, item), crate::eqmap::kNeutralGain);
            }
            setControl(ConfigKey(eq, QStringLiteral("enabled")), eqOn ? 1.0 : 0.0);
        }
        setControl(ConfigKey(kMainGroup, QStringLiteral("crossfader")), -1.0);
        // DlgPrefReplayGain's defaults: on, 0 dB boost, -6 dB without data
        setControl(ConfigKey(QStringLiteral("[ReplayGain]"),
                           QStringLiteral("ReplayGainEnabled")),
                replayGain ? 1.0 : 0.0);
        setControl(ConfigKey(QStringLiteral("[ReplayGain]"),
                           QStringLiteral("ReplayGainBoost")),
                1.0);
        setControl(
                ConfigKey(QStringLiteral("[ReplayGain]"), QStringLiteral("DefaultBoost")),
                std::pow(10.0, -6.0 / 20.0));
        for (int i = 0; i < 4; ++i) {
            process();
        }
    }

    /// Loads `pTrack` into the deck, stopped, processing buffers until it is.
    bool load(int index, const TrackPointer& pTrack) {
        deck(index)->slotLoadTrack(pTrack,
#ifdef __STEM__
                mixxx::StemChannelSelection(),
#endif
                false);
        return waitLoaded(index);
    }

    bool waitLoaded(int index) {
        for (int i = 0; i < 4000; ++i) {
            process();
            QCoreApplication::processEvents();
            if (deck(index)->getEngineDeck()->getEngineBuffer()->isTrackLoaded()) {
                return true;
            }
            QThread::usleep(1000);
        }
        return false;
    }

    PlayerManagerInterface* players() const {
        return m_pPlayers.get();
    }
    EffectsManager& effects() const {
        return *m_pEffects;
    }

  private:
    struct LastGain {
        double previous = 1.0;
        double current = 1.0;
    };

    UserSettingsPointer m_pConfig;
    std::unique_ptr<mixxx::ControlIndicatorTimer> m_pIndicatorTimer;
    ChannelHandleFactoryPointer m_pChannelHandleFactory;
    ControlObject m_numDecks;
    ControlObject m_numSamplers;
    ControlObject m_numPreviewDecks;
    std::unique_ptr<EffectsManager> m_pEffects;
    std::unique_ptr<TestEngineMixer> m_pMixer;
    std::unique_ptr<Deck> m_pDeck1;
    std::unique_ptr<Deck> m_pDeck2;
    std::unique_ptr<TwoDecks> m_pPlayers;
    std::unique_ptr<EngineThread> m_pThread;
    LastGain m_lastGain[2];
    LastGain m_lastPostGain[2];
};

/// What upstream's analysis gives the track before it plays, after
/// AnalysisImporter: beats, ReplayGain 2.0 and the first and last sound.
void analyze(UserSettingsPointer pConfig, const TrackPointer& pTrack) {
    mixxx::AudioSource::OpenParams openParams;
    openParams.setChannelCount(mixxx::kAnalysisMaxChannels);
    mixxx::AudioSourcePointer pSource = SoundSourceProxy(pTrack).openAudioSource(openParams);
    ASSERT_TRUE(pSource) << pTrack->getLocation().toStdString();
    if (pSource->getSignalInfo().getChannelCount() % mixxx::kAnalysisChannels) {
        pSource = std::make_shared<mixxx::AudioSourceStereoProxy>(
                pSource, mixxx::kAnalysisFramesPerChunk);
    }
    AnalyzerBeats beats(pConfig, true);
    AnalyzerEbur128 gain(pConfig);
    AnalyzerSilence silence(pConfig);
    std::vector<Analyzer*> analyzers;
    for (Analyzer* pAnalyzer : std::initializer_list<Analyzer*>{&beats, &gain, &silence}) {
        if (pAnalyzer->initialize(AnalyzerTrack(pTrack),
                    pSource->getSignalInfo().getSampleRate(),
                    pSource->getSignalInfo().getChannelCount(),
                    pSource->frameLength())) {
            analyzers.push_back(pAnalyzer);
        }
    }
    mixxx::SampleBuffer buffer(mixxx::kAnalysisSamplesPerChunk);
    mixxx::IndexRange remaining = pSource->frameIndexRange();
    while (!remaining.empty()) {
        auto chunk = remaining.splitAndShrinkFront(
                std::min(mixxx::kAnalysisFramesPerChunk, remaining.length()));
        const auto read = pSource->readSampleFrames(mixxx::WritableSampleFrames(
                chunk, mixxx::SampleBuffer::WritableSlice(buffer)));
        remaining = intersect(remaining, pSource->frameIndexRange());
        if (!read.frameIndexRange().empty()) {
            for (Analyzer* pAnalyzer : analyzers) {
                pAnalyzer->processSamples(read.readableData(), read.readableLength());
            }
        }
    }
    for (Analyzer* pAnalyzer : analyzers) {
        pAnalyzer->storeResults(pTrack);
        pAnalyzer->cleanup();
    }
}

TEST_F(CrateDecksG4Render, RendersAutoDJTransitions) {
    const QStringList only = qEnvironmentVariable("CRATE_DECKS_G4_ONLY")
                                     .split(QLatin1Char(','), Qt::SkipEmptyParts);
    const QStringList variantNames =
            qEnvironmentVariable("CRATE_DECKS_G4_VARIANTS", QStringLiteral("v3,v2,v3-norg"))
                    .split(QLatin1Char(','), Qt::SkipEmptyParts);
    QList<Variant> variants;
    for (const QString& name : variantNames) {
        variants.append(Variant{name,
                name.startsWith(QLatin1String("v2")) ? QStringLiteral("smart-mix-v2")
                                                     : QStringLiteral("smart-mix-v3"),
                !name.endsWith(QLatin1String("-norg"))});
    }

    // Auto DJ's queue
    PlaylistDAO& playlistDao = internalCollection()->getPlaylistDAO();
    int autoDjPlaylistId = playlistDao.getPlaylistIdFromName(AUTODJ_TABLE);
    if (autoDjPlaylistId < 0) {
        autoDjPlaylistId = playlistDao.createPlaylist(AUTODJ_TABLE, PlaylistDAO::PLHT_AUTO_DJ);
    }
    // Auto DJ's default transition time, as the app's preferences keep it
    config()->set(ConfigKey(QStringLiteral("[Auto DJ]"), QStringLiteral("Transition")),
            ConfigValue(10));
    Engine engine(config());

    // The pairs and their tracks, analysed once as the app would
    QList<Pair> pairs;
    QHash<QString, TrackPointer> tracks;
    QHash<QString, crate::cache::TrackKey> trackKeys;
    const QJsonArray pairArray = m_pairs.value(QStringLiteral("pairs")).toArray();
    for (const QJsonValue& value : pairArray) {
        const QJsonObject object = value.toObject();
        Pair pair;
        pair.id = object.value(QStringLiteral("id")).toString();
        if (!only.isEmpty() && !only.contains(pair.id)) {
            continue;
        }
        pair.outgoingUid = object.value(QStringLiteral("outgoing")).toString();
        pair.incomingUid = object.value(QStringLiteral("incoming")).toString();
        pair.outgoingPath = object.value(QStringLiteral("outgoingPath")).toString();
        pair.incomingPath = object.value(QStringLiteral("incomingPath")).toString();
        pair.startMs = object.value(QStringLiteral("renderStartMs")).toDouble();
        pairs.append(pair);
        for (const auto& [uid, path] : {std::pair{pair.outgoingUid, pair.outgoingPath},
                     std::pair{pair.incomingUid, pair.incomingPath}}) {
            if (tracks.contains(uid)) {
                continue;
            }
            const TrackPointer pTrack = getOrAddTrackByLocation(path);
            ASSERT_TRUE(pTrack) << path.toStdString();
            const auto profileJson =
                    get(QStringLiteral("/api/tracks/by-entity/%1/mix-profile").arg(uid));
            ASSERT_TRUE(profileJson.has_value()) << uid.toStdString();
            const auto profile = crate::analysis::parseMixProfile(*profileJson);
            ASSERT_TRUE(profile.ok()) << profile.error().message.toStdString();
            // The app's default policy (CratePreferences::analysisPolicy())
            crate::analysis::AnalysisImporter::apply(
                    pTrack, profile.value(), crate::analysis::Policy());
            analyze(config(), pTrack);
            measure(pair.id.toStdString() + " track " + uid.toStdString() + " bpm " +
                    std::to_string(pTrack->getBpm()) + " replaygain " +
                    std::to_string(pTrack->getReplayGain().getRatio()));
            tracks.insert(uid, pTrack);
            trackKeys.insert(pTrack->getLocation(), crate::cache::TrackKey{kServerId, uid});
        }
    }
    ASSERT_FALSE(pairs.isEmpty());

    crate::PlanClient planClient(m_pClient);
    QFile deviation(QDir(m_outDir).filePath(QStringLiteral("deviation.csv")));
    const bool newDeviation = !deviation.exists();
    ASSERT_TRUE(deviation.open(QIODevice::Append | QIODevice::Text));
    QTextStream deviationOut(&deviation);
    if (newDeviation) {
        deviationOut << "pair,variant,pass,reason,detail,mixed,candidateIndex,usedBeatClock,"
                        "plannedStartOutgoingMs,startOutgoingMs,incomingStartMs,lateMs,"
                        "durationMs,overridden\n";
    }

    QModelIndexList queueRows;
    QStringList overridden;
    for (const Pair& pair : std::as_const(pairs)) {
        for (const Variant& variant : std::as_const(variants)) {
            for (const bool stems : {false, true}) {
                const QString runDir = QDir(m_outDir).filePath(
                        QStringLiteral("%1/%2").arg(pair.id, variant.name));
                ASSERT_TRUE(QDir().mkpath(runDir));
                engine.reset(!stems, variant.replayGain);
                // The bass handoff needs the default EQ on (F6.C-PR3); in
                // the stems pass the EQs are off, and the context is kept
                const bool defaultEq = crate::eqmap::isDefaultEqOn(engine.effects(), kDeck1) &&
                        crate::eqmap::isDefaultEqOn(engine.effects(), kDeck2);
                if (!stems) {
                    EXPECT_TRUE(defaultEq);
                }

                double nowMs = 0.0;
                auto pDirector = std::make_unique<crate::SmartMixDirector>(
                        std::make_unique<EngineClockEnvironment>(&nowMs));
                crate::SmartMixDirector* pDirectorRef = pDirector.get();
                QList<StateEvent> events;
                const auto positionMs = [&engine](int index) {
                    const QString group = engine.deck(index)->getGroup();
                    return VisualPlayPosition::getVisualPlayPosition(group)->getEnginePlayPos() *
                            ControlObject::get(ConfigKey(group, QStringLiteral("duration"))) *
                            1000.0;
                };
                // The director's states as they change, read after each step
                // of the run; a state passed within one step is noted at its time
                const QList<crate::SmartMixDirector::State> order{
                        crate::SmartMixDirector::State::Idle,
                        crate::SmartMixDirector::State::Armed,
                        crate::SmartMixDirector::State::Preparing,
                        crate::SmartMixDirector::State::Running};
                crate::SmartMixDirector::State noted = crate::SmartMixDirector::State::Idle;
                const auto noteState = [&](crate::SmartMixDirector::State state) {
                    if (state == noted) {
                        return;
                    }
                    // States of the way in that one step went through are
                    // noted too; an end state is noted alone
                    QList<crate::SmartMixDirector::State> passed;
                    const qsizetype from = order.indexOf(noted);
                    const qsizetype to = order.indexOf(state);
                    for (qsizetype i = from + 1; from >= 0 && i < to; ++i) {
                        passed.append(order[i]);
                    }
                    passed.append(state);
                    for (const crate::SmartMixDirector::State each : std::as_const(passed)) {
                        events.append(StateEvent{
                                QString::fromLatin1(
                                        QMetaEnum::fromType<crate::SmartMixDirector::State>()
                                                .valueToKey(static_cast<int>(each))),
                                nowMs,
                                positionMs(0),
                                positionMs(1)});
                    }
                    noted = state;
                };

                crate::AutoDJSmartMixAdapter::Options options;
                options.pConfig = config();
                options.install = true;
                options.trackOfFile = [&trackKeys](const QString& location)
                        -> std::optional<crate::cache::TrackKey> {
                    if (!trackKeys.contains(location)) {
                        return std::nullopt;
                    }
                    return trackKeys.value(location);
                };
                options.planClientFor = [&planClient](const QString&) {
                    return &planClient;
                };
                const bool v2Only = variant.plannerVersion == QLatin1String("smart-mix-v2");
                options.capabilitiesOf = [this, v2Only](const QString&) {
                    return capabilities(v2Only);
                };
                options.defaultEqOn = [](const QStringList&) {
                    return true;
                };
                options.pDirector = std::move(pDirector);
                options.settleTime = std::chrono::milliseconds(0);
                auto pAdapter = std::make_unique<crate::AutoDJSmartMixAdapter>(std::move(options));
                const crate::PlanContext context = pAdapter->contextFor(kServerId);
                ASSERT_EQ(variant.plannerVersion, context.plannerVersion);

                // Asked ahead, as the adapter does when the queue changes
                const crate::PlanEdge edge{pair.outgoingUid,
                        pair.incomingUid,
                        QString(),
                        QString()};
                {
                    QSignalSpy finished(
                            planClient.requestPlans({edge}, context), &crate::PlanFetch::finished);
                    ASSERT_TRUE(finished.wait(kNetworkWaitMs));
                }
                const auto asked = planClient.cachedPlan(edge, context);
                ASSERT_TRUE(asked.has_value()) << pair.id.toStdString();

                // The outgoing deck plays from renderStartMs; Auto DJ loads the
                // incoming one from its queue
                ASSERT_TRUE(engine.load(0, tracks.value(pair.outgoingUid)));
                const double outgoingDurationMs =
                        ControlObject::get(ConfigKey(kDeck1, QStringLiteral("duration"))) * 1000.0;
                ControlObject::set(ConfigKey(kDeck1, QStringLiteral("playposition")),
                        std::max(0.0, pair.startMs) / outgoingDurationMs);
                ControlObject::set(ConfigKey(kDeck1, QStringLiteral("play")), 1.0);
                engine.process();
                auto pAutoDJ = std::make_unique<EngineAutoDJ>(
                        config(), engine.players(), trackCollectionManager(), autoDjPlaylistId);
                {
                    PlaylistTableModel* pQueue = pAutoDJ->getTableModel();
                    queueRows.clear();
                    for (int row = 0; row < pQueue->rowCount(); ++row) {
                        queueRows.append(pQueue->index(row, 0));
                    }
                    pQueue->removeTracks(queueRows);
                    pQueue->appendTrack(tracks.value(pair.incomingUid)->getId());
                }
                pAutoDJ->setTransitionMode(AutoDJProcessor::TransitionMode::CrateSmartMix);
                ASSERT_EQ(AutoDJProcessor::ADJ_OK, pAutoDJ->toggleAutoDJ(true));
                ASSERT_TRUE(engine.waitLoaded(1));

                // The run, one buffer at a time
                std::vector<float> mix;
                std::vector<float> outgoing;
                std::vector<float> incoming;
                QList<TraceRow> trace;
                double nextTickMs = kTickMs;
                double engineMs = nowMs;
                std::optional<double> endedMs;
                std::optional<double> outgoingStoppedMs;
                const double limitMs = outgoingDurationMs - pair.startMs + 60000.0;
                while (engineMs < limitMs) {
                    engine.process();
                    engineMs += kBufferMs;
                    nowMs = engineMs;
                    QCoreApplication::processEvents();
                    noteState(pDirectorRef->state());
                    if (stems) {
                        engine.appendStem(0, &outgoing);
                        engine.appendStem(1, &incoming);
                    } else {
                        engine.appendMain(&mix);
                    }
                    const crate::SmartMixDirector::State state = pDirectorRef->state();
                    // The main thread until the next callback: the 16 ms timer
                    // and, while the director prepares, its start timer
                    const double nextEngineMs = engineMs + kBufferMs;
                    double timerMs = engineMs + kStartTimerStepMs;
                    while (true) {
                        const bool preparing = pDirectorRef->state() ==
                                crate::SmartMixDirector::State::Preparing;
                        const double next = preparing ? std::min(nextTickMs, timerMs) : nextTickMs;
                        if (next >= nextEngineMs) {
                            break;
                        }
                        nowMs = next;
                        pDirectorRef->tick();
                        QCoreApplication::processEvents();
                        noteState(pDirectorRef->state());
                        if (next >= nextTickMs) {
                            nextTickMs += kTickMs;
                        }
                        timerMs = next + kStartTimerStepMs;
                    }
                    nowMs = engineMs;

                    TraceRow row;
                    row.tMs = engineMs;
                    for (int index = 0; index < 2; ++index) {
                        const QString group = engine.deck(index)->getGroup();
                        row.positionMs[index] = positionMs(index);
                        row.play[index] = ControlObject::get(
                                ConfigKey(group, QStringLiteral("play")));
                        row.rate[index] =
                                ControlObject::get(ConfigKey(group, QStringLiteral("rate_ratio")));
                        row.pregain[index] =
                                ControlObject::get(ConfigKey(group, QStringLiteral("pregain")));
                        row.totalGain[index] =
                                ControlObject::get(ConfigKey(group, QStringLiteral("total_gain")));
                        row.appliedGain[index] = engine.appliedGain(index);
                        row.lowCutDb[index] = crate::eqmap::cutDbForLowGain(ControlObject::get(
                                ConfigKey(crate::eqmap::effectGroup(group),
                                        crate::eqmap::kLowGain)));
                        row.keylock[index] =
                                ControlObject::get(ConfigKey(group, QStringLiteral("keylock")));
                    }
                    row.crossfader = ControlObject::get(
                            ConfigKey(kMainGroup, QStringLiteral("crossfader")));
                    CSAMPLE_GAIN left = 1.0f;
                    CSAMPLE_GAIN right = 1.0f;
                    const QString xfader = QString(EngineXfader::kXfaderConfigKey);
                    EngineXfader::getXfadeGains(row.crossfader,
                            ControlObject::get(ConfigKey(xfader, QStringLiteral("xFaderCurve"))),
                            ControlObject::get(
                                    ConfigKey(xfader, QStringLiteral("xFaderCalibration"))),
                            ControlObject::get(ConfigKey(xfader, QStringLiteral("xFaderMode"))),
                            ControlObject::get(ConfigKey(xfader, QStringLiteral("xFaderReverse"))) >
                                    0.0,
                            &left,
                            &right);
                    row.crossfaderGain[0] = left;
                    row.crossfaderGain[1] = right;
                    row.directorState = static_cast<int>(state);
                    row.autoDjState = static_cast<int>(pAutoDJ->getState());
                    trace.append(row);

                    if (!outgoingStoppedMs && row.play[0] == 0.0) {
                        outgoingStoppedMs = engineMs;
                    }
                    if (!endedMs && outgoingStoppedMs && row.play[1] > 0.0) {
                        endedMs = engineMs;
                    }
                    if (endedMs && engineMs > *endedMs + kKeepAfterMs + 1000.0) {
                        break;
                    }
                    if (outgoingStoppedMs && row.play[1] == 0.0 &&
                            engineMs > *outgoingStoppedMs + 2000.0) {
                        break;
                    }
                    QThread::usleep(kPacingMicroseconds);
                }
                const crate::SmartMixDirector::Report report = pDirectorRef->report();
                const std::optional<crate::TransitionPlan> played = pDirectorRef->plan();

                // The part kept: from kKeepBeforeMs before the incoming deck
                // started to kKeepAfterMs after the outgoing one stopped
                double startedMs = nowMs;
                for (const StateEvent& event : std::as_const(events)) {
                    if (event.state == QLatin1String("Running")) {
                        startedMs = event.tMs;
                    }
                }
                if (startedMs == nowMs) {
                    // Auto DJ's own fade: from the first buffer where deck 2 plays
                    for (const TraceRow& row : std::as_const(trace)) {
                        if (row.play[1] > 0.0) {
                            startedMs = row.tMs;
                            break;
                        }
                    }
                }
                const double keepFromMs = std::max(0.0, startedMs - kKeepBeforeMs);
                const double keepToMs = std::min(nowMs, endedMs.value_or(nowMs) + kKeepAfterMs);
                const auto firstFrame = static_cast<std::size_t>(
                        std::floor(keepFromMs / kBufferMs) * kBufferFrames);
                const auto lastFrame = static_cast<std::size_t>(
                        std::floor(keepToMs / kBufferMs) * kBufferFrames);
                const auto crop = [firstFrame, lastFrame](const std::vector<float>& samples) {
                    const std::size_t from = std::min(samples.size(), firstFrame * 2);
                    const std::size_t to = std::min(samples.size(), lastFrame * 2);
                    return std::vector<float>(samples.begin() + from, samples.begin() + to);
                };
                const double keptFromMs =
                        static_cast<double>(firstFrame) * 1000.0 / kSampleRate;
                if (stems) {
                    ASSERT_TRUE(writeFloatWav(
                            QDir(runDir).filePath(QStringLiteral("outgoing.wav")), crop(outgoing)));
                    ASSERT_TRUE(writeFloatWav(
                            QDir(runDir).filePath(QStringLiteral("incoming.wav")), crop(incoming)));
                } else {
                    ASSERT_TRUE(writeFloatWav(
                            QDir(runDir).filePath(QStringLiteral("mix.wav")), crop(mix)));
                }

                QFile traceFile(QDir(runDir).filePath(
                        stems ? QStringLiteral("trace-stems.csv") : QStringLiteral("trace.csv")));
                ASSERT_TRUE(traceFile.open(QIODevice::WriteOnly | QIODevice::Text));
                QTextStream traceOut(&traceFile);
                traceOut << "tMs,pos1Ms,pos2Ms,play1,play2,rate1,rate2,pregain1,pregain2,"
                            "totalGain1,totalGain2,appliedGain1,appliedGain2,lowCut1Db,"
                            "lowCut2Db,crossfader,xfGain1,xfGain2,keylock1,keylock2,"
                            "directorState,autoDjState\n";
                for (const TraceRow& row : std::as_const(trace)) {
                    if (row.tMs < keptFromMs || row.tMs > keepToMs + kBufferMs) {
                        continue;
                    }
                    traceOut << QString::number(row.tMs - keptFromMs, 'f', 3);
                    for (const double value : {row.positionMs[0],
                                 row.positionMs[1],
                                 row.play[0],
                                 row.play[1],
                                 row.rate[0],
                                 row.rate[1],
                                 row.pregain[0],
                                 row.pregain[1],
                                 row.totalGain[0],
                                 row.totalGain[1],
                                 row.appliedGain[0],
                                 row.appliedGain[1],
                                 row.lowCutDb[0],
                                 row.lowCutDb[1],
                                 row.crossfader,
                                 row.crossfaderGain[0],
                                 row.crossfaderGain[1],
                                 row.keylock[0],
                                 row.keylock[1]}) {
                        traceOut << ',' << QString::number(value, 'g', 9);
                    }
                    traceOut << ',' << row.directorState << ',' << row.autoDjState << '\n';
                }

                QJsonArray eventArray;
                for (const StateEvent& event : std::as_const(events)) {
                    eventArray.append(QJsonObject{
                            {QStringLiteral("state"), event.state},
                            {QStringLiteral("tMs"), event.tMs - keptFromMs},
                            {QStringLiteral("outgoingMs"), event.outgoingMs},
                            {QStringLiteral("incomingMs"), event.incomingMs},
                    });
                }
                overridden.clear();
                for (const ConfigKey& key : report.overridden) {
                    overridden.append(key.group + key.item);
                }
                QJsonObject reportJson{
                        {QStringLiteral("pair"), pair.id},
                        {QStringLiteral("variant"), variant.name},
                        {QStringLiteral("pass"),
                                stems ? QStringLiteral("stems") : QStringLiteral("mix")},
                        {QStringLiteral("plannerVersion"), context.plannerVersion},
                        {QStringLiteral("localRefinement"), context.localRefinement},
                        {QStringLiteral("allowBassHandoff"), context.allowBassHandoff},
                        {QStringLiteral("preferredDurationMs"), context.preferredDurationMs},
                        {QStringLiteral("defaultEqOn"), defaultEq},
                        {QStringLiteral("replayGain"), variant.replayGain},
                        {QStringLiteral("keptFromMs"), keptFromMs},
                        {QStringLiteral("renderStartMs"), pair.startMs},
                        {QStringLiteral("outgoingDurationMs"), outgoingDurationMs},
                        {QStringLiteral("incomingDurationMs"),
                                ControlObject::get(ConfigKey(kDeck2, QStringLiteral("duration"))) *
                                        1000.0},
                        {QStringLiteral("outgoingBpm"),
                                tracks.value(pair.outgoingUid)->getBpm()},
                        {QStringLiteral("incomingBpm"),
                                tracks.value(pair.incomingUid)->getBpm()},
                        {QStringLiteral("outgoingReplayGain"),
                                tracks.value(pair.outgoingUid)->getReplayGain().getRatio()},
                        {QStringLiteral("incomingReplayGain"),
                                tracks.value(pair.incomingUid)->getReplayGain().getRatio()},
                        {QStringLiteral("events"), eventArray},
                        {QStringLiteral("reason"), report.reason},
                        {QStringLiteral("detail"), report.detail},
                        {QStringLiteral("mixed"), report.mixed},
                        {QStringLiteral("candidateIndex"), report.candidateIndex},
                        {QStringLiteral("usedBeatClock"), report.usedBeatClock},
                        {QStringLiteral("plannedStartOutgoingMs"), report.plannedStartOutgoingMs},
                        {QStringLiteral("startOutgoingMs"), report.startOutgoingMs},
                        {QStringLiteral("incomingStartMs"), report.incomingStartMs},
                        {QStringLiteral("lateMs"), report.lateMs},
                        {QStringLiteral("durationMs"), report.durationMs},
                        {QStringLiteral("overridden"), QJsonArray::fromStringList(overridden)},
                        {QStringLiteral("askedPlan"), planJson(*asked)},
                };
                if (played) {
                    reportJson.insert(QStringLiteral("playedPlan"), planJson(*played));
                }
                QJsonArray candidateChecks;
                for (const crate::TransitionPlanCandidate& candidate : asked->candidates) {
                    const crate::TransitionPlan& plan = candidate.plan;
                    QJsonObject check{
                            {QStringLiteral("index"), candidate.index},
                            {QStringLiteral("mixes"), plan.mixes()},
                            {QStringLiteral("codes"),
                                    QJsonArray::fromStringList(plan.policyLimits
                                                    ? plan.limitViolations(*plan.policyLimits)
                                                    : QStringList())},
                    };
                    const auto problem = plan.validate(
                            static_cast<qint64>(outgoingDurationMs),
                            static_cast<qint64>(ControlObject::get(ConfigKey(
                                                        kDeck2, QStringLiteral("duration"))) *
                                    1000.0));
                    check.insert(QStringLiteral("validateOnDecks"),
                            problem ? QJsonValue(*problem) : QJsonValue());
                    candidateChecks.append(check);
                }
                reportJson.insert(QStringLiteral("candidateChecks"), candidateChecks);
                QFile reportFile(QDir(runDir).filePath(stems
                                ? QStringLiteral("report-stems.json")
                                : QStringLiteral("report.json")));
                ASSERT_TRUE(reportFile.open(QIODevice::WriteOnly));
                reportFile.write(QJsonDocument(reportJson).toJson());

                deviationOut << pair.id << ',' << variant.name << ','
                             << (stems ? "stems" : "mix") << ',' << report.reason << ",\""
                             << QString(report.detail).replace(QLatin1Char('"'), QLatin1Char('\''))
                             << "\"," << (report.mixed ? 1 : 0) << ',' << report.candidateIndex
                             << ',' << (report.usedBeatClock ? 1 : 0) << ','
                             << report.plannedStartOutgoingMs << ',' << report.startOutgoingMs
                             << ',' << report.incomingStartMs << ',' << report.lateMs << ','
                             << report.durationMs << ',' << overridden.join(QLatin1Char(' '))
                             << '\n';
                deviationOut.flush();
                measure(pair.id.toStdString() + " " + variant.name.toStdString() +
                        (stems ? " stems" : " mix") + ": " + report.reason.toStdString() +
                        (report.mixed ? " mixed" : " not mixed") + ", late " +
                        std::to_string(report.lateMs) + " ms, candidate " +
                        std::to_string(report.candidateIndex) + ", beat clock " +
                        std::to_string(report.usedBeatClock));

                pAutoDJ->toggleAutoDJ(false);
                pAdapter.reset();
                pAutoDJ.reset();
                QCoreApplication::processEvents();
            }
        }
    }
}

// G4b: Decks' check of the limits against the server's `limit_violations`
// on the candidates of the G4 pairs' menus and their variations
// (limit_cases.py runs the server's own function).
TEST_F(CrateDecksG4Render, LimitCheckAgreement) {
    const QString casesPath = qEnvironmentVariable("CRATE_DECKS_G4_LIMIT_CASES");
    if (casesPath.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_DECKS_G4_LIMIT_CASES";
    }
    QFile file(casesPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonArray cases = QJsonDocument::fromJson(file.readAll())
                                     .object()
                                     .value(QStringLiteral("cases"))
                                     .toArray();
    int agreeing = 0;
    int mixesAgreeing = 0;
    int withCodes = 0;
    QJsonArray results;
    QStringList expected;
    for (const QJsonValue& value : cases) {
        const QJsonObject testCase = value.toObject();
        QJsonObject json = testCase.value(QStringLiteral("plan")).toObject();
        json.insert(QStringLiteral("policyLimits"), testCase.value(QStringLiteral("policyLimits")));
        const auto parsed = crate::TransitionPlan::fromJson(json);
        expected.clear();
        const QJsonArray codes = testCase.value(QStringLiteral("codes")).toArray();
        for (const QJsonValue& code : codes) {
            expected.append(code.toString());
        }
        withCodes += expected.isEmpty() ? 0 : 1;
        QJsonObject result{{QStringLiteral("name"), testCase.value(QStringLiteral("name"))}};
        if (!parsed.ok()) {
            result.insert(QStringLiteral("error"), parsed.error().message);
            results.append(result);
            continue;
        }
        const crate::TransitionPlan& plan = parsed.value();
        const QStringList decks = plan.limitViolations(*plan.policyLimits);
        const bool same = decks == expected;
        const bool mixesSame = plan.mixes() == testCase.value(QStringLiteral("mixes")).toBool();
        agreeing += same ? 1 : 0;
        mixesAgreeing += mixesSame ? 1 : 0;
        result.insert(QStringLiteral("decksCodes"), QJsonArray::fromStringList(decks));
        result.insert(QStringLiteral("serverCodes"), QJsonArray::fromStringList(expected));
        result.insert(QStringLiteral("agrees"), same);
        result.insert(QStringLiteral("mixesAgrees"), mixesSame);
        results.append(result);
    }
    QFile out(QDir(m_outDir).filePath(QStringLiteral("limit-agreement.json")));
    ASSERT_TRUE(out.open(QIODevice::WriteOnly));
    out.write(QJsonDocument(QJsonObject{{QStringLiteral("results"), results}}).toJson());
    measure("limit check: Decks agrees with the server on " + std::to_string(agreeing) + " of " +
            std::to_string(cases.size()) + " cases (" + std::to_string(withCodes) +
            " with codes), mixes() on " + std::to_string(mixesAgreeing));
}

} // namespace crate_decks_g4
