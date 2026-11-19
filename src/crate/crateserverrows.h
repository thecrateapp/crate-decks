#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace crate {

class CrateServers;

/// Whether a server answers: what its last request learned.
enum class Reachability {
    Unknown,
    Online,
    Offline,
};

/// What the Crate page of Preferences shows of one server (GU1 mockups
/// preferences-crate, plan V2.PR5), without widgets: its name, address and
/// notes, the Decks features of the server and its sign-in state.
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
    /// Whether it answers; only shown while signed in.
    Reachability reachability = Reachability::Unknown;

    /// The crateState property that generated/dialogs.qss selects on, with
    /// the chip's text (Diego, 2026-11-19): signed in, "online" ("Online",
    /// cyan) or "offline" ("Offline", amber), or "signedIn" ("Signed in",
    /// muted) until a request says; otherwise the session's own state,
    /// muted: "connecting" ("Connecting") or "signedOut" ("Signed out").
    QString stateName() const;
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
