#pragma once

#include <QJsonDocument>
#include <QString>
#include <utility>
#include <variant>

#include "util/assert.h"

namespace crate {

/// Why a request to a Crate server, or the reading of its answer, failed.
struct Error {
    enum class Kind {
        /// The server could not be reached, or the transfer broke off or
        /// timed out.
        Network,
        /// The server answered with an HTTP error status.
        Http,
        /// There is no valid session: no access token, or it could not be
        /// renewed.
        Auth,
        /// The answer is not what the client expects.
        Decode,
        /// The request was cancelled before it finished.
        Cancelled,
    };

    Kind kind = Kind::Network;
    /// HTTP status of an Http error; 0 for the other kinds.
    int httpStatus = 0;
    /// Machine-readable code the server sent with an Http error, if any.
    QString code;
    /// Description for logs and the interface. Never contains a secret.
    QString message;

    static Error network(const QString& message) {
        return Error{Kind::Network, 0, QString(), message};
    }
    static Error http(int status, const QString& code, const QString& message) {
        return Error{Kind::Http, status, code, message};
    }
    static Error auth(const QString& message) {
        return Error{Kind::Auth, 0, QString(), message};
    }
    static Error decode(const QString& message) {
        return Error{Kind::Decode, 0, QString(), message};
    }
    static Error cancelled() {
        return Error{Kind::Cancelled, 0, QString(), QStringLiteral("Cancelled")};
    }
};

/// A value, or the Error that prevented it.
template<typename T>
class Result {
  public:
    Result(T value)
            : m_value(std::move(value)) {
    }
    Result(Error error)
            : m_value(std::move(error)) {
    }

    bool ok() const {
        return std::holds_alternative<T>(m_value);
    }
    const T& value() const {
        DEBUG_ASSERT(ok());
        return std::get<T>(m_value);
    }
    const Error& error() const {
        DEBUG_ASSERT(!ok());
        return std::get<Error>(m_value);
    }

  private:
    std::variant<T, Error> m_value;
};

/// Answer of the Crate API: the JSON body (a null document for an empty
/// body), or the error.
using JsonResult = Result<QJsonDocument>;

} // namespace crate
