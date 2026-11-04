#include <gtest/gtest.h>

#include <QString>
#include <QUrl>

#include "crate/client/crateserveraddress.h"

namespace {

using crate::parseServerAddress;
using Security = crate::ServerAddress::Security;

TEST(CratePreferencesTest, AddServerRequiresHttpsExceptLoopbackAndPrivate) {
    struct Case {
        const char* input;
        Security security;
        const char* origin;
    };
    const Case cases[] = {
            {"crate.example.com",
                    Security::Secure,
                    "https://crate.example.com"},
            {"https://Crate.Example.com/admin?x=1#top",
                    Security::Secure,
                    "https://crate.example.com"},
            {"https://crate.example.com:8443/",
                    Security::Secure,
                    "https://crate.example.com:8443"},
            {"http://localhost:8585",
                    Security::Secure,
                    "http://localhost:8585"},
            {"http://127.0.0.1:8585",
                    Security::Secure,
                    "http://127.0.0.1:8585"},
            {"http://[::1]:8585", Security::Secure, "http://[::1]:8585"},
            {"http://192.168.1.10:8585",
                    Security::LocalNetworkHttp,
                    "http://192.168.1.10:8585"},
            {"http://10.0.0.5", Security::LocalNetworkHttp, "http://10.0.0.5"},
            {"http://172.20.1.1",
                    Security::LocalNetworkHttp,
                    "http://172.20.1.1"},
            {"http://nas:8585", Security::LocalNetworkHttp, "http://nas:8585"},
            {"http://crate.local",
                    Security::LocalNetworkHttp,
                    "http://crate.local"},
            {"http://crate.home.arpa",
                    Security::LocalNetworkHttp,
                    "http://crate.home.arpa"},
            {"http://crate.example.com", Security::Rejected, ""},
            {"http://172.32.0.1", Security::Rejected, ""},
            {"http://8.8.8.8", Security::Rejected, ""},
            {"ftp://crate.example.com", Security::Rejected, ""},
            {"https://dj:secret@crate.example.com", Security::Rejected, ""},
            {"", Security::Rejected, ""},
            {"https://", Security::Rejected, ""},
    };

    for (const Case& testCase : cases) {
        const crate::ServerAddress address = parseServerAddress(QString::fromUtf8(testCase.input));
        EXPECT_EQ(testCase.security, address.security) << testCase.input;
        if (testCase.security == Security::Rejected) {
            EXPECT_FALSE(address.problem.isEmpty()) << testCase.input;
        } else {
            EXPECT_EQ(QUrl(QString::fromUtf8(testCase.origin)), address.origin) << testCase.input;
        }
    }
}

} // namespace
