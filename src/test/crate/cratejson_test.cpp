#include "crate/client/cratejson.h"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>

namespace {

using crate::Error;
using crate::json::ObjectReader;

QJsonObject parse(const char* json) {
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

TEST(CrateJsonTest, ReadsRequiredAndOptionalFields) {
    ObjectReader reader(parse(R"({
        "name": "Crate",
        "enabled": true,
        "count": 3,
        "tags": ["a", "b"],
        "inner": {"x": 1}
    })"));

    EXPECT_EQ(QStringLiteral("Crate"), reader.requiredString(u"name"));
    EXPECT_TRUE(reader.requiredBool(u"enabled"));
    EXPECT_EQ(3, reader.requiredInteger(u"count"));
    EXPECT_EQ(QStringList({QStringLiteral("a"), QStringLiteral("b")}),
            reader.optionalStringList(u"tags"));
    EXPECT_EQ(1, reader.requiredObject(u"inner").value(QStringLiteral("x")).toInt());
    EXPECT_TRUE(reader.optionalObject(u"inner").has_value());
    EXPECT_TRUE(reader.ok());
}

TEST(CrateJsonTest, MissingOrNullOptionalFieldsUseDefaults) {
    ObjectReader reader(parse(R"({"cleared": null})"));

    EXPECT_EQ(QStringLiteral("fallback"),
            reader.optionalString(u"absent", QStringLiteral("fallback")));
    EXPECT_EQ(QStringLiteral("fallback"),
            reader.optionalString(u"cleared", QStringLiteral("fallback")));
    EXPECT_TRUE(reader.optionalBool(u"absent", true));
    EXPECT_TRUE(reader.optionalStringList(u"absent").isEmpty());
    EXPECT_FALSE(reader.optionalObject(u"cleared").has_value());
    EXPECT_TRUE(reader.ok());
}

TEST(CrateJsonTest, MissingRequiredFieldFailsWithItsPath) {
    ObjectReader reader(parse(R"({"min_client_version": null})"), QStringLiteral("decks"));

    reader.requiredString(u"min_client_version");

    ASSERT_FALSE(reader.ok());
    EXPECT_EQ(Error::Kind::Decode, reader.error().kind);
    EXPECT_EQ(QStringLiteral("decks.min_client_version is missing"), reader.error().message);
}

TEST(CrateJsonTest, WrongTypeFailsEvenWhenOptional) {
    ObjectReader reader(parse(R"({"label": 7})"));

    reader.optionalString(u"label");

    ASSERT_FALSE(reader.ok());
    EXPECT_EQ(QStringLiteral("label is not a string"), reader.error().message);
}

TEST(CrateJsonTest, OptionalIntegerIsReadOrMissing) {
    ObjectReader reader(parse(R"({"id": 7, "cleared": null})"));

    EXPECT_EQ(std::optional<qint64>(7), reader.optionalInteger(u"id"));
    EXPECT_EQ(std::nullopt, reader.optionalInteger(u"absent"));
    EXPECT_EQ(std::nullopt, reader.optionalInteger(u"cleared"));
    EXPECT_TRUE(reader.ok());
}

TEST(CrateJsonTest, OptionalIntegerRejectsOtherTypes) {
    ObjectReader reader(parse(R"({"id": "7"})"));

    reader.optionalInteger(u"id");

    ASSERT_FALSE(reader.ok());
    EXPECT_EQ(QStringLiteral("id is not an integer"), reader.error().message);
}

TEST(CrateJsonTest, IntegerRejectsFractions) {
    ObjectReader reader(parse(R"({"count": 1.5})"));

    reader.requiredInteger(u"count");

    ASSERT_FALSE(reader.ok());
    EXPECT_EQ(QStringLiteral("count is not an integer"), reader.error().message);
}

TEST(CrateJsonTest, StringListRejectsOtherItems) {
    ObjectReader reader(parse(R"({"features": ["beatmatch", 2]})"));

    reader.optionalStringList(u"features");

    ASSERT_FALSE(reader.ok());
    EXPECT_EQ(QStringLiteral("features has an item that is not a string"),
            reader.error().message);
}

TEST(CrateJsonTest, KeepsTheFirstProblem) {
    ObjectReader reader(parse(R"({"a": 1})"));

    reader.requiredString(u"a");
    reader.requiredString(u"b");

    ASSERT_FALSE(reader.ok());
    EXPECT_EQ(QStringLiteral("a is not a string"), reader.error().message);
}

TEST(CrateJsonTest, RequireObjectRejectsOtherDocuments) {
    EXPECT_FALSE(crate::json::requireObject(QJsonDocument::fromJson("[1]")).ok());
    EXPECT_FALSE(crate::json::requireObject(QJsonDocument()).ok());
    EXPECT_EQ(Error::Kind::Decode,
            crate::json::requireObject(QJsonDocument::fromJson("[1]")).error().kind);
    EXPECT_TRUE(crate::json::requireObject(QJsonDocument::fromJson(R"({"a": 1})")).ok());
}

TEST(CrateJsonTest, OptionalDoubleReadsAnyNumber) {
    ObjectReader reader(parse(R"({"bpm": 124.5, "energy": 7, "none": null})"));

    EXPECT_EQ(124.5, reader.optionalDouble(u"bpm").value_or(0));
    EXPECT_EQ(7.0, reader.optionalDouble(u"energy").value_or(0));
    EXPECT_FALSE(reader.optionalDouble(u"none").has_value());
    EXPECT_FALSE(reader.optionalDouble(u"missing").has_value());
    EXPECT_TRUE(reader.ok());

    ObjectReader wrong(parse(R"({"bpm": "124"})"));
    EXPECT_FALSE(wrong.optionalDouble(u"bpm").has_value());
    EXPECT_FALSE(wrong.ok());
}

TEST(CrateJsonTest, IdIsAStringOrAnInteger) {
    ObjectReader reader(parse(R"({"uuid": "4f1c", "id": 42, "parent": null})"));

    EXPECT_EQ(QStringLiteral("4f1c"), reader.requiredId(u"uuid"));
    EXPECT_EQ(QStringLiteral("42"), reader.requiredId(u"id"));
    EXPECT_EQ(QString(), reader.optionalId(u"parent"));
    EXPECT_TRUE(reader.ok());

    ObjectReader fraction(parse(R"({"id": 4.5})"));
    fraction.requiredId(u"id");
    EXPECT_FALSE(fraction.ok());
    ObjectReader missing(parse(R"({})"));
    missing.requiredId(u"id");
    EXPECT_EQ(QStringLiteral("id is missing"), missing.error().message);
}

} // namespace
