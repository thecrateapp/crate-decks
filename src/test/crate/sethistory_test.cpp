#include "crate/smartmix/sethistory.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "test/mixxxtest.h"

namespace {

using crate::SetHistory;
using crate::SetHistoryEntry;
using crate::TransitionPlan;
using crate::analysis::MixProfile;

const QString kOutgoing = QStringLiteral("00000000-0000-4000-8000-000000000001");
const QString kIncoming = QStringLiteral("00000000-0000-4000-8000-000000000002");

MixProfile profile(const QString& uid,
        std::optional<double> bpm,
        std::optional<double> introEnergy,
        std::optional<double> outroEnergy) {
    MixProfile profile;
    profile.entityUid = uid;
    profile.bpm = bpm;
    profile.introEnergy = introEnergy;
    profile.outroEnergy = outroEnergy;
    return profile;
}

TransitionPlan played(TransitionPlan::Mode mode) {
    TransitionPlan plan;
    plan.outgoingTrackEntityUid = kOutgoing;
    plan.incomingTrackEntityUid = kIncoming;
    plan.mode = mode;
    return plan;
}

class CrateSetHistoryTest : public MixxxTest {};

// The energies of a real full profile (the dev stack's, anonymised).
TEST_F(CrateSetHistoryTest, ReadsTheEnergiesOfAProfile) {
    QFile file(getTestDir().filePath(
            QStringLiteral("crate/data/mix-profiles/real-full-tracked.json")));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto parsed = crate::analysis::parseMixProfile(file.readAll());
    ASSERT_TRUE(parsed.ok()) << parsed.error().message.toStdString();
    EXPECT_EQ(std::optional<double>(0.7411461), parsed.value().introEnergy);
    EXPECT_EQ(std::optional<double>(0.388746), parsed.value().outroEnergy);
}

// Bars at the outgoing tempo and meter; the energy step from the outgoing
// outro to the incoming intro, rounded and within the server's bounds.
TEST_F(CrateSetHistoryTest, CountsBarsAndTheEnergyStep) {
    const auto entry = SetHistory::entryOf(played(TransitionPlan::Mode::Beatmatch),
            31219.0,
            profile(kOutgoing, 123.0, 0.2, 0.8),
            profile(kIncoming, 123.0, 0.7, 0.1));
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ((SetHistoryEntry{TransitionPlan::Mode::Beatmatch, 16, -0.1}), *entry);
    EXPECT_EQ((QJsonObject{{QStringLiteral("mode"), QStringLiteral("beatmatch")},
                      {QStringLiteral("durationBars"), 16},
                      {QStringLiteral("deltaEnergy"), -0.1}}),
            entry->toJson());

    // Three beats to the bar.
    MixProfile waltz = profile(kOutgoing, 120.0, 0.5, 0.5);
    waltz.timeSignature = 3;
    EXPECT_EQ(4,
            SetHistory::entryOf(played(TransitionPlan::Mode::Adaptive),
                    6000.0,
                    waltz,
                    profile(kIncoming, 120.0, 0.5, 0.5))
                    ->durationBars);
    // A long assist blend stays within 128 bars.
    EXPECT_EQ(128,
            SetHistory::entryOf(played(TransitionPlan::Mode::Beatmatch),
                    600000.0,
                    profile(kOutgoing, 174.0, 0.5, 0.5),
                    profile(kIncoming, 174.0, 0.5, 0.5))
                    ->durationBars);
}

// Without the outgoing tempo or either energy there is nothing to send.
TEST_F(CrateSetHistoryTest, SkipsTransitionsWithoutTheNumbers) {
    const TransitionPlan plan = played(TransitionPlan::Mode::Adaptive);
    EXPECT_FALSE(SetHistory::entryOf(plan,
            8000.0,
            profile(kOutgoing, std::nullopt, 0.5, 0.5),
            profile(kIncoming, 120.0, 0.5, 0.5))
                    .has_value());
    EXPECT_FALSE(SetHistory::entryOf(plan,
            8000.0,
            profile(kOutgoing, 120.0, 0.5, std::nullopt),
            profile(kIncoming, 120.0, 0.5, 0.5))
                    .has_value());
    EXPECT_FALSE(SetHistory::entryOf(plan,
            8000.0,
            profile(kOutgoing, 120.0, 0.5, 0.5),
            profile(kIncoming, 120.0, std::nullopt, 0.5))
                    .has_value());

    SetHistory history;
    history.record(plan,
            8000.0,
            profile(kOutgoing, std::nullopt, 0.5, 0.5),
            profile(kIncoming, 120.0, 0.5, 0.5));
    EXPECT_TRUE(history.entries().isEmpty());
}

// prepare() asks the server for the profiles of a transition's tracks, so
// that record() has them once the director played it.
TEST_F(CrateSetHistoryTest, RecordsWithTheProfilesItPrepared) {
    QString askedServer;
    QStringList askedUids;
    SetHistory::Options options;
    options.profilesOf = [&](const QString& serverId,
                                 const QStringList& uids,
                                 const SetHistory::Options::ProfilesDone& done) {
        askedServer = serverId;
        askedUids = uids;
        done({profile(kOutgoing, 128.0, 0.4, 0.6), profile(kIncoming, 126.0, 0.8, 0.3)});
    };
    SetHistory history(options);
    // Not prepared: no profiles, not recorded.
    history.record(played(TransitionPlan::Mode::Beatmatch), 15000.0);
    EXPECT_TRUE(history.entries().isEmpty());

    history.prepare(QStringLiteral("server-1"), kOutgoing.toUpper(), kIncoming);
    EXPECT_EQ(QStringLiteral("server-1"), askedServer);
    EXPECT_EQ((QStringList{kOutgoing, kIncoming}), askedUids);
    // 15 s at 128 BPM: 32 beats, 8 bars; from 0.6 to 0.8.
    for (int count = 0; count < 4; ++count) {
        history.record(played(TransitionPlan::Mode::Beatmatch), 15000.0);
    }
    ASSERT_EQ(SetHistory::kSize, history.entries().size());
    EXPECT_EQ((SetHistoryEntry{TransitionPlan::Mode::Beatmatch, 8, 0.2}),
            history.entries().constLast());
}

} // namespace
