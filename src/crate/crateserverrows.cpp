#include "crate/crateserverrows.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <utility>

#include "crate/client/cratecapabilities.h"
#include "crate/client/crateserveraddress.h"
#include "crate/crateservers.h"
#include "crate/session/cratesession.h"

namespace crate {

namespace {

QString translate(const char* text) {
    return QCoreApplication::translate("crate::DlgPrefCrate", text);
}

ServerRow::State rowState(Session::State state) {
    switch (state) {
    case Session::State::SignedIn:
        return ServerRow::State::SignedIn;
    case Session::State::SigningIn:
        return ServerRow::State::Connecting;
    case Session::State::SignedOut:
        return ServerRow::State::SignedOut;
    }
    return ServerRow::State::SignedOut;
}

} // namespace

QString ServerRow::stateName() const {
    switch (state) {
    case State::SignedIn:
        return QStringLiteral("signedIn");
    case State::Connecting:
        return QStringLiteral("connecting");
    case State::SignedOut:
        return QStringLiteral("signedOut");
    }
    return QString();
}

QString ServerRow::stateText() const {
    switch (state) {
    case State::SignedIn:
        return translate("Signed in");
    case State::Connecting:
        return translate("Connecting");
    case State::SignedOut:
        return translate("Signed out");
    }
    return QString();
}

QList<ServerRow::Feature> serverFeatures(const QByteArray& capabilitiesJson) {
    const Result<Capabilities> capabilities =
            Capabilities::fromJson(QJsonDocument::fromJson(capabilitiesJson));
    if (!capabilities.ok()) {
        return {};
    }
    const std::pair<DecksFeature, const char*> names[] = {
            {DecksFeature::LibraryCursor, QT_TRANSLATE_NOOP("crate::DlgPrefCrate", "library")},
            {DecksFeature::ProfilesBatch, QT_TRANSLATE_NOOP("crate::DlgPrefCrate", "analysis")},
            {DecksFeature::Beatmatch, QT_TRANSLATE_NOOP("crate::DlgPrefCrate", "beatmatch")},
            {DecksFeature::BassHandoff, QT_TRANSLATE_NOOP("crate::DlgPrefCrate", "bass handoff")},
            {DecksFeature::LongBeatmatch,
                    QT_TRANSLATE_NOOP("crate::DlgPrefCrate", "long beatmatch")},
            {DecksFeature::Cues, QT_TRANSLATE_NOOP("crate::DlgPrefCrate", "cues")},
    };
    QList<ServerRow::Feature> features;
    for (const auto& [feature, name] : names) {
        features.append({translate(name), capabilities.value().has(feature)});
    }
    return features;
}

QList<ServerRow> serverRows(const CrateServers& servers) {
    QList<ServerRow> rows;
    const QList<Session*> sessions = servers.sessions();
    for (const Session* pSession : sessions) {
        const ServerProfile& profile = pSession->profile();
        if (!servers.isSaved(profile.id)) {
            continue;
        }
        ServerRow row;
        row.id = profile.id;
        row.name = profile.displayName.isEmpty() ? profile.baseUrl.host() : profile.displayName;
        row.meta.append(profile.baseUrl.toString());
        if (parseServerAddress(profile.baseUrl.toString()).security ==
                ServerAddress::Security::LocalNetworkHttp) {
            row.meta.append(translate("local network, no HTTPS"));
        }
        row.state = rowState(pSession->state());
        if (row.state == ServerRow::State::SignedIn && !profile.accountEmail.isEmpty()) {
            row.meta.append(translate("signed in as %1").arg(profile.accountEmail));
        } else if (row.state == ServerRow::State::SignedOut) {
            row.meta.append(translate("sign out and add the server again"));
        }
        row.features = serverFeatures(servers.lastCapabilities(profile.id));
        rows.append(row);
    }
    return rows;
}

} // namespace crate
