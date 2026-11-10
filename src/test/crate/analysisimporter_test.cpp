#include "crate/analysis/analysisimporter.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "analyzer/analyzerbeats.h"
#include "analyzer/analyzertrack.h"
#include "audio/signalinfo.h"
#include "audio/streaminfo.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/beats.h"
#include "track/keyfactory.h"
#include "track/track.h"
#include "util/math.h"

namespace {

using crate::analysis::AnalysisImporter;
using crate::analysis::AppliedCues;
using crate::analysis::CrateBeatsImporter;
using crate::analysis::GridCheck;
using crate::analysis::ImportOutcome;
using crate::analysis::MixProfile;
using crate::analysis::Policy;
using mixxx::track::io::key::ChromaticKey;

// The fixtures are written by src/test/crate/data/mix-profiles/
// make_mix_profiles.py from the server's schema and serializer: faithful to
// what the server sends, but not dumps of a real library (see that script).
const QString kConstantFixture = QStringLiteral("full-constant-124.json");
const QString kDriftingFixture = QStringLiteral("full-drifting-128.json");
const QString kPartialFixture = QStringLiteral("partial.json");

const QString kAudioFile = QStringLiteral("id3-test-data/cover-test-øé~ł€˚.flac");

mixxx::audio::StreamInfo streamInfo(double sampleRate) {
    return mixxx::audio::StreamInfo(mixxx::audio::SignalInfo(
            mixxx::audio::ChannelCount::stereo(), mixxx::audio::SampleRate(sampleRate)));
}

Policy crateEverything() {
    Policy policy;
    policy.crateBeats = true;
    policy.crateKey = true;
    policy.gainFromLufs = true;
    return policy;
}

class AnalysisImporterTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    MixProfile fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/mix-profiles/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        const auto profile = crate::analysis::parseMixProfile(file.readAll());
        EXPECT_TRUE(profile.ok()) << name.toStdString();
        return profile.ok() ? profile.value() : MixProfile();
    }

    /// A track Mixxx has not opened: no sample rate from the decoder yet.
    TrackPointer unopenedTrack() const {
        const QString path = getTestDir().filePath(kAudioFile);
        EXPECT_TRUE(QFile::exists(path));
        return Track::newTemporary(path);
    }

    /// A track whose file Mixxx has opened, as after a first load.
    TrackPointer openedTrack() const {
        TrackPointer pTrack = unopenedTrack();
        SoundSourceProxy proxy(pTrack);
        EXPECT_TRUE(proxy.openAudioSource());
        return pTrack;
    }

    static void open(const TrackPointer& pTrack) {
        SoundSourceProxy proxy(pTrack);
        ASSERT_TRUE(proxy.openAudioSource());
    }

    static double framesPerMs(const TrackPointer& pTrack) {
        return pTrack->getSampleRate().toDouble() / 1000.0;
    }
};

TEST_F(AnalysisImporterTest, ImportsBeatGridAtFileSampleRate) {
    const MixProfile profile = fixture(kConstantFixture);
    const GridCheck grid = crate::analysis::checkBeatGrid(profile);
    ASSERT_TRUE(grid.passed) << grid.reason.toStdString();
    EXPECT_DOUBLE_EQ(124.0, grid.bpm);

    for (const double sampleRate : {44100.0, 48000.0}) {
        CrateBeatsImporter importer(grid, profile.profileRevision);
        const mixxx::BeatsPointer pBeats = importer.importBeatsAndApplyTimingOffset(
                QString(), QString(), streamInfo(sampleRate));
        ASSERT_NE(nullptr, pBeats) << sampleRate;
        EXPECT_EQ(mixxx::audio::SampleRate(sampleRate), pBeats->getSampleRate());
        EXPECT_TRUE(pBeats->hasConstantTempo());
        EXPECT_NEAR(124.0,
                pBeats->getBpmInRange(mixxx::audio::kStartFramePos,
                              mixxx::audio::FramePos(60 * sampleRate))
                        .value(),
                1e-6);
        EXPECT_EQ(QStringLiteral("crate-mix-profile-") + profile.profileRevision,
                pBeats->getSubVersion());
        // Every beat of Crate's grid, which is rounded to whole ms, is on
        // a beat of the imported grid.
        const double framesPerMs = sampleRate / 1000.0;
        for (const qint64 beatMs : profile.beatGridMs) {
            const auto position = mixxx::audio::FramePos(beatMs * framesPerMs);
            ASSERT_NEAR(position.value(), pBeats->findClosestBeat(position).value(), framesPerMs)
                    << beatMs << " ms at " << sampleRate;
        }
    }

    // A track Mixxx has not opened yet gets the grid when the file opens, at
    // the file's own sample rate.
    TrackPointer pTrack = unopenedTrack();
    const ImportOutcome outcome = AnalysisImporter::apply(pTrack, profile, crateEverything());
    EXPECT_TRUE(outcome.beatsImported);
    EXPECT_EQ(Track::ImportStatus::Pending, pTrack->getBeatsImportStatus());
    open(pTrack);
    EXPECT_EQ(Track::ImportStatus::Complete, pTrack->getBeatsImportStatus());
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    ASSERT_NE(nullptr, pBeats);
    EXPECT_EQ(pTrack->getSampleRate(), pBeats->getSampleRate());
    const auto downbeat = mixxx::audio::FramePos(996 * framesPerMs(pTrack));
    EXPECT_NEAR(downbeat.value(), pBeats->findClosestBeat(downbeat).value(), framesPerMs(pTrack));
}

TEST_F(AnalysisImporterTest, LocksBpm) {
    const MixProfile profile = fixture(kConstantFixture);

    TrackPointer pTrack = openedTrack();
    const ImportOutcome outcome = AnalysisImporter::apply(pTrack, profile, crateEverything());

    EXPECT_TRUE(outcome.beatsImported);
    EXPECT_TRUE(pTrack->isBpmLocked());
    ASSERT_NE(nullptr, pTrack->getBeats());
    EXPECT_EQ(CrateBeatsImporter::subVersion(profile.profileRevision),
            pTrack->getBeats()->getSubVersion());
    EXPECT_NEAR(124.0, pTrack->getBpm(), 1e-6);

    // A new revision replaces the grid Decks locked.
    MixProfile newer = profile;
    newer.profileRevision = QStringLiteral("rev-2");
    newer.bpm = 125.0;
    newer.beatGridMs.clear();
    for (int i = 0; i < 64; ++i) {
        newer.beatGridMs.append(qRound64(500 + i * 480.0));
    }
    EXPECT_TRUE(AnalysisImporter::apply(pTrack, newer, crateEverything()).beatsImported);
    EXPECT_TRUE(pTrack->isBpmLocked());
    EXPECT_EQ(CrateBeatsImporter::subVersion(QStringLiteral("rev-2")),
            pTrack->getBeats()->getSubVersion());
}

TEST_F(AnalysisImporterTest, KeepsGridTheUserLocked) {
    TrackPointer pTrack = openedTrack();
    ASSERT_TRUE(pTrack->trySetBeats(mixxx::Beats::fromConstTempo(
            pTrack->getSampleRate(), mixxx::audio::kStartFramePos, mixxx::Bpm(100.0))));
    pTrack->setBpmLocked(true);

    const ImportOutcome outcome =
            AnalysisImporter::apply(pTrack, fixture(kConstantFixture), crateEverything());

    EXPECT_FALSE(outcome.beatsImported);
    EXPECT_FALSE(outcome.beatsSkipped.isEmpty());
    EXPECT_NEAR(100.0, pTrack->getBpm(), 1e-6);
}

TEST_F(AnalysisImporterTest, LeavesBeatsToMixxxByDefault) {
    TrackPointer pTrack = openedTrack();

    const ImportOutcome outcome =
            AnalysisImporter::apply(pTrack, fixture(kConstantFixture), Policy());

    EXPECT_TRUE(outcome.usedProfile);
    EXPECT_FALSE(outcome.beatsImported);
    EXPECT_FALSE(outcome.keySet);
    EXPECT_FALSE(outcome.gainSet);
    EXPECT_FALSE(pTrack->isBpmLocked());
    EXPECT_EQ(nullptr, pTrack->getBeats());
    EXPECT_EQ(mixxx::track::io::key::INVALID, pTrack->getKey());
    EXPECT_FALSE(pTrack->getReplayGain().hasRatio());
}

// TEST_GAP: the limits of checkBeatGrid() are not calibrated on real profiles
// of the current analyzer: the dev stack was not running and production is
// off limits. G3 checks on the waveform which grids pass.
TEST_F(AnalysisImporterTest, RejectsDriftingGrid) {
    const MixProfile profile = fixture(kDriftingFixture);
    const GridCheck grid = crate::analysis::checkBeatGrid(profile);
    EXPECT_FALSE(grid.passed);
    EXPECT_FALSE(grid.reason.isEmpty());

    // Whole BPM, but the beats wander around it.
    MixProfile wandering = profile;
    wandering.bpm = 128.0;
    EXPECT_FALSE(crate::analysis::checkBeatGrid(wandering).passed);

    // Whole BPM and a constant grid at another tempo: it drifts.
    MixProfile drifting = fixture(kConstantFixture);
    drifting.bpm = 124.5;
    EXPECT_FALSE(crate::analysis::checkBeatGrid(drifting).passed);

    TrackPointer pTrack = openedTrack();
    const ImportOutcome outcome = AnalysisImporter::apply(pTrack, profile, crateEverything());
    EXPECT_FALSE(outcome.beatsImported);
    EXPECT_FALSE(outcome.beatsSkipped.isEmpty());
    EXPECT_FALSE(pTrack->isBpmLocked());
    EXPECT_EQ(nullptr, pTrack->getBeats());
}

TEST_F(AnalysisImporterTest, SetsKeyFromCamelot) {
    using namespace mixxx::track::io::key;
    const QList<std::pair<QString, ChromaticKey>> wheel = {
            {QStringLiteral("1A"), G_SHARP_MINOR},
            {QStringLiteral("2A"), E_FLAT_MINOR},
            {QStringLiteral("3A"), B_FLAT_MINOR},
            {QStringLiteral("4A"), F_MINOR},
            {QStringLiteral("5A"), C_MINOR},
            {QStringLiteral("6A"), G_MINOR},
            {QStringLiteral("7A"), D_MINOR},
            {QStringLiteral("8A"), A_MINOR},
            {QStringLiteral("9A"), E_MINOR},
            {QStringLiteral("10A"), B_MINOR},
            {QStringLiteral("11A"), F_SHARP_MINOR},
            {QStringLiteral("12A"), C_SHARP_MINOR},
            {QStringLiteral("1B"), B_MAJOR},
            {QStringLiteral("2B"), F_SHARP_MAJOR},
            {QStringLiteral("3B"), D_FLAT_MAJOR},
            {QStringLiteral("4B"), A_FLAT_MAJOR},
            {QStringLiteral("5B"), E_FLAT_MAJOR},
            {QStringLiteral("6B"), B_FLAT_MAJOR},
            {QStringLiteral("7B"), F_MAJOR},
            {QStringLiteral("8B"), C_MAJOR},
            {QStringLiteral("9B"), G_MAJOR},
            {QStringLiteral("10B"), D_MAJOR},
            {QStringLiteral("11B"), A_MAJOR},
            {QStringLiteral("12B"), E_MAJOR},
    };
    for (const auto& [camelot, key] : wheel) {
        EXPECT_EQ(key, crate::analysis::keyFromCamelot(camelot)) << camelot.toStdString();
        EXPECT_EQ(key, crate::analysis::keyFromCamelot(camelot.toLower() + QChar(' ')))
                << camelot.toStdString();
    }
    for (const QString& invalid : {QString(),
                 QStringLiteral("0A"),
                 QStringLiteral("13B"),
                 QStringLiteral("8C"),
                 QStringLiteral("Am")}) {
        EXPECT_EQ(INVALID, crate::analysis::keyFromCamelot(invalid)) << invalid.toStdString();
    }

    Policy policy;
    policy.crateKey = true;
    for (const auto& [camelot, key] : wheel) {
        MixProfile profile = fixture(kConstantFixture);
        profile.camelot = camelot;
        TrackPointer pTrack = unopenedTrack();
        EXPECT_TRUE(AnalysisImporter::apply(pTrack, profile, policy).keySet);
        EXPECT_EQ(key, pTrack->getKey()) << camelot.toStdString();
    }
}

TEST_F(AnalysisImporterTest, CreatesIntroOutroCues) {
    const MixProfile profile = fixture(kConstantFixture);
    TrackPointer pTrack = openedTrack();
    const auto mainCue = pTrack->getMainCuePosition();

    const ImportOutcome outcome = AnalysisImporter::apply(pTrack, profile, Policy());

    const double perMs = framesPerMs(pTrack);
    const CuePointer pIntro = pTrack->findCueByType(mixxx::CueType::Intro);
    const CuePointer pOutro = pTrack->findCueByType(mixxx::CueType::Outro);
    ASSERT_NE(nullptr, pIntro);
    ASSERT_NE(nullptr, pOutro);
    EXPECT_NEAR(475 * perMs, pIntro->getPosition().value(), 1.0);
    EXPECT_NEAR(31964 * perMs, pIntro->getEndPosition().value(), 1.0);
    EXPECT_NEAR(165802 * perMs, pOutro->getPosition().value(), 1.0);
    EXPECT_NEAR(196350 * perMs, pOutro->getEndPosition().value(), 1.0);
    // activeEndMs is a musical end, not Mixxx's −60 dB end of sound: Mixxx's
    // silence analysis sets N60dBSound, and the main cue stays Mixxx's.
    EXPECT_EQ(nullptr, pTrack->findCueByType(mixxx::CueType::N60dBSound));
    EXPECT_EQ(mainCue, pTrack->getMainCuePosition());

    ASSERT_TRUE(outcome.appliedCues.has_value());
    EXPECT_EQ(profile.profileRevision, outcome.appliedCues->profileRevision);
    EXPECT_EQ((AppliedCues::Range{475.0, 31964.0}), outcome.appliedCues->intro);
    EXPECT_EQ((AppliedCues::Range{165802.0, 196350.0}), outcome.appliedCues->outro);

    // A track Mixxx has not opened gets them when the file opens.
    TrackPointer pUnopened = unopenedTrack();
    AnalysisImporter::apply(pUnopened, profile, Policy());
    EXPECT_EQ(Track::ImportStatus::Pending, pUnopened->getCueImportStatus());
    open(pUnopened);
    ASSERT_NE(nullptr, pUnopened->findCueByType(mixxx::CueType::Intro));
    EXPECT_NEAR(475 * framesPerMs(pUnopened),
            pUnopened->findCueByType(mixxx::CueType::Intro)->getPosition().value(),
            1.0);
    ASSERT_NE(nullptr, pUnopened->findCueByType(mixxx::CueType::Outro));
}

TEST_F(AnalysisImporterTest, DoesNotOverwriteUserCues) {
    const MixProfile profile = fixture(kConstantFixture);
    TrackPointer pTrack = openedTrack();
    const double perMs = framesPerMs(pTrack);
    pTrack->createAndAddCue(mixxx::CueType::Intro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(2000 * perMs),
            mixxx::audio::FramePos(20000 * perMs));
    const CuePointer pHotcue = pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(5000 * perMs),
            mixxx::audio::kInvalidFramePos);

    // No record: the Intro is the user's. The Outro is missing, so Crate's
    // goes in.
    ImportOutcome outcome = AnalysisImporter::apply(pTrack, profile, Policy());
    EXPECT_NEAR(2000 * perMs, pTrack->findCueByType(mixxx::CueType::Intro)->getPosition().value(), 1.0);
    ASSERT_NE(nullptr, pTrack->findCueByType(mixxx::CueType::Outro));
    EXPECT_NEAR(165802 * perMs,
            pTrack->findCueByType(mixxx::CueType::Outro)->getPosition().value(),
            1.0);
    EXPECT_EQ(pHotcue, pTrack->findHotcueByIndex(0));
    ASSERT_TRUE(outcome.appliedCues.has_value());
    EXPECT_FALSE(outcome.appliedCues->intro.has_value());
    ASSERT_TRUE(outcome.appliedCues->outro.has_value());

    // The user moves Crate's Outro: a new revision leaves it alone.
    pTrack->findCueByType(mixxx::CueType::Outro)
            ->setStartPosition(mixxx::audio::FramePos(150000 * perMs));
    MixProfile newer = profile;
    newer.profileRevision = QStringLiteral("rev-2");
    newer.introCueMs = 30000;
    newer.outroCueMs = 160000;
    outcome = AnalysisImporter::apply(pTrack, newer, Policy(), outcome.appliedCues);
    EXPECT_NEAR(2000 * perMs, pTrack->findCueByType(mixxx::CueType::Intro)->getPosition().value(), 1.0);
    EXPECT_NEAR(150000 * perMs,
            pTrack->findCueByType(mixxx::CueType::Outro)->getPosition().value(),
            1.0);
    EXPECT_FALSE(outcome.appliedCues.has_value());
}

TEST_F(AnalysisImporterTest, ReappliesCratesCuesOnNewRevision) {
    const MixProfile profile = fixture(kConstantFixture);
    TrackPointer pTrack = openedTrack();
    const double perMs = framesPerMs(pTrack);
    const ImportOutcome first = AnalysisImporter::apply(pTrack, profile, Policy());
    ASSERT_TRUE(first.appliedCues.has_value());

    // The same revision again changes nothing.
    EXPECT_FALSE(AnalysisImporter::apply(pTrack, profile, Policy(), first.appliedCues)
                    .appliedCues.has_value());

    MixProfile newer = profile;
    newer.profileRevision = QStringLiteral("rev-2");
    newer.activeStartMs = 600;
    newer.introCueMs = 30000;
    newer.outroCueMs = 160000;
    newer.activeEndMs = 195000;
    const ImportOutcome second = AnalysisImporter::apply(pTrack, newer, Policy(), first.appliedCues);

    const CuePointer pIntro = pTrack->findCueByType(mixxx::CueType::Intro);
    const CuePointer pOutro = pTrack->findCueByType(mixxx::CueType::Outro);
    ASSERT_NE(nullptr, pIntro);
    ASSERT_NE(nullptr, pOutro);
    EXPECT_NEAR(600 * perMs, pIntro->getPosition().value(), 1.0);
    EXPECT_NEAR(30000 * perMs, pIntro->getEndPosition().value(), 1.0);
    EXPECT_NEAR(160000 * perMs, pOutro->getPosition().value(), 1.0);
    EXPECT_NEAR(195000 * perMs, pOutro->getEndPosition().value(), 1.0);
    ASSERT_TRUE(second.appliedCues.has_value());
    EXPECT_EQ(QStringLiteral("rev-2"), second.appliedCues->profileRevision);
    EXPECT_EQ((AppliedCues::Range{600.0, 30000.0}), second.appliedCues->intro);
}

TEST_F(AnalysisImporterTest, PartialProfileLeavesTrackForLocalAnalysis) {
    TrackPointer pTrack = openedTrack();

    const ImportOutcome outcome =
            AnalysisImporter::apply(pTrack, fixture(kPartialFixture), crateEverything());

    EXPECT_FALSE(outcome.usedProfile);
    EXPECT_FALSE(outcome.beatsImported);
    EXPECT_FALSE(outcome.keySet);
    EXPECT_FALSE(outcome.gainSet);
    EXPECT_FALSE(outcome.appliedCues.has_value());
    EXPECT_FALSE(pTrack->isBpmLocked());
    EXPECT_EQ(nullptr, pTrack->getBeats());
    EXPECT_TRUE(pTrack->getCuePoints().isEmpty());
    EXPECT_FALSE(pTrack->getReplayGain().hasRatio());

    // Legacy and unavailable profiles too.
    for (const auto quality : {MixProfile::Quality::Legacy, MixProfile::Quality::Unavailable}) {
        MixProfile profile = fixture(kConstantFixture);
        profile.quality = quality;
        TrackPointer pOther = openedTrack();
        EXPECT_FALSE(AnalysisImporter::apply(pOther, profile, crateEverything()).usedProfile);
        EXPECT_TRUE(pOther->getCuePoints().isEmpty());
    }
}

TEST_F(AnalysisImporterTest, SetsGainFromLufsOnlyWhenAsked) {
    const MixProfile profile = fixture(kConstantFixture);
    Policy policy;
    policy.gainFromLufs = true;
    TrackPointer pTrack = openedTrack();

    EXPECT_TRUE(AnalysisImporter::apply(pTrack, profile, policy).gainSet);

    // Mixxx's ReplayGain 2.0 reference, −18 LUFS.
    ASSERT_TRUE(pTrack->getReplayGain().hasRatio());
    EXPECT_NEAR(db2ratio(-18.0 - -8.6), pTrack->getReplayGain().getRatio(), 1e-6);
}

TEST_F(AnalysisImporterTest, AnalyzerSkipsBeatsForCrateGrid) {
    const MixProfile profile = fixture(kConstantFixture);
    TrackPointer pTrack = openedTrack();
    AnalysisImporter::apply(pTrack, profile, crateEverything());
    ASSERT_TRUE(pTrack->isBpmLocked());

    AnalyzerBeats analyzer(config(), true);
    EXPECT_FALSE(analyzer.initialize(AnalyzerTrack(pTrack),
            pTrack->getSampleRate(),
            mixxx::audio::ChannelCount::stereo(),
            static_cast<SINT>(10 * pTrack->getSampleRate().toDouble())));

    // With Mixxx's beats (the default policy) the analyzer runs.
    TrackPointer pLocal = openedTrack();
    AnalysisImporter::apply(pLocal, profile, Policy());
    AnalyzerBeats localAnalyzer(config(), true);
    EXPECT_TRUE(localAnalyzer.initialize(AnalyzerTrack(pLocal),
            pLocal->getSampleRate(),
            mixxx::audio::ChannelCount::stereo(),
            static_cast<SINT>(10 * pLocal->getSampleRate().toDouble())));
    localAnalyzer.cleanup();
}

} // namespace
