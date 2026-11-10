#pragma once

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>

class QWidget;

namespace crate {

class CrateServers;

/// The Crate server and connection that the toolbar shows (GU1 proposal
/// "Crate status in the top bar"). Without widgets: the skin's labels are a
/// thin view over it (bindServerStatus()).
///
/// It follows the first server of CrateServers: none, signing in, signed in
/// or signed out.
class ServerStatus : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QString stateName READ stateName NOTIFY changed)

  public:
    enum class State {
        /// No Crate server on this installation.
        None,
        /// Signing in.
        Connecting,
        /// Signed in. Without a network a restored session still counts as
        /// signed in (Session::restore()).
        Connected,
        /// Signed out: the user has to sign in again.
        SignedOut,
    };
    Q_ENUM(State)

    /// `pServers` may be null (no CrateServers this run).
    explicit ServerStatus(CrateServers* pServers, QObject* parent = nullptr);

    State state() const {
        return m_state;
    }
    /// The host of the server, or empty without one.
    QString host() const {
        return m_host;
    }
    /// "Crate · <host>", plus "signed out" when it is: the MonoLabel of the
    /// toolbar, which shows it in capitals.
    QString text() const;
    /// connected, connecting, signedOut or none: the crateState property
    /// that the skin's QSS selects on.
    QString stateName() const;

    /// Shows `host` in `state` until the servers change. For tests and the
    /// screenshots of the skin, which have no server to sign in to.
    void showStatus(const QString& host, State state);

  signals:
    void changed();

  private:
    void followServers();
    void update();

    QPointer<CrateServers> m_pServers;
    QList<QMetaObject::Connection> m_sessionConnections;
    State m_state = State::None;
    QString m_host;
};

/// Keeps the toolbar's MonoCrateStatus and CrateStatusDot labels of the skin
/// in step with `pStatus`: text and crateState property.
void bindServerStatus(ServerStatus* pStatus, QWidget* pSkinRoot);

} // namespace crate
