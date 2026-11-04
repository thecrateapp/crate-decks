#pragma once

#include <QJsonDocument>
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
};

/// Version of the Decks server contract this client implements. The server
/// bumps it on every incompatible change; additive changes only add features.
inline constexpr int kDecksContractVersion = 1;

/// What a Crate server says about Crate Decks in `GET /api/capabilities`.
class Capabilities {
  public:
    enum class Compatibility {
        Compatible,
        /// The server does not accept Crate Decks.
        DecksUnavailable,
        /// The server needs a newer Crate Decks (`min_client_version`).
        ClientTooOld,
        /// The server speaks another version of the Decks contract.
        UnsupportedContract,
    };

    /// Parses the answer of `GET /api/capabilities`. A server without a
    /// `decks` section is valid and does not accept Crate Decks; a `decks`
    /// section with missing or mistyped fields is a Decode error. Features
    /// this client does not know are ignored.
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

    /// Whether a client of `clientVersion` can use this server. Versions are
    /// compared numerically, so 0.10.0 is newer than 0.2.0.
    Compatibility compatibility(const QVersionNumber& clientVersion) const;

  private:
    bool m_decksAvailable = false;
    QVersionNumber m_minClientVersion;
    int m_contractVersion = 0;
    /// One bit per DecksFeature.
    unsigned int m_features = 0;
};

} // namespace crate
