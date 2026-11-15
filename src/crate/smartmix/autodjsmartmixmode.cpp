#include "crate/smartmix/autodjsmartmixmode.h"

#include <QComboBox>
#include <QCoreApplication>

#include "crate/crateservers.h"
#include "crate/smartmix/autodjsmartmixadapter.h"
#include "moc_autodjsmartmixmode.cpp"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::AutoDJSmartMixMode");

using TransitionMode = AutoDJProcessor::TransitionMode;

constexpr int kSmartMix = static_cast<int>(TransitionMode::CrateSmartMix);

} // namespace

const ConfigKey& AutoDJSmartMixMode::pendingKey() {
    static const ConfigKey kKey(QStringLiteral("[Crate]"), QStringLiteral("AutoDjSmartMixPending"));
    return kKey;
}

AutoDJSmartMixMode::AutoDJSmartMixMode(Options options)
        : QObject(options.pCombo),
          m_options([&options] {
              if (!options.offered) {
                  options.offered = autoDjSmartMixOffered;
              }
              return options;
          }()) {
    // Queued: DlgAutoDJ's own slot sets the mode first
    connect(m_options.pCombo,
            QOverload<int>::of(&QComboBox::activated),
            this,
            &AutoDJSmartMixMode::onActivated,
            Qt::QueuedConnection);
    if (CrateServers* pServers = m_options.pServers) {
        connect(pServers, &CrateServers::serversChanged, this, &AutoDJSmartMixMode::update);
        connect(pServers, &CrateServers::capabilitiesChanged, this, &AutoDJSmartMixMode::update);
    }
    update();
}

void AutoDJSmartMixMode::update() {
    QComboBox* const pCombo = m_options.pCombo;
    AutoDJProcessor* const pProcessor = m_options.pProcessor;
    if (!pCombo || !pProcessor || !m_options.pConfig) {
        return;
    }
    const bool offered = m_options.offered();
    const int entry = pCombo->findData(kSmartMix);
    if (offered && entry < 0) {
        pCombo->addItem(QCoreApplication::translate("DlgAutoDJ", "Crate Smart Mix"), kSmartMix);
    } else if (!offered && entry >= 0) {
        pCombo->removeItem(entry);
    }
    const TransitionMode mode = pProcessor->getTransitionMode();
    if (!offered && mode == TransitionMode::CrateSmartMix) {
        kLogger.info() << "No server offers Crate Smart Mix: Auto DJ uses Full Intro + "
                          "Outro until one does";
        pProcessor->setTransitionMode(TransitionMode::FullIntroOutro);
        m_options.pConfig->setValue(pendingKey(), true);
    } else if (offered && m_options.pConfig->getValue<bool>(pendingKey())) {
        kLogger.info() << "A server offers Crate Smart Mix again: back to it";
        m_options.pConfig->remove(pendingKey());
        pProcessor->setTransitionMode(TransitionMode::CrateSmartMix);
    }
    pCombo->setCurrentIndex(pCombo->findData(static_cast<int>(pProcessor->getTransitionMode())));
}

void AutoDJSmartMixMode::onActivated() {
    if (!m_options.pConfig) {
        return;
    }
    // The DJ's choice replaces the one that waited
    m_options.pConfig->remove(pendingKey());
    if (!m_options.pConfig->save()) {
        kLogger.warning() << "Could not save Auto DJ's transition mode";
    }
}

void addAutoDjSmartMixMode(
        QComboBox* pCombo, AutoDJProcessor* pProcessor, UserSettingsPointer pConfig) {
    AutoDJSmartMixMode::Options options;
    options.pCombo = pCombo;
    options.pProcessor = pProcessor;
    options.pConfig = std::move(pConfig);
    options.pServers = CrateServers::instance();
    new AutoDJSmartMixMode(std::move(options));
}

} // namespace crate
