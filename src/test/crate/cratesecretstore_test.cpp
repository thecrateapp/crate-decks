#include "crate/session/cratesecretstore.h"

#include <gtest/gtest.h>

#include <optional>

namespace {

using crate::SecretStore;
using Status = crate::SecretStore::Status;

// TEST_GAP: KeychainSecretStore talks to the system keychain (Secret
// Service, macOS Keychain, Windows Credential Manager), so it is only tested
// by hand (F0 Q11). Session tests use a fake SecretStore.

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
