#include "crate/crateaddserverflow.h"

#include <QJsonDocument>
#include <QVersionNumber>

#include "crate/appidentity.h"
#include "crate/client/cratecapabilities.h"
#include "crate/client/crateclient.h"
#include "crate/client/crateserveraddress.h"
#include "crate/crateservers.h"
#include "crate/session/cratesession.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::AddServerFlow");

} // namespace

AddServerFlow::AddServerFlow(CrateServers* pServers, QObject* parent)
        : QObject(parent),
          m_pServers(pServers) {
}

AddServerFlow::~AddServerFlow() {
    forgetPendingServer();
}

void AddServerFlow::setAddress(const QString& address) {
    forgetPendingServer();
    const ServerAddress parsed = parseServerAddress(address);
    if (parsed.security == ServerAddress::Security::Rejected) {
        m_origin = QUrl();
        setStep(Step::EnterAddress, parsed.problem);
        return;
    }
    if (m_pServers && m_pServers->hasOrigin(parsed.origin)) {
        m_origin = QUrl();
        setStep(Step::EnterAddress, tr("This Crate server is already added."));
        return;
    }
    m_origin = parsed.origin;
    if (parsed.security == ServerAddress::Security::LocalNetworkHttp) {
        setStep(Step::ConfirmLocalNetwork);
        return;
    }
    checkServer();
}

void AddServerFlow::confirmLocalNetwork() {
    if (m_step == Step::ConfirmLocalNetwork) {
        checkServer();
    }
}

void AddServerFlow::loginWithPassword(const QString& email, const QString& password) {
    if (m_step != Step::SignIn || !m_pSession) {
        return;
    }
    setStep(Step::SigningIn);
    m_pSession->loginWithPassword(email, password);
}

void AddServerFlow::loginWithBrowser(const QString& provider) {
    if (m_step != Step::SignIn || !m_pSession) {
        return;
    }
    setStep(Step::SigningIn);
    m_pSession->loginWithBrowser(provider);
}

void AddServerFlow::cancel() {
    forgetPendingServer();
    m_origin = QUrl();
    setStep(Step::EnterAddress);
}

void AddServerFlow::checkServer() {
    if (!m_pServers) {
        setStep(Step::EnterAddress, tr("Crate is not available in this build."));
        return;
    }
    Session* const pSession = m_pServers->addServer(m_origin, m_origin.host());
    if (!pSession) {
        setStep(Step::EnterAddress, tr("Crate Decks cannot use this address."));
        return;
    }
    m_pSession = pSession;
    connect(pSession, &Session::openBrowser, this, &AddServerFlow::openBrowser);
    connect(pSession, &Session::signedIn, this, [this, pSession] {
        if (m_pSession != pSession || m_step != Step::SigningIn) {
            return;
        }
        // CrateServers saved the server first: its slot was connected first.
        const QString serverId = pSession->profile().id;
        if (!m_pServers->isSaved(serverId)) {
            // Signed in but not stored: it would be lost at the next start.
            forgetPendingServer();
            setStep(Step::EnterAddress, tr("Crate Decks could not save this server. Try again."));
            return;
        }
        m_pServers->rememberCapabilities(serverId, m_capabilitiesJson);
        m_pSession.clear();
        setStep(Step::Done);
        emit serverAdded(serverId);
    });
    connect(pSession, &Session::loginFailed, this, [this, pSession](const Error& error) {
        if (m_pSession != pSession || m_step != Step::SigningIn) {
            return;
        }
        setStep(Step::SignIn,
                error.kind == Error::Kind::Cancelled ? tr("The sign-in was cancelled.")
                                                     : error.message);
    });

    setStep(Step::CheckingServer);
    RequestOptions anonymous;
    anonymous.authenticated = false;
    Reply* pReply = m_pServers->client(pSession->profile().id)
                            ->get(QStringLiteral("/api/capabilities"), anonymous);
    const QPointer<Session> pChecked(pSession);
    connect(pReply, &Reply::finished, this, [this, pChecked](const JsonResult& result) {
        if (!pChecked || m_pSession != pChecked || m_step != Step::CheckingServer) {
            return;
        }
        if (!result.ok()) {
            kLogger.info() << "Cannot reach" << m_origin << ":" << result.error().message;
            forgetPendingServer();
            setStep(Step::EnterAddress,
                    tr("Cannot reach the server: %1").arg(result.error().message));
            return;
        }
        const Result<Capabilities> capabilities = Capabilities::fromJson(result.value());
        if (!capabilities.ok()) {
            forgetPendingServer();
            setStep(Step::EnterAddress, tr("This does not look like a Crate server."));
            return;
        }
        QString problem;
        switch (capabilities.value().compatibility(
                QVersionNumber::fromString(QString::fromLatin1(kApplicationVersion)))) {
        case Capabilities::Compatibility::Compatible:
            m_capabilitiesJson = result.value().toJson(QJsonDocument::Compact);
            setStep(Step::SignIn);
            return;
        case Capabilities::Compatibility::DecksUnavailable:
            problem = tr("This Crate server does not accept Crate Decks.");
            break;
        case Capabilities::Compatibility::ClientTooOld:
            problem = tr(
                    "This Crate server needs a newer Crate Decks. Update Crate Decks "
                    "and try again.");
            break;
        case Capabilities::Compatibility::UnsupportedContract:
            problem = tr(
                    "This Crate server and this Crate Decks do not speak the same "
                    "version. Update the one that is older.");
            break;
        }
        forgetPendingServer();
        setStep(Step::EnterAddress, problem);
    });
}

void AddServerFlow::setStep(Step step, const QString& error) {
    if (m_step == step && m_error == error) {
        return;
    }
    m_step = step;
    m_error = error;
    emit stepChanged(step);
}

void AddServerFlow::forgetPendingServer() {
    m_capabilitiesJson.clear();
    if (!m_pSession) {
        return;
    }
    const QString serverId = m_pSession->profile().id;
    m_pSession->disconnect(this);
    m_pSession.clear();
    if (m_pServers) {
        m_pServers->removeServer(serverId);
    }
}

} // namespace crate

#include "moc_crateaddserverflow.cpp"
