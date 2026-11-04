#include "crate/appidentity.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QString>

#include "util/versionstore.h"

namespace crate {

namespace {

// GitInfo.cmake appends this to `git describe` for a modified work tree.
const QString kModifiedSuffix = QStringLiteral("-modified");
// `git describe` of a commit after a tag: "<tag>-<commits>-g<hash>".
const QRegularExpression kCommitAfterTag(QStringLiteral("-[0-9]+-g([0-9a-f]+)$"));
// Tags of Crate Decks versions, the only ones GitInfo.cmake describes with.
const QString kTagPrefix = QStringLiteral("decks-v");
// `git describe --always` without a tag: the abbreviated commit hash.
const QRegularExpression kCommitOnly(QStringLiteral("^([0-9a-f]+)(-modified)?$"));

} // namespace

void applyApplicationIdentity() {
    // These need to be set early on (not sure how early) in order to trigger
    // logic in the OS X appstore support patch from QTBUG-16549.
    QCoreApplication::setOrganizationDomain(QString::fromUtf8(kOrganizationDomain));

    // With an organization name, QStandardPaths::AppLocalDataLocation (the
    // settings directory on Windows) becomes "<data>/Crate/Crate Decks".
    QCoreApplication::setOrganizationName(QString::fromUtf8(kOrganizationName));

#ifdef __LINUX__
    // Needed by Wayland compositors to set proper app_id and window icon
    QGuiApplication::setDesktopFileName(QString::fromUtf8(kDesktopFileName));
#endif

    // Printed by --version.
    QCoreApplication::setApplicationVersion(buildVersion(VersionStore::gitDescribe()));
}

QUrl sourceCodeUrl(const QString& gitDescribe) {
    const QString repository = QString::fromUtf8(kSourceRepositoryUrl);
    QString ref = gitDescribe.trimmed();
    if (ref.endsWith(kModifiedSuffix)) {
        // The closest published source is the commit the build started from.
        ref.chop(kModifiedSuffix.size());
    }
    if (ref.isEmpty()) {
        return QUrl(repository);
    }
    const QRegularExpressionMatch commitAfterTag = kCommitAfterTag.match(ref);
    if (commitAfterTag.hasMatch()) {
        ref = commitAfterTag.captured(1);
    }
    // Otherwise `ref` is a tag or, without tags, a commit hash.
    return QUrl(repository + QStringLiteral("/tree/") +
            QString::fromUtf8(QUrl::toPercentEncoding(ref, QByteArrayLiteral("/"))));
}

QString buildVersion(const QString& gitDescribe) {
    const QString productVersion = QString::fromUtf8(kApplicationVersion);
    const QString describe = gitDescribe.trimmed();
    if (describe.startsWith(kTagPrefix) && describe.size() > kTagPrefix.size()) {
        return describe.mid(kTagPrefix.size());
    }
    const QRegularExpressionMatch commitOnly = kCommitOnly.match(describe);
    if (commitOnly.hasMatch()) {
        // A development build towards the product version.
        return productVersion + QStringLiteral("-dev+g") + commitOnly.captured(1) +
                commitOnly.captured(2);
    }
    return productVersion + QStringLiteral("-unknown");
}

} // namespace crate
