#pragma once

#include <QHash>
#include <QString>
#include <chrono>
#include <functional>

namespace crate {

/// Where Crate Decks keeps the refresh tokens of its sessions. Secrets live
/// only here (AGENTS.md §5.2), never in mixxx.cfg, logs or the database.
///
/// Operations are asynchronous: `done` is called exactly once, possibly
/// before the call returns.
class SecretStore {
  public:
    enum class Status {
        Ok,
        /// There is no secret under that key.
        NotFound,
        /// The store cannot be used: no keychain, locked, denied or too slow.
        Unavailable,
    };
    using ReadDone = std::function<void(Status status, const QString& secret)>;
    using Done = std::function<void(Status status)>;

    virtual ~SecretStore() = default;

    virtual void read(const QString& key, ReadDone done) = 0;
    virtual void write(const QString& key, const QString& secret, Done done) = 0;
    virtual void remove(const QString& key, Done done) = 0;
};

/// Key of the refresh token of a server: "<server_id>:refresh".
QString refreshTokenKey(const QString& serverId);

/// Keeps the secrets in memory for this run only. Used when the keychain is
/// unavailable (F0 Q11): the user signs in again after a restart.
class SessionOnlySecretStore : public SecretStore {
  public:
    void read(const QString& key, ReadDone done) override;
    void write(const QString& key, const QString& secret, Done done) override;
    void remove(const QString& key, Done done) override;

  private:
    QHash<QString, QString> m_secrets;
};

/// The OS keychain through QtKeychain, service `app.cratemusic.decks`.
///
/// Each operation gives up after `timeout` and reports Unavailable, without
/// a nested event loop: without a Secret Service, QtKeychain waits until
/// D-Bus gives up (F0 Q11). A job that finishes after its timeout is ignored.
/// Without QtKeychain in the build, every operation reports Unavailable.
class KeychainSecretStore : public SecretStore {
  public:
    explicit KeychainSecretStore(std::chrono::milliseconds timeout = std::chrono::seconds(3));

    void read(const QString& key, ReadDone done) override;
    void write(const QString& key, const QString& secret, Done done) override;
    void remove(const QString& key, Done done) override;

  private:
    const std::chrono::milliseconds m_timeout;
};

} // namespace crate
