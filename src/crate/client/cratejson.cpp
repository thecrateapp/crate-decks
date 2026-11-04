#include "crate/client/cratejson.h"

#include <QJsonArray>
#include <cmath>

namespace crate::json {

namespace {

bool isAbsent(const QJsonValue& value) {
    return value.isUndefined() || value.isNull();
}

// Integers beyond 2^53 cannot be told apart from their neighbours in a double.
constexpr double kMaxExactInteger = 9007199254740992.0;

} // namespace

ObjectReader::ObjectReader(const QJsonObject& object, const QString& path)
        : m_object(object),
          m_path(path) {
}

QString ObjectReader::requiredString(QStringView key) {
    const QJsonValue value = field(key, true);
    if (isAbsent(value)) {
        return QString();
    }
    if (!value.isString()) {
        fail(key, QStringLiteral("is not a string"));
        return QString();
    }
    return value.toString();
}

QString ObjectReader::optionalString(QStringView key, const QString& defaultValue) {
    const QJsonValue value = field(key, false);
    if (isAbsent(value)) {
        return defaultValue;
    }
    if (!value.isString()) {
        fail(key, QStringLiteral("is not a string"));
        return defaultValue;
    }
    return value.toString();
}

bool ObjectReader::requiredBool(QStringView key) {
    const QJsonValue value = field(key, true);
    if (isAbsent(value)) {
        return false;
    }
    if (!value.isBool()) {
        fail(key, QStringLiteral("is not a boolean"));
        return false;
    }
    return value.toBool();
}

bool ObjectReader::optionalBool(QStringView key, bool defaultValue) {
    const QJsonValue value = field(key, false);
    if (isAbsent(value)) {
        return defaultValue;
    }
    if (!value.isBool()) {
        fail(key, QStringLiteral("is not a boolean"));
        return defaultValue;
    }
    return value.toBool();
}

qint64 ObjectReader::requiredInteger(QStringView key) {
    const QJsonValue value = field(key, true);
    if (isAbsent(value)) {
        return 0;
    }
    const double number = value.toDouble();
    if (!value.isDouble() || std::trunc(number) != number ||
            std::abs(number) > kMaxExactInteger) {
        fail(key, QStringLiteral("is not an integer"));
        return 0;
    }
    return static_cast<qint64>(number);
}

std::optional<qint64> ObjectReader::optionalInteger(QStringView key) {
    const QJsonValue value = field(key, false);
    if (isAbsent(value)) {
        return std::nullopt;
    }
    const double number = value.toDouble();
    if (!value.isDouble() || std::trunc(number) != number ||
            std::abs(number) > kMaxExactInteger) {
        fail(key, QStringLiteral("is not an integer"));
        return std::nullopt;
    }
    return static_cast<qint64>(number);
}

QJsonObject ObjectReader::requiredObject(QStringView key) {
    const QJsonValue value = field(key, true);
    if (isAbsent(value)) {
        return QJsonObject();
    }
    if (!value.isObject()) {
        fail(key, QStringLiteral("is not an object"));
        return QJsonObject();
    }
    return value.toObject();
}

std::optional<QJsonObject> ObjectReader::optionalObject(QStringView key) {
    const QJsonValue value = field(key, false);
    if (isAbsent(value)) {
        return std::nullopt;
    }
    if (!value.isObject()) {
        fail(key, QStringLiteral("is not an object"));
        return std::nullopt;
    }
    return value.toObject();
}

QStringList ObjectReader::optionalStringList(QStringView key) {
    const QJsonValue value = field(key, false);
    if (isAbsent(value)) {
        return QStringList();
    }
    if (!value.isArray()) {
        fail(key, QStringLiteral("is not an array"));
        return QStringList();
    }
    QStringList strings;
    const QJsonArray array = value.toArray();
    strings.reserve(array.size());
    for (const QJsonValue& item : array) {
        if (!item.isString()) {
            fail(key, QStringLiteral("has an item that is not a string"));
            return QStringList();
        }
        strings.append(item.toString());
    }
    return strings;
}

Error ObjectReader::error() const {
    return m_error.value_or(Error::decode(QString()));
}

QJsonValue ObjectReader::field(QStringView key, bool required) {
    if (m_error) {
        return QJsonValue(QJsonValue::Undefined);
    }
    const QJsonValue value = m_object.value(key);
    if (required && isAbsent(value)) {
        fail(key, QStringLiteral("is missing"));
    }
    return value;
}

void ObjectReader::fail(QStringView key, const QString& problem) {
    if (m_error) {
        return;
    }
    const QString path = m_path.isEmpty()
            ? key.toString()
            : m_path + QLatin1Char('.') + key.toString();
    m_error = Error::decode(path + QLatin1Char(' ') + problem);
}

Result<QJsonObject> requireObject(const QJsonDocument& document) {
    if (!document.isObject()) {
        return Error::decode(QStringLiteral("The answer is not a JSON object"));
    }
    return document.object();
}

} // namespace crate::json
