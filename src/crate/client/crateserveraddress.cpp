#include "crate/client/crateserveraddress.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QList>
#include <QPair>
#include <QStringList>

namespace crate {

namespace {

bool isLoopbackHost(const QString& host) {
    if (host == QLatin1String("localhost")) {
        return true;
    }
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
}

bool isLocalNetworkHost(const QString& host) {
    const QHostAddress address(host);
    if (!address.isNull()) {
        static const QList<QPair<QHostAddress, int>> kLocalSubnets = {
                QHostAddress::parseSubnet(QStringLiteral("10.0.0.0/8")),
                QHostAddress::parseSubnet(QStringLiteral("172.16.0.0/12")),
                QHostAddress::parseSubnet(QStringLiteral("192.168.0.0/16")),
                QHostAddress::parseSubnet(QStringLiteral("169.254.0.0/16")),
                QHostAddress::parseSubnet(QStringLiteral("fc00::/7")),
                QHostAddress::parseSubnet(QStringLiteral("fe80::/10")),
        };
        for (const auto& subnet : kLocalSubnets) {
            if (address.isInSubnet(subnet)) {
                return true;
            }
        }
        return false;
    }
    // A name without a dot ("nas") only resolves on the local network.
    if (!host.contains(QLatin1Char('.'))) {
        return true;
    }
    static const QStringList kLocalSuffixes = {
            QStringLiteral(".local"),
            QStringLiteral(".lan"),
            QStringLiteral(".home.arpa"),
            QStringLiteral(".internal"),
    };
    for (const QString& suffix : kLocalSuffixes) {
        if (host.endsWith(suffix)) {
            return true;
        }
    }
    return false;
}

ServerAddress rejected(const QString& problem) {
    ServerAddress address;
    address.security = ServerAddress::Security::Rejected;
    address.problem = problem;
    return address;
}

} // namespace

ServerAddress parseServerAddress(const QString& input) {
    QString text = input.trimmed();
    if (text.isEmpty()) {
        return rejected(QCoreApplication::translate("crate::ServerAddress",
                "Enter the address of your Crate server."));
    }
    if (!text.contains(QStringLiteral("://"))) {
        text.prepend(QStringLiteral("https://"));
    }
    const QUrl url(text);
    if (!url.isValid() || url.host().isEmpty()) {
        return rejected(QCoreApplication::translate(
                "crate::ServerAddress", "This is not a valid address."));
    }
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) {
        return rejected(QCoreApplication::translate("crate::ServerAddress",
                "Use an address that starts with https://."));
    }
    if (!url.userInfo().isEmpty()) {
        return rejected(QCoreApplication::translate("crate::ServerAddress",
                "Do not put a user name or a password in the address."));
    }

    const QString host = url.host().toLower();
    ServerAddress address;
    address.origin.setScheme(scheme);
    address.origin.setHost(host);
    if (url.port() != -1) {
        address.origin.setPort(url.port());
    }
    if (scheme == QLatin1String("https") || isLoopbackHost(host)) {
        address.security = ServerAddress::Security::Secure;
    } else if (isLocalNetworkHost(host)) {
        address.security = ServerAddress::Security::LocalNetworkHttp;
    } else {
        return rejected(QCoreApplication::translate("crate::ServerAddress",
                "Use https://. Plain http:// is only allowed on this computer "
                "or on your local network."));
    }
    return address;
}

} // namespace crate
