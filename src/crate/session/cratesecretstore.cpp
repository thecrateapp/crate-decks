#include "crate/session/cratesecretstore.h"

#include <QHash>
#include <QObject>
#include <QTimer>
#include <deque>
#include <utility>

#ifdef __QTKEYCHAIN__
#include <qt6keychain/keychain.h>
#endif

#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::SecretStore");

#ifdef __QTKEYCHAIN__
/// Keychain service of Crate Decks (design D3), never Mixxx's.
const QString kKeychainService = QStringLiteral("app.cratemusic.decks");

SecretStore::Status statusOf(const QKeychain::Job& job) {
    switch (job.error()) {
    case QKeychain::NoError:
        return SecretStore::Status::Ok;
    case QKeychain::EntryNotFound:
        return SecretStore::Status::NotFound;
    default:
        kLogger.warning() << "Keychain unavailable:" << static_cast<int>(job.error())
                          << job.errorString();
        return SecretStore::Status::Unavailable;
    }
}
#endif

class QtKeychainBackend : public KeychainBackend {
  public:
    void run(Operation operation,
            const QString& key,
            const QString& secret,
            Finished finished) override {
#ifdef __QTKEYCHAIN__
        QKeychain::Job* pJob = nullptr;
        switch (operation) {
        case Operation::Read:
            pJob = new QKeychain::ReadPasswordJob(kKeychainService);
            break;
        case Operation::Write: {
            auto* pWrite = new QKeychain::WritePasswordJob(kKeychainService);
            pWrite->setTextData(secret);
            pJob = pWrite;
            break;
        }
        case Operation::Remove:
            pJob = new QKeychain::DeletePasswordJob(kKeychainService);
            break;
        }
        pJob->setAutoDelete(true);
        pJob->setKey(key);
        QObject::connect(pJob,
                &QKeychain::Job::finished,
                pJob,
                [finished](QKeychain::Job* pFinished) {
                    const SecretStore::Status status = statusOf(*pFinished);
                    const auto* pRead = qobject_cast<QKeychain::ReadPasswordJob*>(pFinished);
                    finished(status,
                            status == SecretStore::Status::Ok && pRead ? pRead->textData()
                                                                       : QString());
                });
        pJob->start();
#else
        Q_UNUSED(operation);
        Q_UNUSED(key);
        Q_UNUSED(secret);
        finished(SecretStore::Status::Unavailable, QString());
#endif
    }
};

/// The answer to one caller, given once: by the keychain or by the timeout.
struct Answer {
    bool given = false;
    KeychainBackend::Finished done;

    void give(SecretStore::Status status, const QString& secret) {
        if (std::exchange(given, true)) {
            return;
        }
        const KeychainBackend::Finished callback = std::move(done);
        callback(status, secret);
    }
};

} // namespace

std::unique_ptr<KeychainBackend> makeQtKeychainBackend() {
    return std::make_unique<QtKeychainBackend>();
}

QString refreshTokenKey(const QString& serverId) {
    return serverId + QStringLiteral(":refresh");
}

void SessionOnlySecretStore::read(const QString& key, ReadDone done) {
    const auto it = m_secrets.constFind(key);
    if (it == m_secrets.cend()) {
        done(Status::NotFound, QString());
        return;
    }
    done(Status::Ok, it.value());
}

void SessionOnlySecretStore::write(const QString& key, const QString& secret, Done done) {
    m_secrets.insert(key, secret);
    done(Status::Ok);
}

void SessionOnlySecretStore::remove(const QString& key, Done done) {
    done(m_secrets.remove(key) > 0 ? Status::Ok : Status::NotFound);
}

struct KeychainSecretStore::State {
    struct Pending {
        KeychainBackend::Operation operation;
        QString key;
        QString secret;
        std::shared_ptr<Answer> pAnswer;
    };

    std::chrono::milliseconds timeout;
    std::unique_ptr<KeychainBackend> pBackend;
    Schedule schedule;
    /// Per key, the operation running at the front and the waiting ones.
    QHash<QString, std::deque<Pending>> queues;

    static void startNext(const std::shared_ptr<State>& pState, const QString& key) {
        const auto it = pState->queues.find(key);
        if (it == pState->queues.end() || it->empty()) {
            return;
        }
        const Pending& next = it->front();
        // The State stays alive until the keychain answers, also when the
        // store is gone by then.
        pState->pBackend->run(next.operation,
                next.key,
                next.secret,
                [pState, key, pAnswer = next.pAnswer](
                        SecretStore::Status status, const QString& secret) {
                    pAnswer->give(status, secret);
                    const auto done = pState->queues.find(key);
                    if (done == pState->queues.end()) {
                        return;
                    }
                    done->pop_front();
                    if (done->empty()) {
                        pState->queues.erase(done);
                    } else {
                        startNext(pState, key);
                    }
                });
    }
};

KeychainSecretStore::KeychainSecretStore(std::chrono::milliseconds timeout,
        std::unique_ptr<KeychainBackend> pBackend,
        Schedule schedule)
        : m_pState(std::make_shared<State>()) {
    m_pState->timeout = timeout;
    m_pState->pBackend = std::move(pBackend);
    m_pState->schedule = std::move(schedule);
}

KeychainSecretStore::~KeychainSecretStore() = default;

void KeychainSecretStore::read(const QString& key, ReadDone done) {
    enqueue(KeychainBackend::Operation::Read, key, QString(), std::move(done));
}

void KeychainSecretStore::write(const QString& key, const QString& secret, Done done) {
    enqueue(KeychainBackend::Operation::Write,
            key,
            secret,
            [done](Status status, const QString&) { done(status); });
}

void KeychainSecretStore::remove(const QString& key, Done done) {
    enqueue(KeychainBackend::Operation::Remove,
            key,
            QString(),
            [done](Status status, const QString&) { done(status); });
}

void KeychainSecretStore::enqueue(KeychainBackend::Operation operation,
        const QString& key,
        const QString& secret,
        KeychainBackend::Finished done) {
    auto pAnswer = std::make_shared<Answer>();
    pAnswer->done = std::move(done);
    // The caller waits at most the timeout, also while earlier operations on
    // the key are still running.
    auto giveUp = [pAnswer] {
        if (!pAnswer->given) {
            kLogger.warning() << "Keychain did not answer in time";
            pAnswer->give(Status::Unavailable, QString());
        }
    };
    if (m_pState->schedule) {
        m_pState->schedule(m_pState->timeout, std::move(giveUp));
    } else {
        QTimer::singleShot(m_pState->timeout, std::move(giveUp));
    }

    std::deque<State::Pending>& queue = m_pState->queues[key];
    queue.push_back({operation, key, secret, pAnswer});
    if (queue.size() == 1) {
        State::startNext(m_pState, key);
    }
}

} // namespace crate
