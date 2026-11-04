#pragma once

#include <QHash>
#include <QString>
#include <chrono>
#include <functional>
#include <memory>

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

/// The operations of the OS keychain, for KeychainSecretStore. Tests replace
/// it to decide when each operation finishes.
class KeychainBackend {
  public:
    enum class Operation {
        Read,
        Write,
        Remove,
    };
    using Finished = std::function<void(SecretStore::Status status, const QString& secret)>;

    virtual ~KeychainBackend() = default;

    /// Runs one operation and calls `finished` exactly once, however late.
    virtual void run(Operation operation,
            const QString& key,
            const QString& secret,
            Finished finished) = 0;
};

/// QtKeychain with the service `app.cratemusic.decks`. Without QtKeychain in
/// the build, every operation finishes as Unavailable.
std::unique_ptr<KeychainBackend> makeQtKeychainBackend();

/// The OS keychain.
///
/// - Each operation reports Unavailable after `timeout` if the keychain has
///   not answered, without a nested event loop: without a Secret Service,
///   QtKeychain waits until D-Bus gives up (F0 Q11). A later answer is
///   ignored.
/// - The operations on one key run one at a time, in the order they were
///   asked for, even after a timeout: a remove asked after a slow write
///   always runs after it.
class KeychainSecretStore : public SecretStore {
  public:
    using Schedule = std::function<void(std::chrono::milliseconds delay,
            std::function<void()> callback)>;

    /// `schedule` defaults to a single-shot QTimer; tests replace it.
    explicit KeychainSecretStore(std::chrono::milliseconds timeout = std::chrono::seconds(3),
            std::unique_ptr<KeychainBackend> pBackend = makeQtKeychainBackend(),
            Schedule schedule = Schedule());
    ~KeychainSecretStore() override;

    void read(const QString& key, ReadDone done) override;
    void write(const QString& key, const QString& secret, Done done) override;
    void remove(const QString& key, Done done) override;

  private:
    struct State;
    void enqueue(KeychainBackend::Operation operation,
            const QString& key,
            const QString& secret,
            KeychainBackend::Finished done);

    // Shared with the operations in flight, which may finish after this
    // store is gone.
    std::shared_ptr<State> m_pState;
};

} // namespace crate
