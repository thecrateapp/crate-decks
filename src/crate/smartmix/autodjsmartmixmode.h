#pragma once

#include <QObject>
#include <QPointer>
#include <functional>

#include "library/autodj/autodjprocessor.h"
#include "preferences/usersettings.h"

class QComboBox;

namespace crate {

class CrateServers;

/// The "Crate Smart Mix" entry of Auto DJ's transition modes (plan F6.C-PR3)
/// and the mode behind it, for DlgAutoDJ's combo box.
///
/// - The entry is there while a saved Crate server announces `beatmatch`
///   (autoDjSmartMixOffered()), and comes and goes with the servers and
///   their capabilities, without a restart.
/// - Auto DJ keeps its mode in `[Auto DJ],TransitionMode`, as upstream does.
///   When Crate Smart Mix is not offered (no server, or none that announces
///   `beatmatch` yet), Auto DJ runs "Full Intro + Outro", whose fade points
///   Crate Smart Mix uses, and the choice waits in pendingKey(): it comes
///   back as soon as a server offers the mode again, unless the DJ picked
///   another mode in between.
/// - A mode the DJ picks reaches the settings file at once. Upstream only
///   writes it when the app quits cleanly, so a session that is killed or
///   crashes lost it (Diego's restart of 2026-11-15).
///
/// It belongs to the combo box and goes with it.
class AutoDJSmartMixMode : public QObject {
    Q_OBJECT
  public:
    struct Options {
        QComboBox* pCombo = nullptr;
        AutoDJProcessor* pProcessor = nullptr;
        UserSettingsPointer pConfig;
        /// Whether Crate Smart Mix is offered; autoDjSmartMixOffered() by
        /// default.
        std::function<bool()> offered;
        /// Its changes ask offered() again; CrateServers::instance() by
        /// default. Optional.
        CrateServers* pServers = nullptr;
    };

    /// `[Crate],AutoDjSmartMixPending`: Crate Smart Mix was the DJ's mode and
    /// waits for a server that offers it.
    static const ConfigKey& pendingKey();

    explicit AutoDJSmartMixMode(Options options);

    /// Adds or removes the entry and sets Auto DJ's mode, as offered() says.
    void update();

  private:
    void onActivated();

    const Options m_options;
};

/// DlgAutoDJ's call: the entry and its mode on `pCombo`, with the app's
/// servers.
void addAutoDjSmartMixMode(
        QComboBox* pCombo, AutoDJProcessor* pProcessor, UserSettingsPointer pConfig);

} // namespace crate
