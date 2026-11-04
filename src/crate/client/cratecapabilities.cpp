#include "crate/client/cratecapabilities.h"

#include <QJsonObject>
#include <optional>

#include "crate/client/cratejson.h"

namespace crate {

namespace {

std::optional<DecksFeature> decksFeatureFromName(const QString& name) {
    if (name == QLatin1String("library_cursor")) {
        return DecksFeature::LibraryCursor;
    }
    if (name == QLatin1String("profiles_batch")) {
        return DecksFeature::ProfilesBatch;
    }
    if (name == QLatin1String("beatmatch")) {
        return DecksFeature::Beatmatch;
    }
    if (name == QLatin1String("bass_handoff")) {
        return DecksFeature::BassHandoff;
    }
    if (name == QLatin1String("long_beatmatch")) {
        return DecksFeature::LongBeatmatch;
    }
    if (name == QLatin1String("cues")) {
        return DecksFeature::Cues;
    }
    return std::nullopt;
}

unsigned int featureBit(DecksFeature feature) {
    return 1u << static_cast<unsigned int>(feature);
}

} // namespace

Result<Capabilities> Capabilities::fromJson(const QJsonDocument& document) {
    const Result<QJsonObject> root = json::requireObject(document);
    if (!root.ok()) {
        return root.error();
    }
    json::ObjectReader rootReader(root.value());
    const std::optional<QJsonObject> decks = rootReader.optionalObject(u"decks");
    if (!rootReader.ok()) {
        return rootReader.error();
    }

    Capabilities capabilities;
    if (!decks) {
        return capabilities;
    }

    json::ObjectReader reader(*decks, QStringLiteral("decks"));
    capabilities.m_decksAvailable = reader.requiredBool(u"available");
    const QString minClientVersion = reader.requiredString(u"min_client_version");
    capabilities.m_contractVersion = static_cast<int>(reader.requiredInteger(u"contract_version"));
    const QStringList features = reader.optionalStringList(u"features");
    if (!reader.ok()) {
        return reader.error();
    }

    qsizetype suffixIndex = 0;
    capabilities.m_minClientVersion = QVersionNumber::fromString(minClientVersion, &suffixIndex);
    if (capabilities.m_minClientVersion.isNull()) {
        return Error::decode(
                QStringLiteral("decks.min_client_version is not a version: %1")
                        .arg(minClientVersion));
    }
    for (const QString& name : features) {
        if (const std::optional<DecksFeature> feature = decksFeatureFromName(name)) {
            capabilities.m_features |= featureBit(*feature);
        }
    }
    return capabilities;
}

bool Capabilities::has(DecksFeature feature) const {
    return (m_features & featureBit(feature)) != 0;
}

Capabilities::Compatibility Capabilities::compatibility(
        const QVersionNumber& clientVersion) const {
    if (!m_decksAvailable) {
        return Compatibility::DecksUnavailable;
    }
    // A server that needs a newer client is reported first: updating Crate
    // Decks is what fixes it, and a new contract comes with a new minimum.
    if (QVersionNumber::compare(clientVersion, m_minClientVersion) < 0) {
        return Compatibility::ClientTooOld;
    }
    if (m_contractVersion != kDecksContractVersion) {
        return Compatibility::UnsupportedContract;
    }
    return Compatibility::Compatible;
}

} // namespace crate
