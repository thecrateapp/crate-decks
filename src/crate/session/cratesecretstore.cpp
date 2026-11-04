#include "crate/session/cratesecretstore.h"

#include <QObject>
#include <QTimer>
#include <memory>
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

/// Starts `pJob` and calls `onFinished` when it finishes or `onTimeout`
/// after `timeout`, whichever comes first, and only that one. The job
/// deletes itself when it finishes, also after a timeout.
template<typename OnFinished, typename OnTimeout>
void runJob(QKeychain::Job* pJob,
        std::chrono::milliseconds timeout,
        OnFinished onFinished,
        OnTimeout onTimeout) {
    const auto pAnswered = std::make_shared<bool>(false);
    pJob->setAutoDelete(true);
    QObject::connect(pJob,
            &QKeychain::Job::finished,
            pJob,
            [pAnswered, onFinished](QKeychain::Job* pFinished) {
                if (!std::exchange(*pAnswered, true)) {
                    onFinished(*pFinished);
                }
            });
    // Bound to the job: it is cancelled if the job finishes and is deleted.
    QTimer::singleShot(timeout, pJob, [pAnswered, onTimeout] {
        if (!std::exchange(*pAnswered, true)) {
            kLogger.warning() << "Keychain did not answer in time";
            onTimeout();
        }
    });
    pJob->start();
}
#endif

} // namespace

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

KeychainSecretStore::KeychainSecretStore(std::chrono::milliseconds timeout)
        : m_timeout(timeout) {
}

void KeychainSecretStore::read(const QString& key, ReadDone done) {
#ifdef __QTKEYCHAIN__
    auto* pJob = new QKeychain::ReadPasswordJob(kKeychainService);
    pJob->setKey(key);
    runJob(
            pJob,
            m_timeout,
            [done, pJob](const QKeychain::Job& job) {
                const Status status = statusOf(job);
                done(status, status == Status::Ok ? pJob->textData() : QString());
            },
            [done] { done(Status::Unavailable, QString()); });
#else
    Q_UNUSED(key);
    done(Status::Unavailable, QString());
#endif
}

void KeychainSecretStore::write(const QString& key, const QString& secret, Done done) {
#ifdef __QTKEYCHAIN__
    auto* pJob = new QKeychain::WritePasswordJob(kKeychainService);
    pJob->setKey(key);
    pJob->setTextData(secret);
    runJob(
            pJob,
            m_timeout,
            [done](const QKeychain::Job& job) { done(statusOf(job)); },
            [done] { done(Status::Unavailable); });
#else
    Q_UNUSED(key);
    Q_UNUSED(secret);
    done(Status::Unavailable);
#endif
}

void KeychainSecretStore::remove(const QString& key, Done done) {
#ifdef __QTKEYCHAIN__
    auto* pJob = new QKeychain::DeletePasswordJob(kKeychainService);
    pJob->setKey(key);
    runJob(
            pJob,
            m_timeout,
            [done](const QKeychain::Job& job) { done(statusOf(job)); },
            [done] { done(Status::Unavailable); });
#else
    Q_UNUSED(key);
    done(Status::Unavailable);
#endif
}

} // namespace crate
