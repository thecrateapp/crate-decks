#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

namespace crate {

class CrateServers;
class Session;

/// Adds a Crate server step by step. The preferences page and the first-run
/// dialog are thin views on it; it has no widgets.
///
/// 1. setAddress() checks the address (parseServerAddress()). HTTP on the
///    local network waits for confirmLocalNetwork().
/// 2. It asks the server for `GET /api/capabilities` and checks that it
///    accepts this Crate Decks (available, `min_client_version`, contract).
/// 3. loginWithPassword() or loginWithBrowser() signs in; the server is then
///    kept by CrateServers and serverAdded() is emitted.
///
/// On a problem the flow goes back to the step that can fix it, with
/// error() set. Cancelling it, or destroying it before the end, forgets the
/// server.
class AddServerFlow : public QObject {
    Q_OBJECT
  public:
    enum class Step {
        EnterAddress,
        ConfirmLocalNetwork,
        CheckingServer,
        SignIn,
        SigningIn,
        Done,
    };

    explicit AddServerFlow(CrateServers* pServers, QObject* parent = nullptr);
    ~AddServerFlow() override;

    Step step() const {
        return m_step;
    }
    /// What went wrong last, for the interface; empty when nothing did.
    QString error() const {
        return m_error;
    }
    /// The address being added, as `scheme://host[:port]`.
    QUrl origin() const {
        return m_origin;
    }

    void setAddress(const QString& address);
    void confirmLocalNetwork();
    void loginWithPassword(const QString& email, const QString& password);
    /// `provider` is "google" or "apple"; openBrowser() gives the page.
    void loginWithBrowser(const QString& provider);
    void cancel();

  signals:
    void stepChanged(crate::AddServerFlow::Step step);
    void openBrowser(const QUrl& url);
    void serverAdded(const QString& serverId);

  private:
    void checkServer();
    void setStep(Step step, const QString& error = QString());
    void forgetPendingServer();

    const QPointer<CrateServers> m_pServers;
    Step m_step = Step::EnterAddress;
    QString m_error;
    QUrl m_origin;
    QPointer<Session> m_pSession;
    QByteArray m_capabilitiesJson;
};

} // namespace crate
