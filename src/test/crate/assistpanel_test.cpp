// The Smart Mix assist mode (plan F6.C-PR4): the plan from the master deck to
// the other one, asked of the Crate server with the assistant's context, and
// played by the director when the DJ prepares or executes it, from the
// assistant or from a controller. The decks are plain controls and the
// engine, the clock and the network are faked; the plans are the real
// fixtures of F6.C-PR1.
//
// TEST_GAP: how the real engine plays these transitions is measured on
// rendered transitions by G4's metrics.

#include "crate/smartmix/assistpanel.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <map>
#include <memory>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "crate/client/crateclient.h"
#include "crate/cratepreferences.h"
#include "crate/smartmix/eqmap.h"
#include "crate/smartmix/planclient.h"
#include "crate/smartmix/smartmixassist.h"
#include "crate/smartmix/smartmixdirector.h"
#include "crate/ui/widgets/transitionplanview.h"
#include "crate/ui/widgets/transitionplanviewmodel.h"
#include "mixer/playerinfo.h"
#include "test/crate/fakecratenetwork.h"
#include "test/mixxxtest.h"
#include "track/track.h"

namespace {

using crate::SmartMixAssist;
using crate::SmartMixDirector;
using Status = SmartMixAssist::Status;
using State = SmartMixDirector::State;

const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");
const QString kServer = QStringLiteral("srv-1");
// The anonymised uids and durations of the real plan fixtures
// (src/test/crate/data/transition-plans/)
const QString kOutgoingUid = QStringLiteral("637bd7c0-380e-5bc7-b841-dc2234cdfaf7");
const QString kIncomingUid = QStringLiteral("34137088-fcab-5f93-8866-31f1bf6d0871");
constexpr double kOutgoingSeconds = 427.298;
constexpr double kIncomingSeconds = 148.052;
const QString kPlansPath = QStringLiteral("/api/playback/transition-plans");
constexpr double kTickMs = SmartMixDirector::kTickMs;
// The real beatmatch plan's cue
constexpr double kCueMs = 418135.0;

/// The clock, and the decks' positions from their controls.
class FakeEnvironment : public crate::SmartMixEnvironment {
  public:
    explicit FakeEnvironment(const double* pNow)
            : m_pNow(pNow) {
    }
    double nowMs() const override {
        return *m_pNow;
    }
    double enginePlayPosition(const QString& group) const override {
        return ControlObject::get(ConfigKey(group, QStringLiteral("playposition")));
    }
    QString trackKey(const QString& group) const override {
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
        return pTrack ? pTrack->getLocation() : QString();
    }

  private:
    const double* const m_pNow;
};

class CrateSmartMixAssistTest : public MixxxTest {
  protected:
    CrateSmartMixAssistTest() {
        addControl(QStringLiteral("[App]"), QStringLiteral("num_decks"), 2.0);
        addControl(QStringLiteral("[App]"), QStringLiteral("num_samplers"), 0.0);
        addControl(QStringLiteral("[App]"), QStringLiteral("num_preview_decks"), 0.0);
        for (const QString& group : {kDeck1, kDeck2}) {
            for (const QString& item : {QStringLiteral("playposition"),
                         QStringLiteral("duration"),
                         QStringLiteral("bpm"),
                         QStringLiteral("beat_distance"),
                         QStringLiteral("quantize"),
                         QStringLiteral("sync_enabled"),
                         QStringLiteral("keylock"),
                         QStringLiteral("orientation")}) {
                addControl(group, item, 0.0);
            }
            auto pPlay = std::make_unique<ControlPushButton>(ConfigKey(group, "play"));
            pPlay->setButtonMode(mixxx::control::ButtonMode::Toggle);
            m_buttons.push_back(std::move(pPlay));
            addControl(group, QStringLiteral("pregain"), 1.0);
            addControl(group, QStringLiteral("volume"), 1.0);
            addControl(group, QStringLiteral("rate_ratio"), 1.0);
            const QString eq = crate::eqmap::effectGroup(group);
            for (const QString& item : {crate::eqmap::kLowGain,
                         crate::eqmap::kMidGain,
                         crate::eqmap::kHighGain}) {
                addControl(eq, item, crate::eqmap::kNeutralGain);
            }
        }
        set(kDeck2, QStringLiteral("orientation"), 2.0);
        addControl(QStringLiteral("[Master]"), QStringLiteral("crossfader"), -1.0);
        auto pAutoDj = std::make_unique<ControlPushButton>(ConfigKey("[AutoDJ]", "enabled"));
        pAutoDj->setButtonMode(mixxx::control::ButtonMode::Toggle);
        m_buttons.push_back(std::move(pAutoDj));
        // After the controls it reads
        PlayerInfo::create();

        m_outgoingFile = getTestDataDir().filePath(QStringLiteral("outgoing.mp3"));
        m_incomingFile = getTestDataDir().filePath(QStringLiteral("incoming.mp3"));
        crate::ClientOptions clientOptions;
        clientOptions.deviceFingerprint = QStringLiteral("install-1");
        m_pClient = std::make_unique<crate::Client>(
                QUrl(QStringLiteral("https://crate.test")), &m_network, clientOptions);
        m_pClient->setTokenSource(&m_tokens);
        crate::PlanClient::Options planOptions;
        planOptions.schedule = m_scheduler.function();
        m_pPlans = std::make_unique<crate::PlanClient>(m_pClient.get(), std::move(planOptions));
    }

    ~CrateSmartMixAssistTest() override {
        m_pAssist.reset();
        m_pPlans.reset();
        m_pClient.reset();
        PlayerInfo::destroy();
    }

    void addControl(const QString& group, const QString& item, double value) {
        const ConfigKey key(group, item);
        m_controls[key] = std::make_unique<ControlObject>(key, true, false, false, value);
    }
    static void set(const QString& group, const QString& item, double value) {
        ControlObject::set(ConfigKey(group, item), value);
    }
    static double get(const QString& group, const QString& item) {
        return ControlObject::get(ConfigKey(group, item));
    }

    void makeAssist(bool bassHandoff = true) {
        SmartMixAssist::Options options;
        options.trackOfFile =
                [this](const QString& location) -> std::optional<crate::cache::TrackKey> {
            if (location == m_outgoingFile) {
                return crate::cache::TrackKey{kServer, kOutgoingUid};
            }
            if (location == m_incomingFile) {
                return crate::cache::TrackKey{kServer, kIncomingUid};
            }
            return std::nullopt;
        };
        options.planClientFor = [this](const QString& serverId) {
            return serverId == kServer ? m_pPlans.get() : nullptr;
        };
        options.capabilitiesOf =
                [bassHandoff](const QString&) -> std::optional<crate::Capabilities> {
            const QByteArray features = bassHandoff
                    ? QByteArray(R"("beatmatch", "bass_handoff", "long_beatmatch")")
                    : QByteArray(R"("beatmatch")");
            const auto capabilities = crate::Capabilities::fromJson(QJsonDocument::fromJson(
                    R"({"smart_mix": {"available": true,
                        "planner_versions": ["smart-mix-v2", "smart-mix-v3"]},
                        "decks": {"available": true, "min_client_version": "0.1.0",
                        "contract_version": 1, "features": [)" +
                    features + "]}}"));
            return capabilities.value();
        };
        options.defaultEqOn = [](const QStringList&) {
            return true;
        };
        options.masterDeck = [this] {
            return m_master;
        };
        options.pEnvironment = std::make_unique<FakeEnvironment>(&m_now);
        options.pDirector = std::make_unique<SmartMixDirector>(
                std::make_unique<FakeEnvironment>(&m_now));
        options.settleTime = std::chrono::milliseconds(1);
        options.pPreferences = m_pPreferences;
        m_pAssist = std::make_unique<SmartMixAssist>(std::move(options));
    }

    /// Deck 1 plays the outgoing track `msBeforeCue` before the real
    /// beatmatch plan's cue, and deck 2 holds the incoming one, stopped.
    void loadDecks(double msBeforeCue = 60000.0) {
        set(kDeck1, QStringLiteral("duration"), kOutgoingSeconds);
        set(kDeck2, QStringLiteral("duration"), kIncomingSeconds);
        set(kDeck1,
                QStringLiteral("playposition"),
                (kCueMs - msBeforeCue) / (kOutgoingSeconds * 1000.0));
        PlayerInfo::instance().setTrackInfo(kDeck1, Track::newTemporary(m_outgoingFile));
        PlayerInfo::instance().setTrackInfo(kDeck2, Track::newTemporary(m_incomingFile));
        set(kDeck1, QStringLiteral("play"), 1.0);
        m_master = kDeck1;
    }

    QList<crate::test::FakeNetworkAccessManager::Sent> planRequests() const {
        QList<crate::test::FakeNetworkAccessManager::Sent> requests;
        for (const auto& sent : m_network.sent) {
            if (sent.request.url().path() == kPlansPath) {
                requests.append(sent);
            }
        }
        return requests;
    }

    static QJsonObject bodyOf(const crate::test::FakeNetworkAccessManager::Sent& sent) {
        return QJsonDocument::fromJson(sent.body).object();
    }

    QJsonObject fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/transition-plans/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    /// Answers the open plan requests with the fixture `name`.
    void answerPlans(const QString& name = QStringLiteral("real-beatmatch.json")) {
        const QJsonObject plan = fixture(name);
        for (const auto& sent : std::as_const(m_network.sent)) {
            if (sent.request.url().path() != kPlansPath || !sent.pReply ||
                    sent.pReply->isFinished()) {
                continue;
            }
            sent.pReply->respond(200,
                    QJsonDocument(QJsonObject{
                                          {QStringLiteral("plannerVersion"),
                                                  QStringLiteral("smart-mix-v3")},
                                          {QStringLiteral("plans"), QJsonArray{plan}},
                                  })
                            .toJson());
        }
    }

    bool waitFor(const std::function<bool()>& condition) {
        return QTest::qWaitFor(condition, 5000);
    }

    /// The decks as the assistant asks: the plan of deck 1 → deck 2 ready.
    void readyPlan(const QString& name = QStringLiteral("real-beatmatch.json")) {
        loadDecks();
        makeAssist();
        ASSERT_TRUE(waitFor([this] { return !planRequests().isEmpty(); }));
        answerPlans(name);
        ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::Ready; }));
    }

    /// Plays the decks on for `ms`, as the engine would, and ticks the
    /// director at 60 Hz.
    void play(double ms) {
        for (double elapsed = 0.0; elapsed < ms; elapsed += kTickMs) {
            m_now += kTickMs;
            for (const auto& [group, seconds] :
                    {std::pair{kDeck1, kOutgoingSeconds}, std::pair{kDeck2, kIncomingSeconds}}) {
                if (get(group, QStringLiteral("play")) > 0.5) {
                    set(group,
                            QStringLiteral("playposition"),
                            std::min(1.0,
                                    get(group, QStringLiteral("playposition")) +
                                            kTickMs * get(group, QStringLiteral("rate_ratio")) /
                                                    (seconds * 1000.0)));
                }
            }
            m_pAssist->director()->tick();
            QCoreApplication::processEvents();
        }
    }

    std::map<ConfigKey, std::unique_ptr<ControlObject>> m_controls;
    std::vector<std::unique_ptr<ControlPushButton>> m_buttons;
    double m_now = 1000.0;
    QString m_master;
    QString m_outgoingFile;
    QString m_incomingFile;
    crate::test::FakeNetworkAccessManager m_network;
    crate::test::FakeTokenSource m_tokens;
    crate::test::FakeScheduler m_scheduler;
    std::unique_ptr<crate::Client> m_pClient;
    std::unique_ptr<crate::PlanClient> m_pPlans;
    /// The assistant's preferences; none by default.
    crate::CratePreferences* m_pPreferences = nullptr;
    std::unique_ptr<SmartMixAssist> m_pAssist;
};

// With a deck on air and the other one loaded, the assistant asks the server
// for that edge's plan with its own context, and holds the plan once it
// fits the tracks.
TEST_F(CrateSmartMixAssistTest, AsksThePlanOfTheLoadedDecks) {
    makeAssist();
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::NoTransition; }));
    EXPECT_FALSE(m_pAssist->canExecute());

    loadDecks(120000.0);
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::Requesting; }));
    EXPECT_EQ(kDeck1, m_pAssist->outgoingGroup());
    EXPECT_EQ(kDeck2, m_pAssist->incomingGroup());
    ASSERT_EQ(1, planRequests().size());
    const QJsonObject body = bodyOf(planRequests().constFirst());
    EXPECT_EQ(QStringLiteral("smart-mix-v3"),
            body.value(QStringLiteral("plannerVersion")).toString());
    const QJsonObject edge = body.value(QStringLiteral("edges")).toArray().first().toObject();
    EXPECT_EQ(kOutgoingUid, edge.value(QStringLiteral("outgoingTrackEntityUid")).toString());
    EXPECT_EQ(kIncomingUid, edge.value(QStringLiteral("incomingTrackEntityUid")).toString());
    const QJsonObject context = edge.value(QStringLiteral("context")).toObject();
    EXPECT_EQ(QStringLiteral("decks"), context.value(QStringLiteral("source")).toString());
    EXPECT_TRUE(context.value(QStringLiteral("assist")).toBool());
    EXPECT_TRUE(context.value(QStringLiteral("automatic")).toBool());
    EXPECT_EQ(8000, context.value(QStringLiteral("preferredDurationMs")).toInt());
    EXPECT_TRUE(context.value(QStringLiteral("allowBeatmatch")).toBool());
    EXPECT_TRUE(context.value(QStringLiteral("allowBassHandoff")).toBool());
    EXPECT_TRUE(context.value(QStringLiteral("allowLongBeatmatch")).toBool());

    answerPlans(QStringLiteral("synthetic-assist-long-blend.json"));
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::Ready; }));
    ASSERT_TRUE(m_pAssist->plan().has_value());
    EXPECT_EQ(96000, m_pAssist->plan()->durationMs);
    EXPECT_TRUE(m_pAssist->plan()->hasBassHandoff());
    EXPECT_TRUE(m_pAssist->canPrepare());
    EXPECT_TRUE(m_pAssist->canExecute());
    EXPECT_FALSE(m_pAssist->canCancel());
    // The long blend starts at its own cue, 88 s before the real plan's
    ASSERT_TRUE(m_pAssist->msToEntry().has_value());
    EXPECT_NEAR(120000.0 - (kCueMs - 330298.0), *m_pAssist->msToEntry(), 1.0);
}

/// The transition length of each plan request, in milliseconds.
QList<int> preferredDurations(const QList<crate::test::FakeNetworkAccessManager::Sent>& requests) {
    QList<int> durations;
    for (const auto& sent : requests) {
        const QJsonObject edge = QJsonDocument::fromJson(sent.body)
                                         .object()
                                         .value(QStringLiteral("edges"))
                                         .toArray()
                                         .first()
                                         .toObject();
        durations.append(edge.value(QStringLiteral("context"))
                        .toObject()
                        .value(QStringLiteral("preferredDurationMs"))
                        .toInt());
    }
    return durations;
}

// The assistant asks for the transition length of the preferences, and
// asks again for the plan of the same decks when it changes.
TEST_F(CrateSmartMixAssistTest, AsksForTheTransitionLengthOfThePreferences) {
    crate::CratePreferences preferences(nullptr);
    preferences.setSmartMixAssistTransitionSeconds(12);
    m_pPreferences = &preferences;
    readyPlan();
    EXPECT_EQ(QList<int>{12000}, preferredDurations(planRequests()));
    EXPECT_EQ(12000, m_pAssist->contextFor(kServer, {kDeck1, kDeck2}).preferredDurationMs);

    preferences.setSmartMixAssistTransitionSeconds(4);
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::Requesting; }));
    EXPECT_EQ(QList<int>({12000, 4000}), preferredDurations(planRequests()));
    EXPECT_FALSE(m_pAssist->plan().has_value());
    answerPlans();
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::Ready; }));

    // Without a change, the plan holds
    preferences.setSmartMixAssistTransitionSeconds(4);
    m_pAssist->refresh();
    EXPECT_EQ(Status::Ready, m_pAssist->status());
    EXPECT_EQ(2, planRequests().size());
    // Beyond what the server takes, it asks for the most: the first plan,
    // which the plan client keeps
    preferences.setSmartMixAssistTransitionSeconds(60);
    EXPECT_EQ(12000, m_pAssist->contextFor(kServer, {kDeck1, kDeck2}).preferredDurationMs);
    QSignalSpy changed(m_pAssist.get(), &SmartMixAssist::changed);
    m_pAssist->refresh();
    EXPECT_GE(changed.size(), 2) << "it asked again";
    EXPECT_EQ(Status::Ready, m_pAssist->status());
    EXPECT_EQ(0, m_pAssist->planLatencyMs());
    EXPECT_EQ(2, planRequests().size());
    // Before the preferences go
    m_pAssist.reset();
}

// The other deck must hold a stopped track of the same Crate server.
TEST_F(CrateSmartMixAssistTest, NeedsTwoCrateTracks) {
    loadDecks();
    PlayerInfo::instance().setTrackInfo(
            kDeck2, Track::newTemporary(getTestDataDir().filePath(QStringLiteral("local.mp3"))));
    makeAssist();
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::NotCrate; }));

    PlayerInfo::instance().setTrackInfo(kDeck2, Track::newTemporary(m_incomingFile));
    set(kDeck2, QStringLiteral("play"), 1.0);
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::NoTransition; }));
    EXPECT_TRUE(planRequests().isEmpty());
}

// Execute hands the plan to the director, which plays it from the plan's
// entry: deck 2 starts at the cue, at the plan's tempo.
TEST_F(CrateSmartMixAssistTest, ExecuteRunsDirector) {
    readyPlan();
    ASSERT_TRUE(m_pAssist->execute());
    EXPECT_EQ(Status::Executing, m_pAssist->status());
    EXPECT_TRUE(m_pAssist->canCancel());
    EXPECT_FALSE(m_pAssist->canExecute());

    play(60000.0 + 2 * kTickMs);
    ASSERT_EQ(Status::Running, m_pAssist->status());
    EXPECT_EQ(State::Running, m_pAssist->director()->state());
    EXPECT_EQ(1.0, get(kDeck2, QStringLiteral("play")));
    EXPECT_NEAR(1.023355, get(kDeck2, QStringLiteral("rate_ratio")), 1e-6);
    EXPECT_EQ(1.0, get(kDeck2, QStringLiteral("keylock")));
    play(8000.0 + 4 * kTickMs);
    EXPECT_EQ(State::Completed, m_pAssist->director()->state());
    EXPECT_EQ(0.0, get(kDeck1, QStringLiteral("play")));
}

// Prepare cues the other deck at once and holds it; Execute lets the
// transition start at the entry, and Cancel puts the decks back.
TEST_F(CrateSmartMixAssistTest, PrepareHoldsUntilExecute) {
    readyPlan();
    set(kDeck2, QStringLiteral("quantize"), 1.0);
    ASSERT_TRUE(m_pAssist->prepare());
    EXPECT_EQ(Status::Prepared, m_pAssist->status());
    EXPECT_EQ(State::Preparing, m_pAssist->director()->state());
    EXPECT_EQ(0.0, get(kDeck2, QStringLiteral("quantize")));
    EXPECT_FALSE(m_pAssist->canPrepare());
    EXPECT_TRUE(m_pAssist->canExecute());

    // Held at the entry
    play(60000.0 + 2 * kTickMs);
    EXPECT_EQ(Status::Prepared, m_pAssist->status());
    EXPECT_EQ(0.0, get(kDeck2, QStringLiteral("play")));

    m_pAssist->cancel();
    EXPECT_EQ(State::Cancelled, m_pAssist->director()->state());
    EXPECT_EQ(1.0, get(kDeck2, QStringLiteral("quantize")));
    EXPECT_EQ(0.0, get(kDeck2, QStringLiteral("play")));
}

// The controls a controller maps do what the buttons do.
TEST_F(CrateSmartMixAssistTest, ControlsAreMappable) {
    readyPlan();
    ControlProxy prepare(SmartMixAssist::kControlGroup, QStringLiteral("prepare"));
    ControlProxy execute(SmartMixAssist::kControlGroup, QStringLiteral("execute"));
    ControlProxy cancel(SmartMixAssist::kControlGroup, QStringLiteral("cancel"));
    ASSERT_TRUE(prepare.valid());
    ASSERT_TRUE(execute.valid());
    ASSERT_TRUE(cancel.valid());
    for (const ControlProxy* pControl : {&prepare, &execute, &cancel}) {
        const auto* pButton = qobject_cast<ControlPushButton*>(
                ControlObject::getControl(pControl->getKey()));
        ASSERT_NE(nullptr, pButton);
        EXPECT_EQ(mixxx::control::ButtonMode::Push, pButton->getButtonMode());
    }

    // A pad: pressed, then released
    prepare.set(1.0);
    prepare.set(0.0);
    EXPECT_EQ(Status::Prepared, m_pAssist->status());
    execute.set(1.0);
    execute.set(0.0);
    EXPECT_EQ(Status::Executing, m_pAssist->status());
    cancel.set(1.0);
    cancel.set(0.0);
    EXPECT_EQ(State::Cancelled, m_pAssist->director()->state());
}

// Auto DJ comes first: it cancels the assistant's transition, and while it
// is on the assistant plans and plays nothing.
TEST_F(CrateSmartMixAssistTest, AutoDJComesFirst) {
    readyPlan();
    ASSERT_TRUE(m_pAssist->prepare());
    ControlProxy autoDj(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"));
    autoDj.set(1.0);
    EXPECT_EQ(State::Cancelled, m_pAssist->director()->state());
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::AutoDj; }));
    EXPECT_FALSE(m_pAssist->plan().has_value());
    EXPECT_FALSE(m_pAssist->execute());
    EXPECT_FALSE(m_pAssist->prepare());

    autoDj.set(0.0);
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::Ready; }));
    // From the plan client's cache
    EXPECT_EQ(1, planRequests().size());
    EXPECT_TRUE(m_pAssist->canExecute());
}

// A new track on the other deck asks for its plan.
TEST_F(CrateSmartMixAssistTest, AsksAgainWhenADeckLoads) {
    readyPlan();
    const QString other = getTestDataDir().filePath(QStringLiteral("other.mp3"));
    PlayerInfo::instance().setTrackInfo(kDeck2, Track::newTemporary(other));
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::NotCrate; }));
    EXPECT_FALSE(m_pAssist->plan().has_value());

    PlayerInfo::instance().setTrackInfo(kDeck2, Track::newTemporary(m_incomingFile));
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::Ready; }));
}

// Plan F8.SM: the correction of a deck's track changed and its PlanClient
// dropped its plans: the assistant asks for the plan again, unless the
// transition is under way. Other tracks and servers change nothing.
TEST_F(CrateSmartMixAssistTest, AsksAgainWhenATracksCorrectionChanges) {
    readyPlan();
    ASSERT_EQ(1, planRequests().size());
    m_pAssist->trackPlansChanged(kServer, QStringLiteral("00000000-0000-4000-8000-000000000009"));
    m_pAssist->trackPlansChanged(QStringLiteral("srv-2"), kIncomingUid);
    EXPECT_EQ(Status::Ready, m_pAssist->status());
    EXPECT_TRUE(m_pAssist->plan().has_value());

    m_pPlans->invalidateTrack(kIncomingUid);
    m_pAssist->trackPlansChanged(kServer, kIncomingUid.toUpper());
    EXPECT_EQ(Status::Requesting, m_pAssist->status());
    EXPECT_FALSE(m_pAssist->plan().has_value());
    ASSERT_TRUE(waitFor([this] { return planRequests().size() == 2; }));
    answerPlans();
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::Ready; }));

    // A prepared transition keeps its plan
    ASSERT_TRUE(m_pAssist->prepare());
    ASSERT_EQ(Status::Prepared, m_pAssist->status());
    m_pPlans->invalidateTrack(kOutgoingUid);
    m_pAssist->trackPlansChanged(kServer, kOutgoingUid);
    EXPECT_EQ(Status::Prepared, m_pAssist->status());
    EXPECT_TRUE(m_pAssist->plan().has_value());
    QTest::qWait(20);
    EXPECT_EQ(2, planRequests().size());
}

// A plan that does not fit the tracks is not offered.
TEST_F(CrateSmartMixAssistTest, RejectsAPlanThatDoesNotFit) {
    loadDecks();
    set(kDeck2, QStringLiteral("duration"), 1.0);
    makeAssist();
    ASSERT_TRUE(waitFor([this] { return !planRequests().isEmpty(); }));
    answerPlans();
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::NoPlan; }));
    EXPECT_FALSE(m_pAssist->problem().isEmpty());
    EXPECT_FALSE(m_pAssist->canExecute());
}

// The assistant in the library pane shows the plan of the loaded decks: its
// mode, entry, tempo, bass swap, gain and planner, and what the DJ can do.
TEST_F(CrateSmartMixAssistTest, ShowsPlanForLoadedDecks) {
    crate::AssistPanel panel;
    EXPECT_EQ(QStringLiteral("No Crate server"), panel.statusText());
    EXPECT_FALSE(panel.executeButton()->isEnabled());

    loadDecks(120000.0);
    set(kDeck1, QStringLiteral("bpm"), 124.0);
    makeAssist();
    panel.setAssist(m_pAssist.get());
    ASSERT_TRUE(waitFor([this] { return !planRequests().isEmpty(); }));
    EXPECT_EQ(QStringLiteral("Asking Crate"), panel.statusText());
    answerPlans(QStringLiteral("synthetic-assist-long-blend.json"));
    ASSERT_TRUE(waitFor([&] { return m_pAssist->status() == Status::Ready; }));

    EXPECT_TRUE(panel.statusText().startsWith(QStringLiteral("Plan ready")));
    EXPECT_EQ(QStringLiteral("outgoing.mp3 → incoming.mp3"), panel.titleText());
    EXPECT_EQ(QStringLiteral("Beatmatch"), panel.modeText());
    EXPECT_EQ(QStringLiteral("Ready"), panel.stateText());
    EXPECT_EQ(QStringLiteral("beatmatch · long blend · bass handoff"),
            panel.metadata(QStringLiteral("Mode")));
    EXPECT_EQ(QStringLiteral("A 5:30.3 → B 0:00.5"), panel.metadata(QStringLiteral("Entry")));
    EXPECT_EQ(QStringLiteral("B ×1.0234 · +2.34 %"), panel.metadata(QStringLiteral("Tempo")));
    EXPECT_EQ(QStringLiteral("at 50 % · 0:48 in"), panel.metadata(QStringLiteral("Bass swap")));
    EXPECT_EQ(QStringLiteral("A \u22124.3 dB · B \u22123.8 dB"),
            panel.metadata(QStringLiteral("Gain")));
    EXPECT_EQ(QStringLiteral("smart-mix-v3 · assist · phrases"),
            panel.metadata(QStringLiteral("Planner")));
    EXPECT_EQ(QStringLiteral("0.68"), panel.readingText(QStringLiteral("Confidence")));
    EXPECT_EQ(QStringLiteral("1:36"), panel.readingText(QStringLiteral("Length")));
    EXPECT_EQ(QStringLiteral("0:32.2"), panel.readingText(QStringLiteral("Entry in")));
    EXPECT_TRUE(panel.prepareButton()->isEnabled());
    EXPECT_TRUE(panel.executeButton()->isEnabled());
    EXPECT_FALSE(panel.cancelButton()->isEnabled());
}

// The plan's timeline under the host (plan V3.PR1b): there while there is a
// plan, laid out from the decks where the director plays the plan, its
// playhead following the outgoing deck and its tempo the decks'; gone
// without a plan.
TEST_F(CrateSmartMixAssistTest, ShowsThePlanTimeline) {
    using Side = crate::widgets::TransitionPlanViewModel::Side;
    crate::AssistPanel panel;
    EXPECT_FALSE(panel.planView()->isVisibleTo(&panel));
    loadDecks(120000.0);
    makeAssist();
    panel.setAssist(m_pAssist.get());
    ASSERT_TRUE(waitFor([this] { return !planRequests().isEmpty(); }));
    EXPECT_FALSE(panel.planView()->isVisibleTo(&panel));
    answerPlans(QStringLiteral("synthetic-assist-long-blend.json"));
    ASSERT_TRUE(waitFor([&] { return m_pAssist->status() == Status::Ready; }));

    EXPECT_TRUE(panel.planView()->isVisibleTo(&panel));
    const crate::widgets::TransitionPlanViewModel* pModel = panel.planModel();
    ASSERT_TRUE(pModel->hasPlan());
    EXPECT_DOUBLE_EQ(330298.0, pModel->windowStartMs());
    EXPECT_DOUBLE_EQ(330298.0 + 96000.0, pModel->windowEndMs());
    EXPECT_EQ(QStringLiteral("A"), pModel->deckLetter(Side::Outgoing));
    EXPECT_EQ(QStringLiteral("B"), pModel->deckLetter(Side::Incoming));
    EXPECT_EQ(QStringLiteral("outgoing.mp3"), pModel->deck(Side::Outgoing).title);
    EXPECT_DOUBLE_EQ(kOutgoingSeconds * 1000.0, pModel->deck(Side::Outgoing).durationMs);
    EXPECT_NEAR(kCueMs - 120000.0, pModel->nowMs(), 1.0);
    // The decks' tracks have no grid yet: no bars
    EXPECT_FALSE(pModel->windowBars());

    // While the panel shows, the playhead follows the outgoing deck
    panel.resize(1400, 600);
    panel.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&panel));
    set(kDeck1, QStringLiteral("playposition"), (330298.0 - 5000.0) / (kOutgoingSeconds * 1000.0));
    ASSERT_TRUE(waitFor([&] { return std::abs(pModel->nowMs() - (330298.0 - 5000.0)) < 1.0; }));
    // and a deck's tempo lays it out again
    set(kDeck2, QStringLiteral("rate_ratio"), 1.02);
    ASSERT_TRUE(waitFor([&] { return pModel->deck(Side::Incoming).rate == 1.02; }));
    EXPECT_TRUE(pModel->hasPlan());

    panel.setAssist(nullptr);
    EXPECT_FALSE(panel.planView()->isVisibleTo(&panel));
    EXPECT_FALSE(pModel->hasPlan());
}

// The panel's buttons are the assistant's.
TEST_F(CrateSmartMixAssistTest, PanelButtonsDriveTheDirector) {
    readyPlan();
    crate::AssistPanel panel;
    panel.setAssist(m_pAssist.get());

    panel.prepareButton()->click();
    EXPECT_EQ(Status::Prepared, m_pAssist->status());
    EXPECT_EQ(QStringLiteral("Prepared"), panel.stateText());
    EXPECT_FALSE(panel.prepareButton()->isEnabled());
    EXPECT_TRUE(panel.cancelButton()->isEnabled());
    panel.executeButton()->click();
    EXPECT_EQ(Status::Executing, m_pAssist->status());
    EXPECT_EQ(QStringLiteral("Armed"), panel.stateText());
    panel.cancelButton()->click();
    EXPECT_EQ(State::Cancelled, m_pAssist->director()->state());
}

// With Auto DJ on the panel says so and its buttons wait; its chips switch
// between Auto DJ and the assistant.
TEST_F(CrateSmartMixAssistTest, PanelSaysAutoDJComesFirst) {
    readyPlan();
    crate::AssistPanel panel;
    panel.setAssist(m_pAssist.get());
    EXPECT_TRUE(panel.assistChip()->isChecked());

    panel.autoDjChip()->click();
    EXPECT_EQ(1.0, get(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled")));
    ASSERT_TRUE(waitFor([&] { return panel.statusText() == QStringLiteral("Auto DJ is mixing"); }));
    EXPECT_TRUE(panel.autoDjChip()->isChecked());
    EXPECT_EQ(QStringLiteral("Auto DJ"), panel.modeText());
    EXPECT_FALSE(panel.executeButton()->isEnabled());
    EXPECT_FALSE(panel.prepareButton()->isEnabled());

    panel.assistChip()->click();
    EXPECT_EQ(0.0, get(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled")));
    ASSERT_TRUE(waitFor([this] { return m_pAssist->status() == Status::Ready; }));
    EXPECT_TRUE(panel.executeButton()->isEnabled());
}

} // namespace
