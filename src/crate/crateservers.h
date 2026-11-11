#pragma once

#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QUrl>
#include <memory>
#include <vector>

#include "crate/session/cratesession.h"
#include "preferences/usersettings.h"
#include "util/db/dbconnectionpool.h"

class QNetworkAccessManager;

namespace crate {

class Client;
class SecretStore;

namespace analysis {
class ProfileStore;
} // namespace analysis

namespace cache {
class AudioCache;
} // namespace cache

/// The Crate servers of this installation, each with its Client and Session.
///
/// CoreServices creates it at start (`CRATE` builds); the interface reaches
/// it through instance(). A server added with addServer() lives in memory
/// until its session signs in; then its profile is saved in
/// `crateserver_servers` and restored at every start.
class CrateServers : public QObject {
    Q_OBJECT
  public:
    struct Dependencies {
        /// For the installation's device fingerprint.
        UserSettingsPointer pConfig;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// Not owned. Without one, CrateServers makes its own.
        QNetworkAccessManager* pNetworkAccessManager = nullptr;
        /// Not owned. Without one, the OS keychain is used.
        SecretStore* pSecretStore = nullptr;
        /// The audio cache's directory; empty for
        /// cache::AudioCache::defaultDirectory().
        QString audioCacheDirectory;
    };

    explicit CrateServers(const Dependencies& dependencies, QObject* parent = nullptr);
    ~CrateServers() override;

    /// The instance CoreServices created, or nullptr.
    static CrateServers* instance();
    static void setInstance(CrateServers* pInstance);

    /// Creates the Crate tables, loads the saved servers and restores their
    /// sessions from the keychain.
    void start();

    QList<Session*> sessions() const;
    Session* session(const QString& serverId) const;
    Client* client(const QString& serverId) const;

    /// A new server at `origin`. Returns its session, to sign in with; the
    /// server is kept once it signs in. Returns nullptr for an address that
    /// parseServerAddress() rejects, such as plain HTTP on the internet.
    Session* addServer(const QUrl& origin, const QString& displayName);
    /// Signs out of the server, revoking its session, and forgets it.
    void removeServer(const QString& serverId);
    /// Keeps the last `GET /api/capabilities` answer of a saved server.
    void rememberCapabilities(const QString& serverId, const QByteArray& capabilitiesJson);
    /// That answer, or empty if there is none.
    QByteArray lastCapabilities(const QString& serverId) const;

    /// The audio cache of every server (design D4), created the first time
    /// it is asked for.
    cache::AudioCache* audioCache();
    /// The mix profiles of a server; nullptr for an unknown server. Created
    /// the first time they are asked for.
    analysis::ProfileStore* profileStore(const QString& serverId);

    /// Whether a server with this origin is kept or being added.
    bool hasOrigin(const QUrl& origin) const;
    /// Whether the server is saved: signed in once and stored.
    bool isSaved(const QString& serverId) const;

  signals:
    void serversChanged();

  private:
    struct Server;
    Server* createServer(const ServerProfile& profile, bool saved);
    bool save(Server* pServer);
    QSqlDatabase database() const;

    Dependencies m_dependencies;
    std::unique_ptr<QNetworkAccessManager> m_pOwnNetworkAccessManager;
    std::unique_ptr<SecretStore> m_pOwnSecretStore;
    std::vector<std::unique_ptr<Server>> m_servers;
    /// Removed servers, kept until their sign-out reached the server.
    std::vector<std::unique_ptr<Server>> m_retired;
    std::unique_ptr<cache::AudioCache> m_pAudioCache;
};

} // namespace crate
