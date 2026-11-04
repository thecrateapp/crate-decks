#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <optional>

#include "crate/client/crateresult.h"

namespace crate::json {

/// Reads the fields of a JSON object from a Crate server strictly.
///
/// A required field that is missing, or any field that is present with the
/// wrong type, makes the read fail. A field set to null counts as missing.
/// The reader keeps the first problem, with the path of the field, and returns
/// default values from then on, so a parser reads every field and checks ok()
/// once:
///
///     ObjectReader reader(object, QStringLiteral("decks"));
///     const bool available = reader.requiredBool(u"available");
///     const QStringList features = reader.optionalStringList(u"features");
///     if (!reader.ok()) {
///         return reader.error();
///     }
class ObjectReader {
  public:
    /// `path` names the object in error messages, e.g. "decks".
    explicit ObjectReader(const QJsonObject& object, const QString& path = QString());

    QString requiredString(QStringView key);
    QString optionalString(QStringView key, const QString& defaultValue = QString());
    bool requiredBool(QStringView key);
    bool optionalBool(QStringView key, bool defaultValue);
    /// A JSON number without a fractional part.
    qint64 requiredInteger(QStringView key);
    QJsonObject requiredObject(QStringView key);
    std::optional<QJsonObject> optionalObject(QStringView key);
    /// An array whose items are all strings; empty if the field is missing.
    QStringList optionalStringList(QStringView key);

    bool ok() const {
        return !m_error.has_value();
    }
    /// The first problem, as a Decode error. Only meaningful if !ok().
    Error error() const;

  private:
    /// The value of `key`, or Undefined once the read has failed. Records a
    /// problem if a required field is missing.
    QJsonValue field(QStringView key, bool required);
    void fail(QStringView key, const QString& problem);

    QJsonObject m_object;
    QString m_path;
    std::optional<Error> m_error;
};

/// The JSON object of an answer, or a Decode error if the answer is
/// something else.
Result<QJsonObject> requireObject(const QJsonDocument& document);

} // namespace crate::json
