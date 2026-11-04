#include "crate/session/cratesecretstore.h"

#include <gtest/gtest.h>

#include <QList>
#include <memory>
#include <optional>

#include "test/crate/fakecratenetwork.h"

namespace {

using crate::SecretStore;
using Status = crate::SecretStore::Status;

using namespace std::chrono_literals;
using crate::KeychainBackend;
using crate::test::FakeScheduler;

// TEST_GAP: the QtKeychain backend talks to the system keychain (Secret
// Service, macOS Keychain, Windows Credential Manager), so it is only tested
// by hand (F0 Q11). KeychainSecretStore is tested over a fake backend.

/// A keychain whose operations the test finishes by hand.
class FakeKeychainBackend : public KeychainBackend {
  public:
    struct Run {
        Operation operation;
        QString key;
        QString secret;
        Finished finished;
    };

    explicit FakeKeychainBackend(std::shared_ptr<QList<Run>> pRuns)
            : m_pRuns(std::move(pRuns)) {
    }

    void run(Operation operation,
            const QString& key,
            const QString& secret,
            Finished finished) override {
        m_pRuns->append({operation, key, secret, std::move(finished)});
    }

  private:
    std::shared_ptr<QList<Run>> m_pRuns;
};

class CrateKeychainSecretStoreTest : public testing::Test {
  protected:
    CrateKeychainSecretStoreTest()
            : m_pRuns(std::make_shared<QList<FakeKeychainBackend::Run>>()),
              m_store(3s, std::make_unique<FakeKeychainBackend>(m_pRuns), m_scheduler.function()) {
    }

    std::shared_ptr<QList<FakeKeychainBackend::Run>> m_pRuns;
    FakeScheduler m_scheduler;
    crate::KeychainSecretStore m_store;
};

TEST_F(CrateKeychainSecretStoreTest, AnswersAnOperationThatFinishesInTime) {
    QList<std::pair<Status, QString>> answers;
    m_store.read(QStringLiteral("a:refresh"), [&](Status status, const QString& secret) {
        answers.append({status, secret});
    });
    ASSERT_EQ(1, m_pRuns->size());
    EXPECT_EQ(KeychainBackend::Operation::Read, m_pRuns->first().operation);

    m_pRuns->first().finished(Status::Ok, QStringLiteral("secret-a"));
    m_scheduler.runAll();

    ASSERT_EQ(1, answers.size());
    EXPECT_EQ(Status::Ok, answers.first().first);
    EXPECT_EQ(QStringLiteral("secret-a"), answers.first().second);
}

TEST_F(CrateKeychainSecretStoreTest, GivesUpAfterTheTimeoutAndIgnoresTheLateAnswer) {
    QList<Status> answers;
    m_store.read(QStringLiteral("a:refresh"), [&](Status status, const QString&) {
        answers.append(status);
    });
    ASSERT_EQ(1, m_scheduler.tasks.size());
    EXPECT_EQ(std::chrono::milliseconds(3s), m_scheduler.tasks.first().delay);

    m_scheduler.runAll();
    m_pRuns->first().finished(Status::Ok, QStringLiteral("secret-a"));

    EXPECT_EQ(QList<Status>({Status::Unavailable}), answers);
}

TEST_F(CrateKeychainSecretStoreTest, RunsTheOperationsOfAKeyInOrder) {
    m_store.write(QStringLiteral("a:refresh"), QStringLiteral("secret-a"), [](Status) {});
    m_store.remove(QStringLiteral("a:refresh"), [](Status) {});
    m_store.write(QStringLiteral("b:refresh"), QStringLiteral("secret-b"), [](Status) {});
    ASSERT_EQ(2, m_pRuns->size());
    EXPECT_EQ(QStringLiteral("b:refresh"), m_pRuns->at(1).key);

    // The write is slow: both answers time out, the remove still waits.
    m_scheduler.runAll();
    EXPECT_EQ(2, m_pRuns->size());
    m_pRuns->first().finished(Status::Ok, QString());

    ASSERT_EQ(3, m_pRuns->size());
    EXPECT_EQ(KeychainBackend::Operation::Remove, m_pRuns->at(2).operation);
    EXPECT_EQ(QStringLiteral("a:refresh"), m_pRuns->at(2).key);
}

TEST(CrateSecretStoreTest, RefreshTokenKeyIsPerServer) {
    EXPECT_EQ(QStringLiteral("5b0c:refresh"), crate::refreshTokenKey(QStringLiteral("5b0c")));
}

TEST(CrateSecretStoreTest, SessionOnlyStoreKeepsSecretsInMemory) {
    crate::SessionOnlySecretStore store;
    std::optional<Status> written;
    std::optional<Status> readStatus;
    QString readSecret;
    std::optional<Status> removed;
    std::optional<Status> readAfterRemove;

    store.write(QStringLiteral("a:refresh"), QStringLiteral("secret-a"), [&](Status status) {
        written = status;
    });
    store.read(QStringLiteral("a:refresh"), [&](Status status, const QString& secret) {
        readStatus = status;
        readSecret = secret;
    });
    store.remove(QStringLiteral("a:refresh"), [&](Status status) {
        removed = status;
    });
    store.read(QStringLiteral("a:refresh"), [&](Status status, const QString&) {
        readAfterRemove = status;
    });

    EXPECT_EQ(Status::Ok, written);
    EXPECT_EQ(Status::Ok, readStatus);
    EXPECT_EQ(QStringLiteral("secret-a"), readSecret);
    EXPECT_EQ(Status::Ok, removed);
    EXPECT_EQ(Status::NotFound, readAfterRemove);
}

} // namespace
