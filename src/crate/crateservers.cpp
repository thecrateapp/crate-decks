#include "crate/crateservers.h"

#include <QNetworkAccessManager>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <chrono>

#include "crate/client/crateclient.h"
#include "crate/db/crateserverprofiles.h"
#include "crate/db/crateserverschema.h"
#include "crate/session/cratesecretstore.h"
#include "util/db/dbconnectionpooled.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::CrateServers");

CrateServers* s_pInstance = nullptr;

/// Long enough for a sign-out to renew a stale token and revoke the session
/// (two requests of at most the Client's transfer timeout each).
constexpr auto kRetiredServerLifetime = std::chrono::seconds(70);

} // namespace

struct CrateServers::Server {
    // The Session goes first: it unsets itself as the Client's TokenSource.
    std::unique_ptr<Client> pClient;
    std::unique_ptr<Session> pSession;
    bool saved = false;
};

CrateServers::CrateServers(Dependencies dependencies, QObject* parent)
        : QObject(parent),
          m_dependencies(std::move(dependencies)) {
    if (!m_dependencies.pNetworkAccessManager) {
        m_pOwnNetworkAccessManager = std::make_unique<QNetworkAccessManager>();
        m_dependencies.pNetworkAccessManager = m_pOwnNetworkAccessManager.get();
    }
    if (!m_dependencies.pSecretStore) {
        m_pOwnSecretStore = std::make_unique<KeychainSecretStore>();
        m_dependencies.pSecretStore = m_pOwnSecretStore.get();
    }
}

CrateServers::~CrateServers() {
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
    // Sessions before the stores and the network they use.
    m_servers.clear();
    m_retired.clear();
}

CrateServers* CrateServers::instance() {
    return s_pInstance;
}

void CrateServers::setInstance(CrateServers* pInstance) {
    s_pInstance = pInstance;
}

void CrateServers::start() {
    if (!db::createCrateServerSchema(database())) {
        kLogger.warning() << "Crate tables unavailable: no Crate servers this run";
        return;
    }
    for (const ServerProfile& profile : db::loadServerProfiles(database())) {
        createServer(profile, true)->pSession->restore();
    }
    emit serversChanged();
}

QList<Session*> CrateServers::sessions() const {
    QList<Session*> sessions;
    for (const auto& pServer : m_servers) {
        sessions.append(pServer->pSession.get());
    }
    return sessions;
}

Session* CrateServers::session(const QString& serverId) const {
    for (const auto& pServer : m_servers) {
        if (pServer->pSession->profile().id == serverId) {
            return pServer->pSession.get();
        }
    }
    return nullptr;
}

Client* CrateServers::client(const QString& serverId) const {
    for (const auto& pServer : m_servers) {
        if (pServer->pSession->profile().id == serverId) {
            return pServer->pClient.get();
        }
    }
    return nullptr;
}

Session* CrateServers::addServer(const QUrl& origin, const QString& displayName) {
    ServerProfile profile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.baseUrl = origin;
    profile.displayName = displayName;
    Session* pSession = createServer(profile, false)->pSession.get();
    emit serversChanged();
    return pSession;
}

void CrateServers::removeServer(const QString& serverId) {
    const auto it = std::find_if(m_servers.begin(), m_servers.end(), [&](const auto& pServer) {
        return pServer->pSession->profile().id == serverId;
    });
    if (it == m_servers.end()) {
        return;
    }
    std::unique_ptr<Server> pServer = std::move(*it);
    m_servers.erase(it);
    pServer->pSession->logout();
    if (pServer->saved) {
        db::removeServerProfile(database(), serverId);
    }
    // Keep its Client until the sign-out has reached the server.
    Server* const pRetired = pServer.get();
    m_retired.push_back(std::move(pServer));
    QTimer::singleShot(kRetiredServerLifetime, this, [this, pRetired] {
        m_retired.erase(std::remove_if(m_retired.begin(),
                                m_retired.end(),
                                [pRetired](const auto& pServer) {
                                    return pServer.get() == pRetired;
                                }),
                m_retired.end());
    });
    emit serversChanged();
}

void CrateServers::rememberCapabilities(
        const QString& serverId, const QByteArray& capabilitiesJson) {
    for (const auto& pServer : m_servers) {
        if (pServer->pSession->profile().id == serverId && pServer->saved) {
            db::saveLastCapabilities(database(), serverId, capabilitiesJson);
            return;
        }
    }
}

CrateServers::Server* CrateServers::createServer(const ServerProfile& profile, bool saved) {
    auto pServer = std::make_unique<Server>();
    ClientOptions options;
    options.deviceFingerprint = deviceFingerprint(m_dependencies.pConfig);
    pServer->pClient = std::make_unique<Client>(
            profile.baseUrl, m_dependencies.pNetworkAccessManager, std::move(options));
    pServer->pSession = std::make_unique<Session>(
            profile, pServer->pClient.get(), m_dependencies.pSecretStore);
    pServer->saved = saved;

    Server* const pCreated = pServer.get();
    connect(pCreated->pSession.get(), &Session::signedIn, this, [this, pCreated] {
        if (!pCreated->saved) {
            pCreated->saved = true;
            save(pCreated);
        }
    });
    connect(pCreated->pSession.get(), &Session::profileChanged, this, [this, pCreated] {
        if (pCreated->saved) {
            save(pCreated);
        }
    });
    m_servers.push_back(std::move(pServer));
    return pCreated;
}

void CrateServers::save(Server* pServer) {
    if (!db::saveServerProfile(database(), pServer->pSession->profile())) {
        kLogger.warning() << "Cannot save the Crate server" << pServer->pSession->profile().id;
        return;
    }
    emit serversChanged();
}

QSqlDatabase CrateServers::database() const {
    return mixxx::DbConnectionPooled(m_dependencies.pDbConnectionPool);
}

} // namespace crate

#include "moc_crateservers.cpp"
