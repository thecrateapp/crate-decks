#pragma once

// Crate Decks started offscreen for the tests that need the real application:
// the core services, the main window and the skin, as main.cpp builds them.
// Shared by the skin tests and the test of what the interface shows.

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QList>
#include <QMessageBox>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <memory>
#include <utility>

#include "config.h"
#include "coreservices.h"
#include "mixxxmainwindow.h"
#include "preferences/usersettings.h"
#include "skin/legacy/launchimage.h"
#include "util/cmdlineargs.h"
#include "util/versionstore.h"

namespace crate::test {

/// Answers every modal dialog as it shows, so that nothing waits offscreen:
/// "Continue" where there is one (no output devices, which has no sound
/// card on purpose), otherwise reject.
class DialogRejecter : public QObject {
  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() == QEvent::Show) {
            if (auto* pDialog = qobject_cast<QDialog*>(pWatched); pDialog && pDialog->isModal()) {
                if (auto* pBox = qobject_cast<QMessageBox*>(pDialog)) {
                    const QList<QAbstractButton*> buttons = pBox->buttons();
                    for (QAbstractButton* pButton : buttons) {
                        if (pButton->text() == QLatin1String("Continue")) {
                            QTimer::singleShot(0, pButton, &QAbstractButton::click);
                            return QObject::eventFilter(pWatched, pEvent);
                        }
                    }
                }
                QTimer::singleShot(0, pDialog, &QDialog::reject);
            }
        }
        return QObject::eventFilter(pWatched, pEvent);
    }
};

/// Processes events until `done` or `timeoutMs`.
inline bool waitFor(const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

/// Crate Decks started offscreen on a disposable profile, with no sound
/// devices: the real main window and skin, as main.cpp builds them.
class OffscreenDecks {
  public:
    using Settings = QList<std::pair<ConfigKey, QString>>;

    explicit OffscreenDecks(const QString& settingsPath)
            : m_settingsPath(settingsPath),
              m_previousArgs(CmdlineArgs::Instance()) {
    }

    ~OffscreenDecks() {
        stop();
        QCoreApplication::instance()->removeEventFilter(&m_dialogRejecter);
        CmdlineArgs::Instance() = m_previousArgs;
    }

    /// Writes the profile: the skin, no sound API and `settings`.
    void writeProfile(const QString& skin, const Settings& settings = {}) {
        auto config = UserSettingsPointer(new UserSettings(m_settingsPath + MIXXX_SETTINGS_FILE));
        config->setValue(ConfigKey("[Config]", "Version"), VersionStore::version());
        config->setValue(ConfigKey("[Config]", "ResizableSkin"), skin);
        // Offscreen there is no OpenGL: do not warn about it
        config->setValue(ConfigKey("[Direct Rendering]", "Warned"), QStringLiteral("yes"));
        for (const auto& [key, value] : settings) {
            config->setValue(key, value);
        }
        config->save();
        QFile soundConfig(m_settingsPath + "soundconfig.xml");
        if (soundConfig.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            soundConfig.write("<SoundManagerConfig api=\"None\"/>\n");
        }
    }

    /// Starts the core services and the main window. Returns how long the
    /// main window took to initialize, skin included, in milliseconds.
    /// `beforeSkin` runs once the core services are up, just before the
    /// skin loads: the first CoreServices of a process installs Mixxx's log
    /// handler, so message handlers go in there.
    qint64 start(QApplication* pApp, const std::function<void()>& beforeSkin = {}) {
        pApp->installEventFilter(&m_dialogRejecter);
        CmdlineArgs::Instance().setSettingsPath(m_settingsPath);
        CmdlineArgs args;
        args.setSettingsPath(m_settingsPath);
        m_pCoreServices = std::make_shared<mixxx::CoreServices>(args, pApp);
        m_pMainWindow = std::make_unique<MixxxMainWindow>(m_pCoreServices);
        m_pCoreServices->initialize();
        if (beforeSkin) {
            beforeSkin();
        }
        QElapsedTimer timer;
        timer.start();
#ifdef MIXXX_USE_QOPENGL
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
            // As main.cpp does: the OpenGL probe initializes the window once
            // its context is up
            m_pMainWindow->initializeQOpenGL();
            waitFor(
                    [this] {
                        return !qobject_cast<LaunchImage*>(m_pMainWindow->centralWidget());
                    },
                    30000);
        } else
#endif
        {
            m_pMainWindow->initialize();
        }
        const qint64 elapsed = timer.elapsed();
        QCoreApplication::processEvents();
        return elapsed;
    }

    void stop() {
        m_pMainWindow.reset();
        m_pCoreServices.reset();
    }

    /// Whether modal dialogs are answered as they show (from start()), so
    /// that nothing waits offscreen. The screenshots keep them open.
    void answerModalDialogs(bool answer) {
        if (answer) {
            QCoreApplication::instance()->installEventFilter(&m_dialogRejecter);
        } else {
            QCoreApplication::instance()->removeEventFilter(&m_dialogRejecter);
        }
    }

    MixxxMainWindow* mainWindow() const {
        return m_pMainWindow.get();
    }
    QWidget* skin() const {
        return m_pMainWindow ? m_pMainWindow->centralWidget() : nullptr;
    }
    mixxx::CoreServices* coreServices() const {
        return m_pCoreServices.get();
    }

  private:
    const QString m_settingsPath;
    const CmdlineArgs m_previousArgs;
    DialogRejecter m_dialogRejecter;
    std::shared_ptr<mixxx::CoreServices> m_pCoreServices;
    std::unique_ptr<MixxxMainWindow> m_pMainWindow;
};

} // namespace crate::test
