#include "crate/system/systemprobes.h"

#include <QApplication>
#include <QPointer>
#include <QScreen>
#include <QWidget>

#include "control/controlobject.h"
#include "crate/cratepreferences.h"
#include "crate/system/powerhelper.h"
#include "waveform/waveformwidgetfactory.h"

namespace crate::system {

namespace {

const ConfigKey kAudioUsageKey(QStringLiteral("[App]"), QStringLiteral("audio_latency_usage"));
const ConfigKey kAudioOverloadsKey(
        QStringLiteral("[App]"), QStringLiteral("audio_latency_overload_count"));

/// The main window, or the active one.
QWidget* mainWindow() {
    const QWidgetList windows = QApplication::topLevelWidgets();
    for (QWidget* pWindow : windows) {
        if (pWindow->inherits("MixxxMainWindow")) {
            return pWindow;
        }
    }
    return QApplication::activeWindow();
}

} // namespace

QString rendererOf(const QString& openGlVersion) {
    // The last parenthesis, which may hold parentheses of its own
    if (!openGlVersion.endsWith(QLatin1Char(')'))) {
        return QString();
    }
    int depth = 0;
    for (qsizetype i = openGlVersion.size() - 1; i >= 0; --i) {
        if (openGlVersion[i] == QLatin1Char(')')) {
            ++depth;
        } else if (openGlVersion[i] == QLatin1Char('(') && --depth == 0) {
            return openGlVersion.mid(i + 1, openGlVersion.size() - i - 2);
        }
    }
    return QString();
}

Probes applicationProbes(const CratePreferences* pPreferences, const PowerHelper* pPowerHelper) {
    Probes probes;
    probes.render = [] {
        RenderInfo info;
        if (!WaveformWidgetFactory::isCreated()) {
            return info;
        }
        const WaveformWidgetFactory* pFactory = WaveformWidgetFactory::instance();
        const QString version = pFactory->getOpenGLVersion();
        if (version.isEmpty()) {
            return info;
        }
        info.known = true;
        info.renderer = rendererOf(version);
        info.usable = pFactory->isOpenGlAvailable() || pFactory->isOpenGlesAvailable();
        return info;
    };
    probes.display = [] {
        DisplayInfo info;
        QWidget* pWindow = mainWindow();
        const QScreen* pScreen = pWindow ? pWindow->screen() : QGuiApplication::primaryScreen();
        if (!pScreen) {
            return info;
        }
        const QSize size = pScreen->size() * pScreen->devicePixelRatio();
        info.known = true;
        info.width = size.width();
        info.height = size.height();
        info.refreshHz = pScreen->refreshRate();
        return info;
    };
    probes.audio = [] {
        AudioInfo info;
        if (!ControlObject::exists(kAudioUsageKey) || !ControlObject::exists(kAudioOverloadsKey)) {
            return info;
        }
        info.known = true;
        // A fraction of the buffer's time spent in the audio callback
        info.usagePercent = 100 * ControlObject::get(kAudioUsageKey);
        info.overloadCount = static_cast<int>(ControlObject::get(kAudioOverloadsKey));
        return info;
    };
    const QPointer<const CratePreferences> preferences(pPreferences);
    probes.discreteGpuPreferred = [preferences] {
        return preferences && preferences->discreteGpu();
    };
    const QPointer<const PowerHelper> helper(pPowerHelper);
    probes.powerHelperAvailable = [helper] {
        return helper && helper->isAvailable();
    };
    return probes;
}

} // namespace crate::system
