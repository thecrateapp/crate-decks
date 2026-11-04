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
    };

    explicit CrateServers(Dependencies dependencies, QObject* parent = nullptr);
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

    /// A new server at `origin` (see parseServerAddress()). Returns its
    /// session, to sign in with; the server is kept once it signs in.
    Session* addServer(const QUrl& origin, const QString& displayName);
    /// Signs out of the server, revoking its session, and forgets it.
    void removeServer(const QString& serverId);
    /// Keeps the last `GET /api/capabilities` answer of a saved server.
    void rememberCapabilities(const QString& serverId, const QByteArray& capabilitiesJson);
    /// That answer, or empty if there is none.
    QByteArray lastCapabilities(const QString& serverId) const;

  signals:
    void serversChanged();

  private:
    struct Server;
    Server* createServer(const ServerProfile& profile, bool saved);
    void save(Server* pServer);
    QSqlDatabase database() const;

    Dependencies m_dependencies;
    std::unique_ptr<QNetworkAccessManager> m_pOwnNetworkAccessManager;
    std::unique_ptr<SecretStore> m_pOwnSecretStore;
    std::vector<std::unique_ptr<Server>> m_servers;
    /// Removed servers, kept until their sign-out reached the server.
    std::vector<std::unique_ptr<Server>> m_retired;
};

} // namespace crate
