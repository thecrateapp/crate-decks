#include "crate/crateservers.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <chrono>

#include "crate/analysis/profilestore.h"
#include "crate/cache/audiocache.h"
#include "crate/client/cratecapabilities.h"
#include "crate/client/crateclient.h"
#include "crate/client/crateserveraddress.h"
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
    // Before the Client it uses.
    std::unique_ptr<analysis::ProfileStore> pProfileStore;
    bool saved = false;
};

CrateServers::CrateServers(const Dependencies& dependencies, QObject* parent)
        : QObject(parent),
          m_dependencies(dependencies) {
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
    // The cache's downloads use the Clients.
    m_pAudioCache.reset();
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
    const QList<ServerProfile> profiles = db::loadServerProfiles(database());
    for (const ServerProfile& profile : profiles) {
        // The address rules hold for stored servers too: a public plain HTTP
        // origin never gets a token.
        if (parseServerAddress(profile.baseUrl.toString()).security ==
                ServerAddress::Security::Rejected) {
            kLogger.warning() << "Skipping the Crate server" << profile.id
                              << "with an unsafe address" << profile.baseUrl;
            continue;
        }
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
    const ServerAddress address = parseServerAddress(origin.toString());
    if (address.security == ServerAddress::Security::Rejected) {
        kLogger.warning() << "Refusing the Crate server address" << origin;
        return nullptr;
    }
    ServerProfile profile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.baseUrl = address.origin;
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
            if (pServer->pProfileStore) {
                const Result<Capabilities> capabilities =
                        Capabilities::fromJson(QJsonDocument::fromJson(capabilitiesJson));
                if (capabilities.ok()) {
                    pServer->pProfileStore->setCapabilities(capabilities.value());
                }
            }
            return;
        }
    }
}

cache::AudioCache* CrateServers::audioCache() {
    if (!m_pAudioCache) {
        cache::AudioCache::Options options;
        options.directory = m_dependencies.audioCacheDirectory.isEmpty()
                ? cache::AudioCache::defaultDirectory()
                : m_dependencies.audioCacheDirectory;
        options.clientFor = [this](const QString& serverId) {
            return client(serverId);
        };
        m_pAudioCache = std::make_unique<cache::AudioCache>(std::move(options));
    }
    return m_pAudioCache.get();
}

analysis::ProfileStore* CrateServers::profileStore(const QString& serverId) {
    const auto it = std::find_if(m_servers.begin(), m_servers.end(), [&](const auto& pServer) {
        return pServer->pSession->profile().id == serverId;
    });
    if (it == m_servers.end()) {
        return nullptr;
    }
    Server* const pServer = it->get();
    if (!pServer->pProfileStore) {
        pServer->pProfileStore = std::make_unique<analysis::ProfileStore>(
                serverId, pServer->pClient.get(), m_dependencies.pDbConnectionPool);
        const Result<Capabilities> capabilities =
                Capabilities::fromJson(QJsonDocument::fromJson(lastCapabilities(serverId)));
        if (capabilities.ok()) {
            pServer->pProfileStore->setCapabilities(capabilities.value());
        }
    }
    return pServer->pProfileStore.get();
}

QByteArray CrateServers::lastCapabilities(const QString& serverId) const {
    return db::loadLastCapabilities(database(), serverId);
}

bool CrateServers::hasOrigin(const QUrl& origin) const {
    const QUrl canonical = parseServerAddress(origin.toString()).origin;
    return std::any_of(m_servers.cbegin(), m_servers.cend(), [&](const auto& pServer) {
        return pServer->pSession->profile().baseUrl == canonical;
    });
}

bool CrateServers::isSaved(const QString& serverId) const {
    return std::any_of(m_servers.cbegin(), m_servers.cend(), [&](const auto& pServer) {
        return pServer->saved && pServer->pSession->profile().id == serverId;
    });
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
        // Marked as saved before anyone hears of it.
        if (!pCreated->saved && save(pCreated)) {
            pCreated->saved = true;
            emit serversChanged();
        }
    });
    // The preferences page shows the keychain state of every session.
    connect(pCreated->pSession.get(),
            &Session::secretStoreUnavailable,
            this,
            &CrateServers::serversChanged);
    connect(pCreated->pSession.get(), &Session::profileChanged, this, [this, pCreated] {
        if (pCreated->saved && save(pCreated)) {
            emit serversChanged();
        }
    });
    m_servers.push_back(std::move(pServer));
    return pCreated;
}

bool CrateServers::save(Server* pServer) {
    if (!db::saveServerProfile(database(), pServer->pSession->profile())) {
        kLogger.warning() << "Cannot save the Crate server" << pServer->pSession->profile().id;
        return false;
    }
    return true;
}

QSqlDatabase CrateServers::database() const {
    return mixxx::DbConnectionPooled(m_dependencies.pDbConnectionPool);
}

} // namespace crate

#include "moc_crateservers.cpp"
