#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <optional>

#include "crate/client/crateresult.h"

namespace crate::analysis {

/// The parts of a Smart Mix profile that Decks uses, as
/// `GET /api/tracks/by-entity/{uid}/mix-profile?detail=full` and the S4 batch
/// `POST /api/decks/mix-profiles` serve it (`MixProfileResponse` on the
/// server). Times are in milliseconds of the track's own audio. The server
/// leaves out the fields it has no value for, so each optional one may be
/// missing.
struct MixProfile {
    enum class Quality {
        Full,
        Partial,
        Legacy,
        Unavailable,
    };

    /// Lowercase, as the batch answer keys it.
    QString entityUid;
    QString profileRevision;
    QString analyzer;
    QString analyzerVersion;
    Quality quality = Quality::Unavailable;
    qint64 durationMs = 0;
    std::optional<double> bpm;
    std::optional<double> tempoStability;
    std::optional<qint64> beatAnchorMs;
    std::optional<qint64> downbeatAnchorMs;
    std::optional<int> timeSignature;
    /// Absolute beat positions, strictly increasing; empty without a grid.
    QVector<qint64> beatGridMs;
    /// Camelot notation, 1A to 12B; empty without a key.
    QString camelot;
    std::optional<qint64> introCueMs;
    std::optional<qint64> outroCueMs;
    /// The musical start and end, not a −60 dB threshold (server contract,
    /// "Plan and profile fields").
    std::optional<qint64> activeStartMs;
    std::optional<qint64> activeEndMs;
    std::optional<double> integratedLufs;
    std::optional<double> truePeakDbfs;

    bool isFull() const {
        return quality == Quality::Full;
    }
};

/// Reads one profile object. A profile without its uid, revision or a known
/// quality, or with a field of the wrong type, is a Decode error.
Result<MixProfile> parseMixProfile(const QJsonObject& object);
Result<MixProfile> parseMixProfile(const QByteArray& json);

} // namespace crate::analysis
