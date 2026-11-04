#include "crate/ui/cratefirstrun.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QLabel>
#include <QVBoxLayout>

#include "crate/crateservers.h"
#include "crate/ui/crateaddserverwidget.h"

namespace crate {

namespace {

const ConfigKey kFirstRunOfferedKey(
        QStringLiteral("[CrateDecks]"), QStringLiteral("FirstRunOffered"));

} // namespace

bool shouldOfferFirstRun(const UserSettingsPointer& pConfig, const CrateServers& servers) {
    if (!pConfig || !servers.sessions().isEmpty()) {
        return false;
    }
    return pConfig->getValueString(kFirstRunOfferedKey) != QLatin1String("1");
}

void markFirstRunOffered(const UserSettingsPointer& pConfig) {
    if (pConfig) {
        pConfig->set(kFirstRunOfferedKey, ConfigValue(QStringLiteral("1")));
    }
}

void offerFirstRun(const UserSettingsPointer& pConfig, CrateServers* pServers) {
    if (!pServers || QGuiApplication::platformName() == QLatin1String("offscreen") ||
            !shouldOfferFirstRun(pConfig, *pServers)) {
        return;
    }
    markFirstRunOffered(pConfig);

    QDialog dialog;
    dialog.setWindowTitle(QCoreApplication::translate("crate::FirstRun", "Connect to Crate"));
    auto* pIntro = new QLabel(QCoreApplication::translate("crate::FirstRun",
                                      "Crate Decks plays the library of your Crate server. "
                                      "Connect to it now, or later in Preferences > Crate."),
            &dialog);
    pIntro->setWordWrap(true);
    auto* pAddServer = new AddServerWidget(pServers, &dialog);
    auto* pButtons = new QDialogButtonBox(&dialog);
    pButtons->addButton(QCoreApplication::translate("crate::FirstRun", "Not now"),
            QDialogButtonBox::RejectRole);
    auto* pLayout = new QVBoxLayout(&dialog);
    pLayout->addWidget(pIntro);
    pLayout->addWidget(pAddServer);
    pLayout->addWidget(pButtons);

    QObject::connect(pAddServer, &AddServerWidget::serverAdded, &dialog, &QDialog::accept);
    QObject::connect(pAddServer, &AddServerWidget::cancelled, &dialog, &QDialog::reject);
    QObject::connect(pButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

} // namespace crate
