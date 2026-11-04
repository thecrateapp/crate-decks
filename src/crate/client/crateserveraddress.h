#pragma once

#include <QString>
#include <QUrl>

namespace crate {

/// What Crate Decks makes of the address of a Crate server typed by the user.
struct ServerAddress {
    enum class Security {
        /// HTTPS, or HTTP to this computer.
        Secure,
        /// HTTP on the local network: the password and the tokens cross it
        /// unencrypted, so the user must confirm it first.
        LocalNetworkHttp,
        /// Not usable: HTTP on the internet, another scheme, credentials in
        /// the address, or not an address at all.
        Rejected,
    };

    Security security = Security::Rejected;
    /// `scheme://host[:port]`; a path, query or fragment is dropped.
    QUrl origin;
    /// Why the address was rejected, for the interface.
    QString problem;
};

/// Parses the address as typed, e.g. "crate.example.com",
/// "https://crate.example.com/" or "http://192.168.1.10:8585". Without a
/// scheme, HTTPS is assumed.
///
/// HTTP is accepted without asking for this computer (localhost, 127.0.0.0/8,
/// ::1); it needs confirmation for the local network (10.0.0.0/8,
/// 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16, fc00::/7, fe80::/10, names
/// without a dot, and names ending in .local, .lan, .home.arpa or
/// .internal); anywhere else it is rejected.
ServerAddress parseServerAddress(const QString& input);

} // namespace crate
