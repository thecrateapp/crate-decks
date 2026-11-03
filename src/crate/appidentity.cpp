#include "crate/appidentity.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QString>

namespace crate {

void applyApplicationIdentity() {
    // These need to be set early on (not sure how early) in order to trigger
    // logic in the OS X appstore support patch from QTBUG-16549.
    QCoreApplication::setOrganizationDomain(QString::fromUtf8(kOrganizationDomain));

    // Setting the organization name results in a QDesktopStorage::DataLocation
    // of "$HOME/Library/Application Support/Mixxx/Mixxx" on OS X. Leave the
    // organization name blank.
    QCoreApplication::setOrganizationName(QString::fromUtf8(kOrganizationName));

#ifdef __LINUX__
    // Needed by Wayland compositors to set proper app_id and window icon
    QGuiApplication::setDesktopFileName(QString::fromUtf8(kDesktopFileName));
#endif
}

} // namespace crate
