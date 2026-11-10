#include "crate/analysis/analysisimporter.h"

#include <QHash>
#include <cmath>

#include "audio/streaminfo.h"
#include "track/beats.h"
#include "track/cueinfo.h"
#include "track/cueinfoimporter.h"
#include "track/keyfactory.h"
#include "track/replaygain.h"
#include "track/track.h"
#include "util/logger.h"
#include "util/math.h"

namespace crate::analysis {

namespace {

const mixxx::Logger kLogger("crate::analysis::AnalysisImporter");

const QString kSubVersionPrefix = QStringLiteral("crate-mix-profile-");

/// Mixxx's ReplayGain 2.0 reference (AnalyzerEbur128).
constexpr double kReplayGainReferenceLufs = -18.0;

/// A cue Decks set counts as unedited while it stays this close to where it
/// was set; Mixxx keeps cue positions in fractional frames.
constexpr double kCueToleranceMs = 1.0;

/// A BPM within this of a whole or half BPM is quantised.
constexpr double kQuantisedBpmTolerance = 0.001;

GridCheck failed(GridCheck check, const QString& reason) {
    check.passed = false;
    check.reason = reason;
    return check;
}

std::optional<double> msOf(mixxx::audio::FramePos position, mixxx::audio::SampleRate sampleRate) {
    if (!position.isValid() || !sampleRate.isValid()) {
        return std::nullopt;
    }
    return position.value() * 1000.0 / sampleRate.toDouble();
}

bool closeTo(const std::optional<double>& actual, const std::optional<double>& expected) {
    if (!actual || !expected) {
        return actual.has_value() == expected.has_value();
    }
    return std::abs(*actual - *expected) <= kCueToleranceMs;
}

/// Whether the track's cue is the one `range` records.
bool matches(const CuePointer& pCue,
        const std::optional<AppliedCues::Range>& range,
        mixxx::audio::SampleRate sampleRate) {
    if (!pCue || !range) {
        return false;
    }
    return closeTo(msOf(pCue->getPosition(), sampleRate), range->startMs) &&
            closeTo(msOf(pCue->getEndPosition(), sampleRate), range->endMs);
}

/// Intro from the musical start to the intro cue; Outro from the outro cue
/// to the musical end.
std::optional<AppliedCues::Range> introOf(const MixProfile& profile) {
    const double startMs = static_cast<double>(profile.activeStartMs.value_or(0));
    std::optional<double> endMs;
    if (profile.introCueMs && *profile.introCueMs > startMs) {
        endMs = static_cast<double>(*profile.introCueMs);
    }
    if (!profile.activeStartMs && !endMs) {
        return std::nullopt;
    }
    return AppliedCues::Range{startMs, endMs};
}

std::optional<AppliedCues::Range> outroOf(const MixProfile& profile) {
    std::optional<double> endMs;
    if (profile.activeEndMs) {
        endMs = static_cast<double>(*profile.activeEndMs);
    }
    std::optional<double> startMs;
    if (profile.outroCueMs && (!endMs || *profile.outroCueMs < *endMs)) {
        startMs = static_cast<double>(*profile.outroCueMs);
    }
    if (!startMs && !endMs) {
        return std::nullopt;
    }
    return AppliedCues::Range{startMs, endMs};
}

mixxx::CueInfo cueInfoOf(mixxx::CueType type, const AppliedCues::Range& range) {
    return mixxx::CueInfo(type, range.startMs, range.endMs, std::nullopt, QString(), std::nullopt);
}

/// Imports Crate's beats (point 2). Returns why it did not, if it did not.
QString importBeats(const TrackPointer& pTrack, const MixProfile& profile) {
    const GridCheck grid = checkBeatGrid(profile);
    if (!grid.passed) {
        return grid.reason;
    }
    if (pTrack->getBeatsImportStatus() == Track::ImportStatus::Pending) {
        return QStringLiteral("An import of beats is already pending");
    }
    const QString subVersion = CrateBeatsImporter::subVersion(profile.profileRevision);
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    const bool cratesGrid = pBeats && pBeats->getSubVersion().startsWith(kSubVersionPrefix);
    if (pBeats && pBeats->getSubVersion() == subVersion && pTrack->isBpmLocked()) {
        return QStringLiteral("The grid of this revision is already there");
    }
    if (pTrack->isBpmLocked()) {
        if (!cratesGrid) {
            return QStringLiteral("The user locked the BPM");
        }
        // Decks locked it for an older revision: the new one replaces it.
        pTrack->setBpmLocked(false);
    }
    pTrack->tryImportBeats(
            std::make_shared<CrateBeatsImporter>(grid, profile.profileRevision), true);
    return QString();
}

/// Places Intro and Outro (point 4). Returns what it placed, if anything.
std::optional<AppliedCues> importCues(const TrackPointer& pTrack,
        const MixProfile& profile,
        const std::optional<AppliedCues>& previous) {
    if (pTrack->getCueImportStatus() == Track::ImportStatus::Pending) {
        kLogger.info() << "An import of cues is already pending for" << profile.entityUid;
        return std::nullopt;
    }
    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    const bool newRevision = !previous || previous->profileRevision != profile.profileRevision;

    AppliedCues applied;
    applied.profileRevision = profile.profileRevision;
    QList<mixxx::CueInfo> cueInfos;
    const auto place = [&](mixxx::CueType type,
                               const std::optional<AppliedCues::Range>& range,
                               const std::optional<AppliedCues::Range>& previousRange,
                               std::optional<AppliedCues::Range>* pApplied) {
        if (!range) {
            return;
        }
        const CuePointer pCue = pTrack->findCueByType(type);
        // Missing, or Decks' own from an older revision and not moved since.
        const bool replace = !pCue ||
                (newRevision && previous && matches(pCue, previousRange, sampleRate));
        if (!replace) {
            return;
        }
        if (pCue) {
            pTrack->removeCue(pCue);
        }
        cueInfos.append(cueInfoOf(type, *range));
        *pApplied = range;
    };
    place(mixxx::CueType::Intro,
            introOf(profile),
            previous ? previous->intro : std::nullopt,
            &applied.intro);
    place(mixxx::CueType::Outro,
            outroOf(profile),
            previous ? previous->outro : std::nullopt,
            &applied.outro);
    if (cueInfos.isEmpty()) {
        return std::nullopt;
    }
    // Mixxx skips an import that has a type of an existing cue; the ones
    // replaced are gone by now, and the rest are of other types.
    pTrack->importCueInfos(std::make_unique<mixxx::CueInfoImporter>(cueInfos));
    return applied;
}

} // namespace

GridCheck checkBeatGrid(const MixProfile& profile) {
    GridCheck check;
    if (!profile.bpm || *profile.bpm <= 0.0) {
        return failed(check, QStringLiteral("No BPM"));
    }
    const double bpm = std::round(*profile.bpm * 2.0) / 2.0;
    check.bpm = bpm;
    if (bpm <= 0.0 || std::abs(*profile.bpm - bpm) > kQuantisedBpmTolerance) {
        return failed(check, QStringLiteral("The BPM %1 is not quantised").arg(*profile.bpm));
    }
    const QVector<qint64>& beats = profile.beatGridMs;
    if (beats.size() < kGridMinBeats) {
        return failed(check, QStringLiteral("Only %1 beats").arg(beats.size()));
    }

    // Each beat's index on a constant grid at the quantised BPM, and the
    // phase that fits them best.
    const double period = 60000.0 / bpm;
    const double origin = static_cast<double>(beats.first());
    QVector<double> indexes;
    indexes.reserve(beats.size());
    double phaseSum = 0.0;
    for (const qint64 beat : beats) {
        const double index = std::round((static_cast<double>(beat) - origin) / period);
        if (!indexes.isEmpty() && index <= indexes.last()) {
            return failed(check, QStringLiteral("Two beats are less than a beat apart"));
        }
        indexes.append(index);
        phaseSum += static_cast<double>(beat) - index * period;
    }
    const double phase = phaseSum / beats.size();

    // The distance of each beat from the grid, and its trend from start to
    // end: a BPM slightly off moves the beats away steadily.
    double meanIndex = 0.0;
    double meanDeviation = 0.0;
    QVector<double> deviations;
    deviations.reserve(beats.size());
    for (int i = 0; i < beats.size(); ++i) {
        const double deviation = static_cast<double>(beats[i]) - phase - indexes[i] * period;
        deviations.append(deviation);
        check.maxDeviationMs = std::max(check.maxDeviationMs, std::abs(deviation));
        meanIndex += indexes[i];
        meanDeviation += deviation;
    }
    meanIndex /= beats.size();
    meanDeviation /= beats.size();
    double covariance = 0.0;
    double variance = 0.0;
    for (int i = 0; i < beats.size(); ++i) {
        covariance += (indexes[i] - meanIndex) * (deviations[i] - meanDeviation);
        variance += (indexes[i] - meanIndex) * (indexes[i] - meanIndex);
    }
    const double slope = variance > 0.0 ? covariance / variance : 0.0;
    check.driftMs = slope * (indexes.last() - indexes.first());
    if (check.maxDeviationMs > kGridMaxDeviationMs) {
        return failed(check,
                QStringLiteral("A beat is %1 ms off a constant grid").arg(check.maxDeviationMs));
    }
    if (std::abs(check.driftMs) > kGridMaxDriftMs) {
        return failed(check, QStringLiteral("The beats drift %1 ms").arg(check.driftMs));
    }

    // Mixxx places a constant grid's marker on its first downbeat.
    double firstBeatMs = phase - std::floor(phase / period) * period;
    if (profile.downbeatAnchorMs) {
        const double downbeat = static_cast<double>(*profile.downbeatAnchorMs);
        const double onGrid = phase + std::round((downbeat - phase) / period) * period;
        if (std::abs(downbeat - onGrid) <= kGridMaxDeviationMs && onGrid >= 0.0) {
            const double bar = period * profile.timeSignature.value_or(4);
            firstBeatMs = onGrid - std::floor(onGrid / bar) * bar;
        }
    }
    check.firstBeatMs = firstBeatMs;
    check.passed = true;
    return check;
}

CrateBeatsImporter::CrateBeatsImporter(GridCheck grid, QString profileRevision)
        : m_grid(std::move(grid)),
          m_profileRevision(std::move(profileRevision)) {
}

bool CrateBeatsImporter::isEmpty() const {
    // Mixxx expects an importer to be empty once it has imported.
    return m_imported || !m_grid.passed;
}

mixxx::BeatsPointer CrateBeatsImporter::importBeatsAndApplyTimingOffset(const QString& filePath,
        const QString& fileType,
        const mixxx::audio::StreamInfo& streamInfo) {
    Q_UNUSED(fileType);
    const mixxx::audio::SampleRate sampleRate = streamInfo.getSignalInfo().getSampleRate();
    if (isEmpty() || !sampleRate.isValid()) {
        return nullptr;
    }
    m_imported = true;
    const auto firstBeat = mixxx::audio::FramePos(m_grid.firstBeatMs * sampleRate.toDouble() / 1000.0);
    kLogger.debug() << "Crate grid for" << filePath << "at" << m_grid.bpm << "BPM from"
                    << m_grid.firstBeatMs << "ms," << sampleRate;
    return mixxx::Beats::fromConstTempo(
            sampleRate, firstBeat, mixxx::Bpm(m_grid.bpm), subVersion(m_profileRevision));
}

QString CrateBeatsImporter::subVersion(const QString& profileRevision) {
    return kSubVersionPrefix + profileRevision;
}

mixxx::track::io::key::ChromaticKey keyFromCamelot(const QString& camelot) {
    using namespace mixxx::track::io::key;
    // The Camelot wheel, as the server's camelot.py writes it.
    static const QHash<QString, ChromaticKey> kWheel = {
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
    return kWheel.value(camelot.trimmed().toUpper(), INVALID);
}

ImportOutcome AnalysisImporter::apply(const TrackPointer& pTrack,
        const MixProfile& profile,
        const Policy& policy,
        const std::optional<AppliedCues>& previous) {
    ImportOutcome outcome;
    VERIFY_OR_DEBUG_ASSERT(pTrack) {
        return outcome;
    }
    if (!profile.isFull()) {
        // Design D6: Mixxx analyses the track itself.
        return outcome;
    }
    outcome.usedProfile = true;

    if (policy.crateBeats) {
        outcome.beatsSkipped = importBeats(pTrack, profile);
        outcome.beatsImported = outcome.beatsSkipped.isEmpty();
        if (!outcome.beatsImported) {
            kLogger.info() << "Mixxx analyses the beats of" << profile.entityUid << ":"
                           << outcome.beatsSkipped;
        }
    }

    if (policy.crateKey) {
        const mixxx::track::io::key::ChromaticKey key = keyFromCamelot(profile.camelot);
        if (key != mixxx::track::io::key::INVALID) {
            pTrack->setKeys(KeyFactory::makeBasicKeys(key, mixxx::track::io::key::FILE_METADATA));
            outcome.keySet = true;
        }
    }

    outcome.appliedCues = importCues(pTrack, profile, previous);

    if (policy.gainFromLufs && profile.integratedLufs &&
            !pTrack->getReplayGain().hasRatio()) {
        mixxx::ReplayGain replayGain = pTrack->getReplayGain();
        replayGain.setRatio(db2ratio(kReplayGainReferenceLufs - *profile.integratedLufs));
        if (profile.truePeakDbfs) {
            replayGain.setPeak(static_cast<CSAMPLE>(db2ratio(*profile.truePeakDbfs)));
        }
        pTrack->setReplayGain(replayGain);
        outcome.gainSet = true;
    }
    return outcome;
}

} // namespace crate::analysis
