#pragma once

#include <QJsonDocument>
#include <QString>
#include <QStringList>
#include <QVersionNumber>

#include "crate/client/crateresult.h"

namespace crate {

/// Parts of the Decks contract that a server announces in
/// `capabilities.decks.features` once it serves them (design D13). Each server
/// phase adds its own.
enum class DecksFeature {
    LibraryCursor, // S3
    ProfilesBatch, // S4
    Beatmatch,     // S5
    BassHandoff,   // S5
    LongBeatmatch, // S5
    Cues,          // S7
    /// Smart Mix Stems ST1: stem analyses and stem files of tracks.
    Stems,
    /// The server takes Decks' stem analyses from this account.
    StemUpload,
    /// ST3: the candidate menu and policy limits in the decks context.
    LocalRefinement,
};

/// Version of the Decks server contract this client implements. The server
/// bumps it on every incompatible change; additive changes only add features.
inline constexpr int kDecksContractVersion = 1;

/// What a Crate server says about Decks in `GET /api/capabilities`.
class Capabilities {
  public:
    enum class Compatibility {
        Compatible,
        /// The server does not accept Decks.
        DecksUnavailable,
        /// The server needs a newer Decks (`min_client_version`).
        ClientTooOld,
        /// The server speaks another version of the Decks contract.
        UnsupportedContract,
    };

    /// Parses the answer of `GET /api/capabilities`. A server without a
    /// `decks` section is valid and does not accept Decks; a `decks`
    /// section with missing or mistyped fields is a Decode error. Features
    /// this client does not know are ignored. Of `smart_mix`, only the
    /// planners and `structure` are read, and may be missing; a mistyped one
    /// is a Decode error.
    static Result<Capabilities> fromJson(const QJsonDocument& document);

    bool decksAvailable() const {
        return m_decksAvailable;
    }
    const QVersionNumber& minClientVersion() const {
        return m_minClientVersion;
    }
    int contractVersion() const {
        return m_contractVersion;
    }
    bool has(DecksFeature feature) const;

    /// The planners `POST /api/playback/transition-plans` takes as
    /// `plannerVersion` (`smart_mix.planner_versions`); empty on a server
    /// before smart-mix-v3 or with Smart Mix off.
    const QStringList& plannerVersions() const {
        return m_plannerVersions;
    }
    bool offersPlanner(const QString& plannerVersion) const {
        return m_plannerVersions.contains(plannerVersion);
    }
    /// The planner of a request without one (`smart_mix.planner_version`);
    /// empty with Smart Mix off.
    const QString& defaultPlannerVersion() const {
        return m_defaultPlannerVersion;
    }
    /// Most of the library has analyzed phrases, so smart-mix-v3 plans on
    /// them (`smart_mix.structure`).
    bool structureAvailable() const {
        return m_structureAvailable;
    }

    /// Whether a client of `clientVersion` can use this server. Versions are
    /// compared numerically, so 0.10.0 is newer than 0.2.0.
    Compatibility compatibility(const QVersionNumber& clientVersion) const;

  private:
    bool m_decksAvailable = false;
    QVersionNumber m_minClientVersion;
    int m_contractVersion = 0;
    /// One bit per DecksFeature.
    unsigned int m_features = 0;
    QStringList m_plannerVersions;
    QString m_defaultPlannerVersion;
    bool m_structureAvailable = false;
};

} // namespace crate
