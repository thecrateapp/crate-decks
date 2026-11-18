#include "crate/writeback/cratecorrections.h"

#include <QJsonArray>
#include <cmath>

#include "crate/analysis/analysisimporter.h"
#include "crate/writeback/outbox.h"
#include "track/keyfactory.h"
#include "track/track.h"
#include "util/logger.h"

namespace crate::writeback {

namespace {

const mixxx::Logger kLogger("crate::writeback::CrateCorrections");

const QString kBpmKey = QStringLiteral("bpm");
const QString kGridFormatKey = QStringLiteral("beat_grid_format");
const QString kGridKey = QStringLiteral("beat_grid_ms");
const QString kKeyKey = QStringLiteral("key");

constexpr double kMinBpm = 30.0;
constexpr double kMaxBpm = 300.0;
/// The server's limits of a grid (encode_beat_grid).
constexpr int kMaxBeats = 100000;
constexpr qint64 kMaxGridMs = 86400000;
/// A grid is constant when no beat is further than this from one.
constexpr double kConstantGridToleranceMs = 2.0;

bool validBpm(double bpm) {
    return std::isfinite(bpm) && bpm >= kMinBpm && bpm <= kMaxBpm;
}

bool validGrid(const QVector<qint64>& grid) {
    if (grid.size() < 2 || grid.size() > kMaxBeats || grid.first() <= 0 ||
            grid.last() > kMaxGridMs) {
        return false;
    }
    for (int i = 1; i < grid.size(); ++i) {
        if (grid[i] <= grid[i - 1]) {
            return false;
        }
    }
    return true;
}

mixxx::audio::FramePos framesOf(double ms, mixxx::audio::SampleRate sampleRate) {
    return mixxx::audio::FramePos(ms * sampleRate.toDouble() / 1000.0);
}

} // namespace

Correction correctionFromJson(const QJsonObject& json) {
    Correction correction;
    const QJsonValue bpm = json.value(kBpmKey);
    if (bpm.isDouble() && validBpm(bpm.toDouble())) {
        correction.bpm = bpm.toDouble();
    }
    if (json.value(kGridFormatKey).toString() == kBeatGridFormat) {
        const QJsonArray array = json.value(kGridKey).toArray();
        QVector<qint64> grid;
        grid.reserve(array.size());
        for (const QJsonValue& beat : array) {
            grid.append(beat.toInteger(-1));
        }
        if (validGrid(grid)) {
            correction.beatGridMs = grid;
        }
    }
    const QString key = json.value(kKeyKey).toString().trimmed().toUpper();
    if (analysis::keyFromCamelot(key) != mixxx::track::io::key::INVALID) {
        correction.key = key;
    }
    return correction;
}

QJsonObject toJson(const Correction& correction) {
    QJsonObject json{
            {kBpmKey, correction.bpm ? QJsonValue(*correction.bpm) : QJsonValue()},
            {kGridFormatKey, QJsonValue()},
            {kGridKey, QJsonValue()},
            {kKeyKey, correction.key.isEmpty() ? QJsonValue() : QJsonValue(correction.key)},
    };
    if (!correction.beatGridMs.isEmpty()) {
        QJsonArray grid;
        for (const qint64 beat : correction.beatGridMs) {
            grid.append(beat);
        }
        json.insert(kGridFormatKey, kBeatGridFormat);
        json.insert(kGridKey, grid);
    }
    return json;
}

QVector<qint64> beatGridOf(const TrackPointer& pTrack) {
    QVector<qint64> grid;
    const mixxx::BeatsPointer pBeats = pTrack ? pTrack->getBeats() : nullptr;
    if (!pBeats || !pBeats->getSampleRate().isValid()) {
        return grid;
    }
    const double durationMs = pTrack->getDuration() * 1000.0;
    if (durationMs <= 0.0) {
        return grid;
    }
    const double lastMs = std::min(durationMs, static_cast<double>(kMaxGridMs));
    const double framesPerMs = pBeats->getSampleRate().toDouble() / 1000.0;
    auto it = pBeats->iteratorFrom(mixxx::audio::kStartFramePos);
    for (int i = 0; i < kMaxBeats * 2 && grid.size() < kMaxBeats; ++i, ++it) {
        const double ms = it->value() / framesPerMs;
        if (ms > lastMs) {
            break;
        }
        const qint64 rounded = std::llround(ms);
        if (rounded >= 1 && (grid.isEmpty() || rounded > grid.last())) {
            grid.append(rounded);
        }
    }
    if (grid.size() < 2) {
        grid.clear();
    }
    return grid;
}

mixxx::BeatsPointer beatsOfGrid(mixxx::audio::SampleRate sampleRate,
        const QVector<qint64>& beatGridMs,
        std::optional<double> bpm) {
    if (!sampleRate.isValid() || beatGridMs.size() < 2) {
        return nullptr;
    }
    const int count = static_cast<int>(beatGridMs.size());
    const double first = static_cast<double>(beatGridMs.first());
    double period = bpm ? 60000.0 / *bpm
                        : (static_cast<double>(beatGridMs.last()) - first) / (count - 1);
    // Each beat's index on a constant grid, and the phase that fits best
    QVector<double> indexes;
    indexes.reserve(count);
    bool constant = period > 0.0;
    for (int i = 0; constant && i < count; ++i) {
        const double index = std::round((static_cast<double>(beatGridMs[i]) - first) / period);
        constant = indexes.isEmpty() || index > indexes.last();
        indexes.append(index);
    }
    if (constant && !bpm) {
        // The least-squares period, finer than the whole milliseconds
        double meanIndex = 0.0;
        double meanBeat = 0.0;
        for (int i = 0; i < count; ++i) {
            meanIndex += indexes[i];
            meanBeat += static_cast<double>(beatGridMs[i]);
        }
        meanIndex /= count;
        meanBeat /= count;
        double covariance = 0.0;
        double variance = 0.0;
        for (int i = 0; i < count; ++i) {
            covariance += (indexes[i] - meanIndex) *
                    (static_cast<double>(beatGridMs[i]) - meanBeat);
            variance += (indexes[i] - meanIndex) * (indexes[i] - meanIndex);
        }
        if (variance > 0.0) {
            period = covariance / variance;
        }
    }
    if (constant) {
        double phase = 0.0;
        for (int i = 0; i < count; ++i) {
            phase += static_cast<double>(beatGridMs[i]) - indexes[i] * period;
        }
        phase /= count;
        for (int i = 0; constant && i < count; ++i) {
            constant = std::abs(static_cast<double>(beatGridMs[i]) - phase -
                               indexes[i] * period) <= kConstantGridToleranceMs;
        }
        if (constant) {
            return mixxx::Beats::fromConstTempo(sampleRate,
                    framesOf(phase, sampleRate).toNearestFrameBoundary(),
                    mixxx::Bpm(60000.0 / period),
                    kCorrectionBeatsSubVersion);
        }
    }
    QVector<mixxx::audio::FramePos> positions;
    positions.reserve(count);
    for (const qint64 beat : beatGridMs) {
        positions.append(framesOf(static_cast<double>(beat), sampleRate));
    }
    return mixxx::Beats::fromBeatPositions(sampleRate, positions, kCorrectionBeatsSubVersion);
}

bool applyCorrection(const TrackPointer& pTrack, const Correction& correction) {
    if (!pTrack) {
        return false;
    }
    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    if ((correction.bpm || !correction.beatGridMs.isEmpty()) && !sampleRate.isValid()) {
        return false;
    }
    mixxx::BeatsPointer pBeats;
    if (correction.beatGridMs.size() >= 2) {
        pBeats = beatsOfGrid(sampleRate, correction.beatGridMs, correction.bpm);
    } else if (correction.bpm) {
        // The BPM alone: the grid keeps its first beat
        const mixxx::BeatsPointer pCurrent = pTrack->getBeats();
        pBeats = mixxx::Beats::fromConstTempo(sampleRate,
                pCurrent ? pCurrent->firstBeat() : mixxx::audio::kStartFramePos,
                mixxx::Bpm(*correction.bpm),
                kCorrectionBeatsSubVersion);
    }
    if (pBeats) {
        pTrack->setBpmLocked(false);
        if (!pTrack->trySetBeats(pBeats)) {
            kLogger.warning() << "Cannot set the corrected grid of" << pTrack->getLocation();
        }
        pTrack->setBpmLocked(true);
    }
    const mixxx::track::io::key::ChromaticKey key = analysis::keyFromCamelot(correction.key);
    if (key != mixxx::track::io::key::INVALID) {
        pTrack->setKeys(KeyFactory::makeBasicKeys(key, mixxx::track::io::key::USER));
    }
    return true;
}

CrateCorrections::CrateCorrections(Options options, QObject* pParent)
        : TrackDataSync(Endpoint{QStringLiteral("corrections"),
                                QStringLiteral("/api/decks/corrections/query"),
                                QStringLiteral("corrections"),
                                outboxkind::kCorrection},
                  std::move(options),
                  pParent) {
    // After the baseline and the revision of what was applied are stored
    connect(this,
            &TrackDataSync::serverDataApplied,
            this,
            [this](const QString& serverId, const QString& entityUid) {
                if (m_appliedChanges.remove(keyOf(serverId, entityUid))) {
                    emit correctionChanged(serverId, entityUid.toLower());
                }
            });
}

std::optional<CrateCorrections::Offer> CrateCorrections::offer(
        const QString& serverId, const QString& entityUid) const {
    const auto it = m_offers.constFind(keyOf(serverId, entityUid));
    if (it == m_offers.cend()) {
        return std::nullopt;
    }
    return it.value();
}

bool CrateCorrections::hasCorrection(const QString& serverId, const QString& entityUid) const {
    return !correctionFromJson(stateOf(keyOf(serverId, entityUid)).baseline).isEmpty();
}

bool CrateCorrections::save(const QString& serverId, const QString& entityUid) {
    const QString key = keyOf(serverId, entityUid);
    const TrackPointer pTrack = trackOf(key);
    const auto it = m_offers.constFind(key);
    if (!pTrack || it == m_offers.cend() || !isReconciled(key)) {
        return false;
    }
    const Offer offer = it.value();
    m_offers.remove(key);
    const Correction stored = correctionFromJson(stateOf(key).baseline);
    Correction correction = stored;
    if (offer.grid) {
        const double bpm = pTrack->getBpm();
        correction.bpm = validBpm(bpm) ? std::optional<double>(bpm) : std::nullopt;
        correction.beatGridMs = beatGridOf(pTrack);
    }
    if (offer.key) {
        correction.key = analysis::camelotFromKey(pTrack->getKey());
    }
    if (correction == stored) {
        return false;
    }
    enqueuePut(key, toJson(correction), editStamp(key));
    takeSnapshot(key, pTrack);
    emit correctionChanged(serverIdOf(key), entityUidOf(key));
    return true;
}

bool CrateCorrections::clear(const QString& serverId, const QString& entityUid) {
    const QString key = keyOf(serverId, entityUid);
    if (!hasCorrection(serverId, entityUid)) {
        return false;
    }
    m_offers.remove(key);
    const Correction cleared = correctionFromJson(stateOf(key).baseline);
    enqueuePut(key, toJson(Correction()), editStamp(key));
    // Crate's analysis applies again here (Diego, 2026-11-16)
    if (const TrackPointer pTrack = trackOf(key)) {
        changeQuietly(key, [&] {
            revertCorrection(key, pTrack, cleared);
        });
        takeSnapshot(key, pTrack);
    }
    emit correctionChanged(serverIdOf(key), entityUidOf(key));
    return true;
}

void CrateCorrections::revertCorrection(
        const QString& key, const TrackPointer& pTrack, const Correction& cleared) {
    bool reverted = false;
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    if (pBeats && pBeats->getSubVersion() == kCorrectionBeatsSubVersion) {
        // The profile, or upstream's analysis, may replace it now
        pTrack->setBpmLocked(false);
        reverted = true;
    }
    const mixxx::track::io::key::ChromaticKey correctedKey =
            analysis::keyFromCamelot(cleared.key);
    if (correctedKey != mixxx::track::io::key::INVALID && pTrack->getKey() == correctedKey &&
            analysis::keySourceOf(pTrack) == mixxx::track::io::key::USER) {
        // The same key, no longer the user's: the profile's replaces it
        pTrack->setKeys(KeyFactory::makeBasicKeys(correctedKey, mixxx::track::io::key::ANALYZER));
        reverted = true;
    }
    if (reverted && m_restoreProfile) {
        m_restoreProfile(pTrack, serverIdOf(key), entityUidOf(key));
    }
}

bool CrateCorrections::reconcile(const QString& key,
        const TrackPointer& pTrack,
        qint64 serverRevision,
        const QJsonObject& serverEntry) {
    if (uploadPending(key)) {
        // Its answer, or its 409, settles the revision
        takeSnapshot(key, pTrack);
        return true;
    }
    State& known = state(key);
    if (serverRevision > known.revision) {
        // An offer of this computer gives way to the newer correction
        m_offers.remove(key);
        m_dirty.remove(key);
        applyFromServer(key, pTrack, contentOf(serverEntry), serverRevision);
    } else if (!known.known || serverRevision < known.revision) {
        known.revision = serverRevision;
        known.baseline = contentOf(serverEntry);
        storeState(key);
    }
    takeSnapshot(key, pTrack);
    return true;
}

QJsonObject CrateCorrections::contentOf(const QJsonObject& serverEntry) const {
    return toJson(correctionFromJson(serverEntry));
}

QJsonObject CrateCorrections::applyServerContent(
        const QString& key, const TrackPointer& pTrack, const QJsonObject& content) {
    const Correction correction = correctionFromJson(content);
    const Correction before = correctionFromJson(stateOf(key).baseline);
    if (correction != before) {
        m_appliedChanges.insert(key);
    }
    if (correction.isEmpty()) {
        // Cleared on another computer: Crate's analysis applies again
        revertCorrection(key, pTrack, before);
    } else if (!applyCorrection(pTrack, correction)) {
        kLogger.warning() << "Cannot apply the correction of" << pTrack->getLocation()
                          << ": its sample rate is not known yet";
    }
    return content;
}

QJsonObject CrateCorrections::contentToResend(const QString& key, const QJsonObject& sent) {
    Q_UNUSED(key);
    // A correction is sent when the user says so, with what it had then
    return sent;
}

void CrateCorrections::trackChanged(
        const QString& key, const TrackPointer& pTrack, Change change) {
    const Snapshot snapshot = m_snapshots.value(key);
    if (change == Change::Beats) {
        const mixxx::BeatsPointer pBeats = pTrack->getBeats();
        const bool changed = static_cast<bool>(pBeats) != static_cast<bool>(snapshot.pBeats) ||
                (pBeats && *pBeats != *snapshot.pBeats);
        if (!changed) {
            return;
        }
        m_dirty[key].grid = true;
    } else if (change == Change::Key) {
        // A key the user typed; the analysis' and the tags' are not corrections
        if (analysis::keySourceOf(pTrack) != mixxx::track::io::key::USER ||
                pTrack->getKey() == snapshot.key) {
            return;
        }
        m_dirty[key].key = true;
    } else {
        return;
    }
    debounce(key, [this, key] {
        offerNow(key);
    });
}

void CrateCorrections::absorb(const QString& key, const TrackPointer& pTrack) {
    m_dirty.remove(key);
    takeSnapshot(key, pTrack);
}

void CrateCorrections::delivered(const QString& key) {
    // The server plans with it from now on
    emit correctionChanged(serverIdOf(key), entityUidOf(key));
}

void CrateCorrections::takeSnapshot(const QString& key, const TrackPointer& pTrack) {
    m_snapshots.insert(key, Snapshot{pTrack->getBeats(), pTrack->getKey()});
}

void CrateCorrections::offerNow(const QString& key) {
    const auto it = m_dirty.constFind(key);
    if (it == m_dirty.cend()) {
        return;
    }
    const Offer dirty = it.value();
    m_dirty.erase(it);
    Offer& offer = m_offers[key];
    offer.grid = offer.grid || dirty.grid;
    offer.key = offer.key || dirty.key;
    if (const TrackPointer pTrack = trackOf(key)) {
        takeSnapshot(key, pTrack);
    }
    kLogger.info() << "Offering to save the correction of" << entityUidOf(key) << "to Crate";
    emit correctionOffered(serverIdOf(key), entityUidOf(key));
}

} // namespace crate::writeback

#include "moc_cratecorrections.cpp"
