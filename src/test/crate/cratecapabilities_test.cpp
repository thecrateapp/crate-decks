#include "crate/client/cratecapabilities.h"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QVersionNumber>

namespace {

using crate::Capabilities;
using crate::DecksFeature;
using crate::Error;
using Compatibility = crate::Capabilities::Compatibility;

QJsonDocument parse(const char* json) {
    return QJsonDocument::fromJson(QByteArray(json));
}

const QVersionNumber kClientVersion(0, 1, 0);

TEST(CrateCapabilitiesTest, ParsesDecksSection) {
    const auto result = Capabilities::fromJson(parse(R"({
        "smart_mix": {"available": true, "planner_version": "v2"},
        "decks": {
            "available": true,
            "min_client_version": "0.1.0",
            "contract_version": 1,
            "features": ["beatmatch", "cues", "a_feature_from_the_future"]
        }
    })"));

    ASSERT_TRUE(result.ok()) << result.error().message.toStdString();
    const Capabilities& capabilities = result.value();
    EXPECT_TRUE(capabilities.decksAvailable());
    EXPECT_EQ(QVersionNumber(0, 1, 0), capabilities.minClientVersion());
    EXPECT_EQ(1, capabilities.contractVersion());
    EXPECT_TRUE(capabilities.has(DecksFeature::Beatmatch));
    EXPECT_TRUE(capabilities.has(DecksFeature::Cues));
    EXPECT_FALSE(capabilities.has(DecksFeature::LibraryCursor));
    EXPECT_EQ(Compatibility::Compatible, capabilities.compatibility(kClientVersion));
}

// Smart Mix Stems (ST1, ST3): what the stems of Decks need (plan ST5.PR2)
TEST(CrateCapabilitiesTest, ParsesTheStemFeatures) {
    const auto result = Capabilities::fromJson(parse(R"({
        "decks": {
            "available": true,
            "min_client_version": "0.1.0",
            "contract_version": 1,
            "features": ["library_cursor", "stems", "stem_upload", "local_refinement"]
        }
    })"));
    ASSERT_TRUE(result.ok()) << result.error().message.toStdString();
    EXPECT_TRUE(result.value().has(DecksFeature::Stems));
    EXPECT_TRUE(result.value().has(DecksFeature::StemUpload));
    EXPECT_TRUE(result.value().has(DecksFeature::LocalRefinement));
    EXPECT_FALSE(result.value().has(DecksFeature::Cues));
}

// Planner v3 in Decks (plan F6b.PR1): the planners the plan API takes and
// whether most of the library has phrases, as the dev stack serves them. The
// AI refinement (`ai_assisted`) was dropped after SG3 and is not read.
TEST(CrateCapabilitiesTest, ParsesPlannerVersionsAndStructure) {
    const auto result = Capabilities::fromJson(parse(R"({
        "smart_mix": {
            "available": true,
            "planner_version": "smart-mix-v2",
            "planner_versions": ["smart-mix-v2", "smart-mix-v3"],
            "structure": true,
            "listen_planner_version": "smart-mix-v2",
            "ai_assisted": {"available": false, "provider_kind": null, "model_label": null,
                            "prompt_version": "sm3-refine-3", "typical_latency_ms": null,
                            "reason": "disabled"}
        },
        "decks": {
            "available": true,
            "min_client_version": "0.1.0",
            "contract_version": 1,
            "features": ["beatmatch", "local_refinement"]
        }
    })"));

    ASSERT_TRUE(result.ok()) << result.error().message.toStdString();
    const Capabilities& capabilities = result.value();
    EXPECT_EQ(QStringLiteral("smart-mix-v2"), capabilities.defaultPlannerVersion());
    EXPECT_EQ((QStringList{QStringLiteral("smart-mix-v2"), QStringLiteral("smart-mix-v3")}),
            capabilities.plannerVersions());
    EXPECT_TRUE(capabilities.offersPlanner(QStringLiteral("smart-mix-v3")));
    EXPECT_FALSE(capabilities.offersPlanner(QStringLiteral("smart-mix-v4")));
    EXPECT_TRUE(capabilities.structureAvailable());
}

// A server before smart-mix-v3 lists no planners, and one with Smart Mix off
// lists none and has a null default: no planner is offered, so Decks asks v2.
TEST(CrateCapabilitiesTest, MissingPlannerVersionsOfferNoPlanner) {
    const auto older = Capabilities::fromJson(parse(R"({
        "smart_mix": {"available": true, "planner_version": "smart-mix-v2"}
    })"));
    const auto off = Capabilities::fromJson(parse(R"({
        "smart_mix": {"available": false, "planner_version": null,
                      "planner_versions": [], "structure": false}
    })"));
    const auto none = Capabilities::fromJson(parse(R"({})"));

    for (const auto* pResult : {&older, &off, &none}) {
        ASSERT_TRUE(pResult->ok()) << pResult->error().message.toStdString();
        EXPECT_TRUE(pResult->value().plannerVersions().isEmpty());
        EXPECT_FALSE(pResult->value().offersPlanner(QStringLiteral("smart-mix-v3")));
        EXPECT_FALSE(pResult->value().structureAvailable());
    }
    EXPECT_EQ(QStringLiteral("smart-mix-v2"), older.value().defaultPlannerVersion());
    EXPECT_TRUE(off.value().defaultPlannerVersion().isEmpty());
}

TEST(CrateCapabilitiesTest, MalformedSmartMixSectionIsADecodeError) {
    const auto result = Capabilities::fromJson(parse(R"({
        "smart_mix": {"available": true, "planner_versions": "smart-mix-v3"}
    })"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(Error::Kind::Decode, result.error().kind);
    EXPECT_EQ(QStringLiteral("smart_mix.planner_versions is not an array"),
            result.error().message);
}

TEST(CrateCapabilitiesTest, MissingDecksSectionMeansUnavailable) {
    const auto result = Capabilities::fromJson(parse(R"({"smart_mix": {"available": true}})"));

    ASSERT_TRUE(result.ok()) << result.error().message.toStdString();
    EXPECT_FALSE(result.value().decksAvailable());
    EXPECT_EQ(Compatibility::DecksUnavailable, result.value().compatibility(kClientVersion));
}

TEST(CrateCapabilitiesTest, DisabledDecksSectionMeansUnavailable) {
    const auto result = Capabilities::fromJson(parse(R"({"decks": {
        "available": false, "min_client_version": "0.1.0", "contract_version": 1, "features": []
    }})"));

    ASSERT_TRUE(result.ok()) << result.error().message.toStdString();
    EXPECT_EQ(Compatibility::DecksUnavailable, result.value().compatibility(kClientVersion));
}

TEST(CrateCapabilitiesTest, MinClientVersionBlocksOlderClient) {
    const auto result = Capabilities::fromJson(parse(R"({"decks": {
        "available": true, "min_client_version": "0.2.0", "contract_version": 1, "features": []
    }})"));

    ASSERT_TRUE(result.ok()) << result.error().message.toStdString();
    const Capabilities& capabilities = result.value();
    EXPECT_EQ(Compatibility::ClientTooOld, capabilities.compatibility(QVersionNumber(0, 1, 0)));
    EXPECT_EQ(Compatibility::Compatible, capabilities.compatibility(QVersionNumber(0, 2, 0)));
    EXPECT_EQ(Compatibility::Compatible, capabilities.compatibility(QVersionNumber(0, 10, 0)));
    EXPECT_EQ(Compatibility::Compatible, capabilities.compatibility(QVersionNumber(1, 0, 0)));
}

TEST(CrateCapabilitiesTest, UnknownContractVersionIsIncompatible) {
    const auto result = Capabilities::fromJson(parse(R"({"decks": {
        "available": true, "min_client_version": "0.1.0", "contract_version": 2, "features": []
    }})"));

    ASSERT_TRUE(result.ok()) << result.error().message.toStdString();
    EXPECT_EQ(Compatibility::UnsupportedContract, result.value().compatibility(kClientVersion));
}

TEST(CrateCapabilitiesTest, MalformedDecksSectionIsADecodeError) {
    const auto mistyped = Capabilities::fromJson(parse(R"({"decks": {
        "available": true, "min_client_version": 1, "contract_version": 1, "features": []
    }})"));
    const auto notAVersion = Capabilities::fromJson(parse(R"({"decks": {
        "available": true, "min_client_version": "soon", "contract_version": 1, "features": []
    }})"));

    ASSERT_FALSE(mistyped.ok());
    EXPECT_EQ(Error::Kind::Decode, mistyped.error().kind);
    EXPECT_TRUE(mistyped.error().message.startsWith(QStringLiteral("decks.min_client_version")));
    ASSERT_FALSE(notAVersion.ok());
    EXPECT_EQ(Error::Kind::Decode, notAVersion.error().kind);
}

TEST(CrateCapabilitiesTest, MinClientVersionMustBeAVersion) {
    const auto junk = Capabilities::fromJson(parse(R"({"decks": {
        "available": true, "min_client_version": "1.2.three", "contract_version": 1, "features": []
    }})"));
    const auto preRelease = Capabilities::fromJson(parse(R"({"decks": {
        "available": true, "min_client_version": "0.2.0-beta.1", "contract_version": 1, "features": []
    }})"));

    ASSERT_FALSE(junk.ok());
    EXPECT_EQ(Error::Kind::Decode, junk.error().kind);
    ASSERT_TRUE(preRelease.ok()) << preRelease.error().message.toStdString();
    EXPECT_EQ(QVersionNumber(0, 2, 0), preRelease.value().minClientVersion());
}

TEST(CrateCapabilitiesTest, NonObjectAnswerIsADecodeError) {
    const auto result = Capabilities::fromJson(parse("[]"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(Error::Kind::Decode, result.error().kind);
}

} // namespace
