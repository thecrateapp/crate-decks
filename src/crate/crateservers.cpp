#include "crate/crateservers.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkInformation>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <chrono>

#include "crate/analysis/profilestore.h"
#include "crate/cache/audiocache.h"
#include "crate/client/cratecapabilities.h"
#include "crate/client/crateclient.h"
#include "crate/client/crateserveraddress.h"
#include "crate/cratepreferences.h"
#include "crate/db/crateserverprofiles.h"
#include "crate/db/crateserverschema.h"
#include "crate/session/cratesecretstore.h"
#include "crate/stems/stemsidecar.h"
#include "crate/writeback/cratecorrections.h"
#include "crate/writeback/cratecuesync.h"
#include "crate/writeback/cratelikes.h"
#include "crate/writeback/crateratings.h"
#include "crate/writeback/historyreporter.h"
#include "crate/writeback/outbox.h"
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
          m_dependencies(dependencies),
          m_pPreferences(std::make_unique<CratePreferences>(dependencies.pConfig)) {
    if (!m_dependencies.pNetworkAccessManager) {
        m_pOwnNetworkAccessManager = std::make_unique<QNetworkAccessManager>();
        m_dependencies.pNetworkAccessManager = m_pOwnNetworkAccessManager.get();
    }
    if (!m_dependencies.pSecretStore) {
        m_pOwnSecretStore = std::make_unique<KeychainSecretStore>();
        m_dependencies.pSecretStore = m_pOwnSecretStore.get();
    }
    createWriteback();
}

CrateServers::~CrateServers() {
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
    // The track on air gets its play event, which the outbox stores for the
    // next run
    m_pHistory.reset();
    // The cue edits waiting for their debounce go to the outbox, which keeps
    // them for the next run
    if (m_pCueSync) {
        m_pCueSync->flushEdits();
    }
    m_pCueSync.reset();
    m_pCorrections.reset();
    m_pRatings.reset();
    m_pLikes.reset();
    m_pOutbox.reset();
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
    m_pOutbox->start();
    m_pRatings->start();
    m_pCueSync->start();
    m_pCorrections->start();
    emit serversChanged();
}

void CrateServers::createWriteback() {
    writeback::Outbox::Options outbox;
    outbox.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    outbox.clientFor = [this](const QString& serverId) -> Client* {
        const Session* pSession = session(serverId);
        return pSession && pSession->state() == Session::State::SignedIn ? client(serverId)
                                                                         : nullptr;
    };
    m_pOutbox = std::make_unique<writeback::Outbox>(std::move(outbox));
    // The network coming back sends what waits, without its backoff. Any
    // change of the network asks the servers again whether they answer.
    if (QNetworkInformation::loadDefaultBackend()) {
        if (QNetworkInformation* pInformation = QNetworkInformation::instance()) {
            connect(pInformation,
                    &QNetworkInformation::reachabilityChanged,
                    m_pOutbox.get(),
                    [pOutbox = m_pOutbox.get()](QNetworkInformation::Reachability reachability) {
                        if (reachability == QNetworkInformation::Reachability::Online) {
                            pOutbox->flush();
                        }
                    });
            connect(pInformation,
                    &QNetworkInformation::reachabilityChanged,
                    this,
                    &CrateServers::checkReachability);
        }
    }

    writeback::HistoryReporter::Options history;
    history.pOutbox = m_pOutbox.get();
    // A Crate track is one whose file is in the audio cache
    const auto trackOfFile = [this](const QString& location) -> std::optional<cache::TrackKey> {
        return m_pAudioCache ? m_pAudioCache->trackOfFile(location) : std::nullopt;
    };
    history.trackOfFile = trackOfFile;
    history.clientFor = [this](const QString& serverId) -> Client* {
        const Session* pSession = session(serverId);
        return pSession && pSession->state() == Session::State::SignedIn ? client(serverId)
                                                                         : nullptr;
    };
    history.minimumPlayed = std::chrono::seconds(m_pPreferences->historyMinimumSeconds());
    m_pHistory = std::make_unique<writeback::HistoryReporter>(std::move(history));

    m_pLikes = std::make_unique<writeback::CrateLikes>(
            writeback::CrateLikes::Options{m_pOutbox.get(), m_dependencies.pDbConnectionPool});
    writeback::CrateRatings::Options ratings;
    ratings.pOutbox = m_pOutbox.get();
    ratings.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    ratings.trackOfFile = trackOfFile;
    m_pRatings = std::make_unique<writeback::CrateRatings>(std::move(ratings));

    // The user's cues and corrections (plan F8), from servers that serve them
    writeback::TrackDataSync::Options sync;
    sync.pOutbox = m_pOutbox.get();
    sync.pDbConnectionPool = m_dependencies.pDbConnectionPool;
    sync.clientFor = [this](const QString& serverId) -> Client* {
        const Session* pSession = session(serverId);
        return pSession && pSession->state() == Session::State::SignedIn ? client(serverId)
                                                                         : nullptr;
    };
    sync.enabledFor = [this](const QString& serverId) {
        return serverHas(serverId, DecksFeature::Cues);
    };
    if (m_dependencies.pConfig) {
        sync.clientId = deviceFingerprint(m_dependencies.pConfig);
    }
    m_pCueSync = std::make_unique<writeback::CrateCueSync>(sync);
    m_pCorrections = std::make_unique<writeback::CrateCorrections>(std::move(sync));
}

bool CrateServers::serverHas(const QString& serverId, DecksFeature feature) const {
    auto it = m_features.find(serverId);
    if (it == m_features.end()) {
        // Read once per server and answer: every load asks
        std::optional<Capabilities> parsed;
        const QByteArray json = isSaved(serverId) ? lastCapabilities(serverId) : QByteArray();
        if (!json.isEmpty()) {
            const Result<Capabilities> capabilities =
                    Capabilities::fromJson(QJsonDocument::fromJson(json));
            if (capabilities.ok()) {
                parsed = capabilities.value();
            }
        }
        it = m_features.insert(serverId, parsed);
    }
    return it.value() && it.value()->has(feature);
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
    m_features.remove(serverId);
    m_reachability.remove(serverId);
    m_checking.remove(serverId);
    // Its sign-out still answers: not for a server that is gone
    disconnect(pServer->pClient.get(), &Client::answered, this, nullptr);
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
            m_features.remove(serverId);
            if (pServer->pProfileStore) {
                const Result<Capabilities> capabilities =
                        Capabilities::fromJson(QJsonDocument::fromJson(capabilitiesJson));
                if (capabilities.ok()) {
                    pServer->pProfileStore->setCapabilities(capabilities.value());
                }
            }
            emit capabilitiesChanged(serverId);
            return;
        }
    }
}

cache::AudioCache* CrateServers::audioCache() {
    if (!m_pAudioCache) {
        cache::AudioCache::Options options;
        options.directory = m_dependencies.audioCacheDirectory;
        if (options.directory.isEmpty()) {
            options.directory = m_pPreferences->cacheDirectory();
        }
        if (options.directory.isEmpty()) {
            options.directory = cache::AudioCache::defaultDirectory();
        }
        options.clientFor = [this](const QString& serverId) {
            return client(serverId);
        };
        options.limitBytes = m_pPreferences->cacheLimitBytes();
        // A track's stem sidecar counts in its size and goes with it
        options.companionsOf = [](const QString& trackPath) {
            return QStringList{StemSidecar::pathFor(trackPath)};
        };
        m_pAudioCache = std::make_unique<cache::AudioCache>(std::move(options));
        // A new directory waits for the next start: the cache is in use
        connect(m_pPreferences.get(),
                &CratePreferences::cacheLimitChanged,
                m_pAudioCache.get(),
                &cache::AudioCache::setLimitBytes);
        emit audioCacheCreated();
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

Reachability CrateServers::reachability(const QString& serverId) const {
    return m_reachability.value(serverId, Reachability::Unknown);
}

void CrateServers::setReachability(const QString& serverId, Reachability reachability) {
    if (m_reachability.value(serverId, Reachability::Unknown) == reachability) {
        return;
    }
    m_reachability.insert(serverId, reachability);
    emit reachabilityChanged(serverId);
}

void CrateServers::checkReachability() {
    for (const auto& pServer : m_servers) {
        const QString serverId = pServer->pSession->profile().id;
        if (!pServer->saved || pServer->pSession->state() != Session::State::SignedIn ||
                m_checking.contains(serverId)) {
            continue;
        }
        m_checking.insert(serverId);
        RequestOptions options;
        options.authenticated = false;
        options.rawBody = true;
        Reply* pReply = pServer->pClient->get(QStringLiteral("/api/capabilities"), options);
        // The Client's answered() sets the reachability; a new answer also
        // updates the server's features
        connect(pReply, &Reply::finished, this, [this, serverId, pReply](const JsonResult& result) {
            m_checking.remove(serverId);
            if (result.ok() && pReply->httpStatus() == 200 && isSaved(serverId) &&
                    pReply->body() != lastCapabilities(serverId) &&
                    Capabilities::fromJson(QJsonDocument::fromJson(pReply->body())).ok()) {
                rememberCapabilities(serverId, pReply->body());
            }
        });
    }
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
    // Every request says whether the server answers
    const QString serverId = profile.id;
    connect(pCreated->pClient.get(), &Client::answered, this, [this, serverId](bool reachable) {
        setReachability(serverId, reachable ? Reachability::Online : Reachability::Offline);
    });
    connect(pCreated->pSession.get(), &Session::signedIn, this, [this, pCreated] {
        // Marked as saved before anyone hears of it.
        if (!pCreated->saved && save(pCreated)) {
            pCreated->saved = true;
            emit serversChanged();
        }
    });
    // What waited for the session goes out
    connect(pCreated->pSession.get(),
            &Session::signedIn,
            m_pOutbox.get(),
            &writeback::Outbox::flush);
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
    m_features.remove(pServer->pSession->profile().id);
    return true;
}

QSqlDatabase CrateServers::database() const {
    return mixxx::DbConnectionPooled(m_dependencies.pDbConnectionPool);
}

} // namespace crate

#include "moc_crateservers.cpp"
