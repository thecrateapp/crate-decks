#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace crate {

class CrateServers;

/// What the Crate page of Preferences shows of one server (GU1 mockups
/// preferences-crate, plan V2.PR5), without widgets: its name, address and
/// notes, the Crate Decks features of the server and its sign-in state.
struct ServerRow {
    enum class State {
        SignedIn,
        Connecting,
        SignedOut,
    };

    struct Feature {
        QString name;
        bool available = false;

        bool operator==(const Feature& other) const = default;
    };

    /// The server's profile id: what "Sign out and remove" removes.
    QString id;
    /// Its display name or, without one, its host.
    QString name;
    /// Its origin ("https://crate.example.com") and what to know about it:
    /// plain HTTP on the local network, the signed-in account.
    QStringList meta;
    /// Every feature of the Decks contract, available or not.
    QList<Feature> features;
    State state = State::SignedOut;

    /// signedIn, connecting or signedOut: the crateState property that
    /// generated/default.qss selects on.
    QString stateName() const;
    /// "Signed in", "Connecting" or "Signed out".
    QString stateText() const;
};

/// The features of the Decks contract, in the mockups' order, each with
/// whether `capabilitiesJson` (GET /api/capabilities) announces it. Empty if
/// there is no valid answer.
QList<ServerRow::Feature> serverFeatures(const QByteArray& capabilitiesJson);

/// The saved servers of `servers`, in their order. A server being added is
/// left out until it is saved.
QList<ServerRow> serverRows(const CrateServers& servers);

} // namespace crate
