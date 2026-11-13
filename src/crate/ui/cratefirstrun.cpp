#include "crate/ui/cratefirstrun.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

#include "coreservices.h"
#include "crate/crateservers.h"
#include "crate/ui/crateaddserverwidget.h"
#include "crate/ui/dialogs/dialogchrome.h"
#include "crate/ui/dialogs/dialogstyle.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "soundio/sounddevice.h"
#include "soundio/sounddevicestatus.h"
#include "soundio/soundmanager.h"
#include "soundio/soundmanagerconfig.h"
#include "soundio/soundmanagerutil.h"

namespace crate {

namespace {

const ConfigKey kFirstRunOfferedKey(
        QStringLiteral("[CrateDecks]"), QStringLiteral("FirstRunOffered"));
// Upstream's menu bar settings (mixxxmainwindow.cpp)
const ConfigKey kHideMenuBarKey(QStringLiteral("[Config]"), QStringLiteral("hide_menubar"));
const ConfigKey kMenuBarHintKey(QStringLiteral("[Config]"), QStringLiteral("show_menubar_hint"));

/// How often the first start checks whether it can show (firstRunCanShow())
constexpr int kFirstRunWaitMs = 100;

QString translate(const char* text) {
    return QCoreApplication::translate("crate::FirstRun", text);
}

/// A page of the stack of the first start. QStackedLayout asks every page
/// for its height for a width, whatever its size policy, so a page that is
/// not shown (Ignored, fitPages()) answers 0: the dialog fits the page it
/// shows.
class FirstRunPage : public QWidget {
  public:
    using QWidget::QWidget;

    int heightForWidth(int width) const override {
        return sizePolicy().verticalPolicy() == QSizePolicy::Ignored
                ? 0
                : QWidget::heightForWidth(width);
    }
};

/// A page of the first start: its own layout, without margins, so that the
/// dialog's padding is the mockup's, at the mockup's width.
QWidget* newPage(QWidget* pParent, QVBoxLayout** ppLayout) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pPage = new FirstRunPage(pParent);
    // The mockup's content width (first-run: 560 px less its padding); the
    // pages never widen the dialog
    pPage->setFixedWidth(metrics.px("firstRunWidth") - 2 * metrics.px("firstRunPaddingX"));
    auto* pLayout = new QVBoxLayout(pPage);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(metrics.px("firstRunGap"));
    *ppLayout = pLayout;
    return pPage;
}

/// The head of a page: the logo and "FIRST START" with its meta (the step,
/// "Sound"...), which it returns.
QLabel* addHead(QVBoxLayout* pLayout, QWidget* pPage) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pHead = new QHBoxLayout();
    pHead->setSpacing(metrics.px("firstRunHeadGap"));
    auto* pLogo = new QLabel(pPage);
    pLogo->setObjectName(QStringLiteral("CrateFirstRunLogo"));
    pLogo->setPixmap(logoPixmap(metrics.px("firstRunLogoWidth"),
            metrics.px("firstRunLogoHeight"),
            pPage->devicePixelRatioF()));
    pHead->addWidget(pLogo);
    auto* pKicker = new QHBoxLayout();
    pKicker->setSpacing(metrics.px("helperGap"));
    pKicker->addWidget(monoLabel(translate("First start"),
            QStringLiteral("MonoFirstRunKicker"),
            pPage,
            true));
    QLabel* pMeta = monoLabel(QString(), QStringLiteral("MonoFirstRunStep"), pPage);
    pKicker->addWidget(pMeta);
    pHead->addLayout(pKicker);
    pHead->addStretch(1);
    pLayout->addLayout(pHead);
    return pMeta;
}

/// The title and the paragraph under it.
void addTitle(QVBoxLayout* pLayout, QWidget* pPage, const QString& title, const QString& intro) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pTitle = new QLabel(title, pPage);
    pTitle->setObjectName(QStringLiteral("CrateFirstRunTitle"));
    pLayout->addWidget(pTitle);
    setDisplayAxes(pTitle);
    auto* pIntro = new QLabel(pPage);
    pIntro->setObjectName(QStringLiteral("CrateFirstRunIntro"));
    setParagraph(pIntro, intro, "fontSizeBody");
    pIntro->setFixedWidth(metrics.px("firstRunWidth") - 2 * metrics.px("firstRunPaddingX"));
    pLayout->addWidget(pIntro);
}

QLabel* plainLabel(const QString& objectName, QWidget* pParent) {
    auto* pLabel = new QLabel(pParent);
    pLabel->setObjectName(objectName);
    pLabel->setTextFormat(Qt::PlainText);
    pLabel->setWordWrap(true);
    return pLabel;
}

QPushButton* pageButton(
        const QString& text, const QString& objectName, const char* variant, QWidget* pParent) {
    auto* pButton = new QPushButton(text, pParent);
    pButton->setObjectName(objectName);
    setButtonVariant(pButton, variant);
    return pButton;
}

/// The buttons of a page and, at the right, where to find it later
/// (`ppLater`, when wanted).
QHBoxLayout* addFoot(
        QVBoxLayout* pLayout, QWidget* pPage, const QString& later, QLabel** ppLater = nullptr) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pFoot = new QHBoxLayout();
    pFoot->setSpacing(metrics.px("helperGap"));
    pLayout->addLayout(pFoot);
    pFoot->addStretch(1);
    QLabel* pLater = monoLabel(later, QStringLiteral("MonoFirstRunLater"), pPage);
    pFoot->addWidget(pLater);
    if (ppLater) {
        *ppLater = pLater;
    }
    return pFoot;
}

/// "Connect your Crate": the page approved in GU1.
QWidget* serverPage(CrateServers* pServers, QWidget* pParent, AddServerWidget** ppAddServer) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    QVBoxLayout* pLayout = nullptr;
    QWidget* pPage = newPage(pParent, &pLayout);
    pPage->setObjectName(QStringLiteral("CrateFirstRunServer"));
    QLabel* pStep = addHead(pLayout, pPage);
    addTitle(pLayout,
            pPage,
            translate("Connect your Crate"),
            translate("Crate Decks uses your Crate server as its library: your tracks, "
                      "Crate's analysis, recommendations and Smart Mix, right in the decks."));

    // The three steps as lanes, the current and the done ones in accentHi
    auto* pSteps = new QHBoxLayout();
    pSteps->setSpacing(metrics.px("stepsGap"));
    const char* names[AddServerWidget::kStepCount] = {
            QT_TRANSLATE_NOOP("crate::FirstRun", "Address"),
            QT_TRANSLATE_NOOP("crate::FirstRun", "Check"),
            QT_TRANSLATE_NOOP("crate::FirstRun", "Sign in"),
    };
    QList<QLabel*> stepLabels;
    QList<QFrame*> stepLanes;
    for (int i = 0; i < AddServerWidget::kStepCount; ++i) {
        auto* pColumn = new QVBoxLayout();
        pColumn->setSpacing(metrics.px("stepGap"));
        QLabel* pLabel = monoLabel(QStringLiteral("%1 · %2").arg(i + 1).arg(translate(names[i])),
                QStringLiteral("MonoStep"),
                pPage);
        auto* pLane = new QFrame(pPage);
        pLane->setObjectName(QStringLiteral("CrateStepLane"));
        pColumn->addWidget(pLabel);
        pColumn->addWidget(pLane);
        pSteps->addLayout(pColumn, 1);
        stepLabels.append(pLabel);
        stepLanes.append(pLane);
    }
    pLayout->addLayout(pSteps);

    auto* pAddServer = new AddServerWidget(pServers, pPage);
    pAddServer->setCancelText(translate("Not now"));
    pAddServer->addTrailing(monoLabel(translate("Later: Preferences · Crate"),
            QStringLiteral("MonoFirstRunLater"),
            pPage));
    pLayout->addWidget(pAddServer);

    const auto showStep = [pStep, stepLabels, stepLanes](int number) {
        pStep->setText(translate("Step %1 of %2")
                        .arg(QString::number(number),
                                QString::number(AddServerWidget::kStepCount)));
        for (int i = 0; i < stepLabels.size(); ++i) {
            const bool on = i < number;
            stepLabels[i]->setProperty("crateTone", on ? QStringLiteral("accent") : QString());
            stepLanes[i]->setProperty("current", on);
            for (QWidget* pWidget : {static_cast<QWidget*>(stepLabels[i]),
                         static_cast<QWidget*>(stepLanes[i])}) {
                pWidget->style()->unpolish(pWidget);
                pWidget->style()->polish(pWidget);
            }
        }
    };
    QObject::connect(pAddServer, &AddServerWidget::stepNumberChanged, pPage, showStep);
    showStep(1);
    *ppAddServer = pAddServer;
    return pPage;
}

/// The sound page: the main output, or a warning without devices. It replaces
/// Upstream's "No Output Devices" on the first start. `done` moves on.
QWidget* soundPage(const FirstRunServices& services,
        QWidget* pParent,
        const std::function<void()>& done) {
    QVBoxLayout* pLayout = nullptr;
    QWidget* pPage = newPage(pParent, &pLayout);
    pPage->setObjectName(QStringLiteral("CrateFirstRunSound"));
    addHead(pLayout, pPage)->setText(translate("Sound"));
    addTitle(pLayout,
            pPage,
            translate("Where the decks play"),
            translate("Choose the output for the main mix. Headphones, booth and inputs "
                      "are in Preferences · Sound hardware."));

    auto* pField = new QVBoxLayout();
    pField->setSpacing(DialogMetrics::installed().px("stepGap"));
    pField->addWidget(monoLabel(translate("Main output"), QStringLiteral("MonoFieldLabel"), pPage));
    auto* pOutputs = new QComboBox(pPage);
    pOutputs->setObjectName(QStringLiteral("crateFirstRunOutput"));
    pField->addWidget(pOutputs);
    QLabel* pNoOutput = plainLabel(QStringLiteral("crateError"), pPage);
    pNoOutput->setText(translate("No output device found."));
    pField->addWidget(pNoOutput);
    QLabel* pHelper = plainLabel(QStringLiteral("CrateFormHelper"), pPage);
    pField->addWidget(pHelper);
    pLayout->addLayout(pField);

    QLabel* pLater = nullptr;
    QHBoxLayout* pFoot =
            addFoot(pLayout, pPage, translate("Later: Preferences · Sound"), &pLater);
    QPushButton* pContinue = pageButton(translate("Continue"),
            QStringLiteral("crateFirstRunContinue"),
            "primary",
            pPage);
    QPushButton* pSearch = pageButton(translate("Search again"),
            QStringLiteral("crateFirstRunSearch"),
            "ghost",
            pPage);
    QPushButton* pMore = pageButton(translate("More options"),
            QStringLiteral("crateFirstRunMore"),
            "ghost",
            pPage);
    pFoot->insertWidget(0, pContinue);
    pFoot->insertWidget(1, pSearch);
    pFoot->insertWidget(2, pMore);

    const auto refresh = [services, pOutputs, pNoOutput, pHelper, pContinue, pSearch, pLater] {
        const QList<FirstRunOutput> outputs = services.outputs();
        const QString current = services.mainOutput ? services.mainOutput() : QString();
        pOutputs->clear();
        for (const FirstRunOutput& output : outputs) {
            pOutputs->addItem(output.name, output.id);
            if (output.id == current) {
                pOutputs->setCurrentIndex(pOutputs->count() - 1);
            }
        }
        const bool none = outputs.isEmpty();
        pOutputs->setVisible(!none);
        pNoOutput->setVisible(none);
        pSearch->setVisible(none);
        // Without a device the page has three buttons: no room for the hint
        pLater->setVisible(!none);
        pHelper->setText(none
                        ? translate("The decks play nothing until there is one. Connect a "
                                    "sound card and search again, or continue without sound.")
                        : translate("Crate Decks opens it now. You can change it at any time."));
        pContinue->setText(none ? translate("Continue without sound") : translate("Continue"));
    };
    refresh();

    QObject::connect(pSearch, &QPushButton::clicked, pPage, [services, refresh] {
        if (services.searchOutputs) {
            services.searchOutputs();
        }
        refresh();
    });
    QObject::connect(pMore, &QPushButton::clicked, pPage, [services, refresh] {
        if (services.showSoundPreferences) {
            services.showSoundPreferences();
        }
        refresh();
    });
    QObject::connect(pContinue,
            &QPushButton::clicked,
            pPage,
            [services, pOutputs, pNoOutput, done] {
                const QString chosen = pOutputs->currentData().toString();
                const QString current =
                        services.mainOutput ? services.mainOutput() : QString();
                if (!chosen.isEmpty() && chosen != current &&
                        services.setMainOutput &&
                        !services.setMainOutput(chosen)) {
                    pNoOutput->setText(
                            translate("Crate Decks cannot open this device. "
                                      "Choose another one."));
                    pNoOutput->setVisible(true);
                    return;
                }
                done();
            });
    return pPage;
}

/// The optional local music page: folders of this computer next to the
/// Crate library. It replaces upstream's music folder prompt. `done` finishes.
QWidget* musicPage(const FirstRunServices& services,
        QWidget* pParent,
        const std::function<void()>& done) {
    QVBoxLayout* pLayout = nullptr;
    QWidget* pPage = newPage(pParent, &pLayout);
    pPage->setObjectName(QStringLiteral("CrateFirstRunMusic"));
    addHead(pLayout, pPage)->setText(translate("Optional"));
    addTitle(pLayout,
            pPage,
            translate("Music on this computer"),
            translate("Your Crate is the library. If you also play files from this "
                      "computer, add their folders and Crate Decks scans them."));

    auto* pField = new QVBoxLayout();
    pField->setSpacing(DialogMetrics::installed().px("stepGap"));
    pField->addWidget(monoLabel(translate("Folders"), QStringLiteral("MonoFieldLabel"), pPage));
    QLabel* pFolders = plainLabel(QStringLiteral("crateFirstRunFolders"), pPage);
    pFolders->setVisible(false);
    pField->addWidget(pFolders);
    QLabel* pHelper = plainLabel(QStringLiteral("CrateFormHelper"), pPage);
    pHelper->setText(translate("Nothing is added until you choose a folder."));
    pField->addWidget(pHelper);
    pLayout->addLayout(pField);

    QHBoxLayout* pFoot = addFoot(pLayout, pPage, translate("Later: Preferences · Library"));
    QPushButton* pFinish = pageButton(translate("Finish"),
            QStringLiteral("crateFirstRunFinish"),
            "primary",
            pPage);
    QPushButton* pAdd = pageButton(translate("Add folder"),
            QStringLiteral("crateFirstRunAddFolder"),
            "ghost",
            pPage);
    pFoot->insertWidget(0, pFinish);
    pFoot->insertWidget(1, pAdd);

    QObject::connect(pAdd, &QPushButton::clicked, pPage, [services, pPage, pFolders, pHelper] {
        if (!services.chooseFolder) {
            return;
        }
        const QString folder = services.chooseFolder(pPage);
        if (folder.isEmpty()) {
            return;
        }
        if (services.addMusicFolder && services.addMusicFolder(folder)) {
            QStringList shown = pFolders->text().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            shown.append(QDir::toNativeSeparators(folder));
            pFolders->setText(shown.join(QLatin1Char('\n')));
            pFolders->setVisible(true);
            pHelper->setText(translate("Crate Decks scans them in the background."));
        } else {
            pHelper->setText(translate("Crate Decks cannot add this folder."));
        }
    });
    QObject::connect(pFinish, &QPushButton::clicked, pPage, done);
    return pPage;
}

/// Sizes the stack of pages to `pPage` alone, not to the tallest page.
void fitPages(QStackedWidget* pPages, QWidget* pPage) {
    for (int i = 0; i < pPages->count(); ++i) {
        QWidget* pOther = pPages->widget(i);
        const auto policy = pOther == pPage ? QSizePolicy::Preferred : QSizePolicy::Ignored;
        pOther->setSizePolicy(policy, policy);
    }
    pPages->setCurrentWidget(pPage);
    // Now, not when the layout requests arrive: the dialog fits at once
    pPages->layout()->invalidate();
    pPages->updateGeometry();
    if (QLayout* pDialogLayout = pPages->window()->layout()) {
        pDialogLayout->invalidate();
        pDialogLayout->activate();
    }
}

/// Shows `pPage` with its primary button `objectName` focused and default,
/// so that Enter moves on. The dialog fits the page and stays centred over
/// its parent.
void showPage(QStackedWidget* pPages, QWidget* pPage, const char* objectName) {
    QWidget* pDialog = pPages->window();
    const QPoint centre = pDialog->geometry().center();
    fitPages(pPages, pPage);
    if (auto* pButton = pPage->findChild<QPushButton*>(QLatin1String(objectName))) {
        pButton->setDefault(true);
        pButton->setFocus();
    }
    if (pDialog->isVisible()) {
        QRect geometry = pDialog->geometry();
        geometry.moveCenter(centre);
        pDialog->move(geometry.topLeft());
    }
}

/// Index of the main output's device in `outputs`, or -1.
int mainOutputIndex(const SoundManagerConfig& config, const QList<SoundDevicePointer>& devices) {
    const QMultiHash<SoundDeviceId, AudioOutput> outputs = config.getOutputs();
    for (auto it = outputs.constBegin(); it != outputs.constEnd(); ++it) {
        if (it.value().getType() != AudioPath::AudioPathType::Main) {
            continue;
        }
        for (int i = 0; i < devices.size(); ++i) {
            if (devices[i]->getDeviceId() == it.key()) {
                return i;
            }
        }
    }
    return -1;
}

} // namespace

bool shouldOfferFirstRun(const UserSettingsPointer& pConfig, const CrateServers& servers) {
    if (!pConfig || !servers.sessions().isEmpty()) {
        return false;
    }
    return firstRunPending(pConfig);
}

void markFirstRunOffered(const UserSettingsPointer& pConfig) {
    if (pConfig) {
        pConfig->set(kFirstRunOfferedKey, ConfigValue(QStringLiteral("1")));
    }
}

bool firstRunPending(const UserSettingsPointer& pConfig) {
    return pConfig && pConfig->getValueString(kFirstRunOfferedKey) != QLatin1String("1");
}

void decideMenuBar(const UserSettingsPointer& pConfig) {
    if (!pConfig || !pConfig->getValue<bool>(kMenuBarHintKey, true)) {
        return;
    }
    pConfig->setValue(kHideMenuBarKey, 1);
    pConfig->setValue(kMenuBarHintKey, 0);
}

FirstRunServices firstRunServices(mixxx::CoreServices* pCoreServices) {
    FirstRunServices services;
    if (!pCoreServices) {
        return services;
    }
    std::shared_ptr<SoundManager> pSound = pCoreServices->getSoundManager();
    // The devices of the last listing: the page's IDs are their indexes
    auto pDevices = std::make_shared<QList<SoundDevicePointer>>();
    services.outputs = [pSound, pDevices] {
        *pDevices = pSound->getDeviceList(pSound->getConfig().getAPI(), true, false);
        QList<FirstRunOutput> outputs;
        for (int i = 0; i < pDevices->size(); ++i) {
            outputs.append({QString::number(i), pDevices->at(i)->getDisplayName()});
        }
        return outputs;
    };
    services.mainOutput = [pSound, pDevices] {
        const int index = mainOutputIndex(pSound->getConfig(), *pDevices);
        return index < 0 ? QString() : QString::number(index);
    };
    services.setMainOutput = [pSound, pDevices](const QString& id) {
        bool ok = false;
        const int index = id.toInt(&ok);
        if (!ok || index < 0 || index >= pDevices->size()) {
            return false;
        }
        SoundManagerConfig config = pSound->getConfig();
        QMultiHash<SoundDeviceId, AudioOutput>& outputs = config.getOutputsRef();
        for (auto it = outputs.begin(); it != outputs.end();) {
            it = it.value().getType() == AudioPath::AudioPathType::Main ? outputs.erase(it)
                                                                        : std::next(it);
        }
        config.addOutput(pDevices->at(index)->getDeviceId(),
                AudioOutput(AudioPath::AudioPathType::Main,
                        0,
                        mixxx::audio::ChannelCount::stereo()));
        return pSound->setConfig(config) == SoundDeviceStatus::Ok;
    };
    services.searchOutputs = [pSound] {
        pSound->clearAndQueryDevices();
    };
    services.showSoundPreferences = [] {
        // Over the first start, as upstream opens it from "No Output Devices"
        const QWidgetList windows = QApplication::topLevelWidgets();
        for (QWidget* pWindow : windows) {
            if (auto* pPreferences = qobject_cast<QDialog*>(pWindow);
                    pPreferences && pWindow->inherits("DlgPreferences")) {
                QMetaObject::invokeMethod(pPreferences, "showSoundHardwarePage");
                pPreferences->setWindowModality(Qt::ApplicationModal);
                pPreferences->exec();
                return;
            }
        }
    };
    services.chooseFolder = [](QWidget* pParent) {
        return QFileDialog::getExistingDirectory(pParent,
                QCoreApplication::translate("crate::FirstRun", "Choose a music folder"),
                QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
                QFileDialog::ShowDirsOnly);
    };
    std::shared_ptr<Library> pLibrary = pCoreServices->getLibrary();
    std::shared_ptr<TrackCollectionManager> pCollections =
            pCoreServices->getTrackCollectionManager();
    services.addMusicFolder = [pLibrary, pCollections](const QString& folder) {
        if (!pLibrary || !pLibrary->requestAddDir(folder)) {
            return false;
        }
        // The scan upstream starts when its own prompt adds a folder
        if (pCollections) {
            pCollections->startLibraryScan();
        }
        return true;
    };
    return services;
}

QDialog* createFirstRunDialog(CrateServers* pServers, QWidget* pParent) {
    return createFirstRunDialog(pServers, FirstRunServices(), pParent);
}

QDialog* createFirstRunDialog(
        CrateServers* pServers, const FirstRunServices& services, QWidget* pParent) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pDialog = new QDialog(pParent);
    pDialog->setObjectName(QStringLiteral("CrateFirstRun"));
    pDialog->setWindowTitle(translate("Connect to Crate"));
    auto* pLayout = new QVBoxLayout(pDialog);
    pLayout->setContentsMargins(metrics.px("firstRunPaddingX"),
            metrics.px("firstRunPaddingTop"),
            metrics.px("firstRunPaddingX"),
            metrics.px("firstRunPaddingBottom"));
    pLayout->setSpacing(0);
    pLayout->setSizeConstraint(QLayout::SetFixedSize);
    new DialogChrome(pDialog);

    auto* pPages = new QStackedWidget(pDialog);
    pPages->setObjectName(QStringLiteral("CrateFirstRunPages"));
    pLayout->addWidget(pPages);
    AddServerWidget* pAddServer = nullptr;
    pPages->addWidget(serverPage(pServers, pPages, &pAddServer));

    if (!services.hasPages()) {
        QObject::connect(pAddServer, &AddServerWidget::serverAdded, pDialog, &QDialog::accept);
        QObject::connect(pAddServer, &AddServerWidget::cancelled, pDialog, &QDialog::reject);
        return pDialog;
    }

    QWidget* pServer = pPages->currentWidget();
    QWidget* pMusic = musicPage(services, pPages, [pDialog] { pDialog->accept(); });
    QWidget* pSound = soundPage(services, pPages, [pPages, pMusic, pDialog] {
        showPage(pPages, pMusic, "crateFirstRunFinish");
        pDialog->setWindowTitle(translate("Music on this computer"));
    });
    pPages->addWidget(pSound);
    pPages->addWidget(pMusic);
    const auto toSound = [pPages, pSound, pDialog] {
        showPage(pPages, pSound, "crateFirstRunContinue");
        pDialog->setWindowTitle(translate("Sound"));
    };
    QObject::connect(pAddServer, &AddServerWidget::serverAdded, pDialog, toSound);
    QObject::connect(pAddServer, &AddServerWidget::cancelled, pDialog, toSound);
    fitPages(pPages, pServer);
    return pDialog;
}

bool firstRunCanShow(const QWidget* pMainWindow) {
    if (QApplication::activeModalWidget()) {
        return false;
    }
    const auto* pWindow = qobject_cast<const QMainWindow*>(pMainWindow);
    if (!pWindow) {
        return true;
    }
    const QWidget* pCentral = pWindow->centralWidget();
    return pCentral && !pCentral->inherits("LaunchImage");
}

void offerFirstRun(const UserSettingsPointer& pConfig, CrateServers* pServers, QWidget* pParent) {
    offerFirstRun(pConfig, pServers, FirstRunServices(), pParent);
}

void offerFirstRun(const UserSettingsPointer& pConfig,
        CrateServers* pServers,
        const FirstRunServices& services,
        QWidget* pParent) {
    if (!pServers || QGuiApplication::platformName() == QLatin1String("offscreen")) {
        return;
    }
    // With the sound and music pages the first start is due once, even if a
    // server was added before it; without them, only while there is none
    const auto due = [pConfig, services](const CrateServers& servers) {
        return services.hasPages() ? firstRunPending(pConfig)
                                   : shouldOfferFirstRun(pConfig, servers);
    };
    if (!due(*pServers)) {
        return;
    }
    // Waits on a timer: upstream has no signal for the end of its start, and
    // its startup dialogs run their own event loops
    auto* pWait = new QTimer(pParent);
    pWait->setInterval(kFirstRunWaitMs);
    QObject::connect(pWait,
            &QTimer::timeout,
            pWait,
            [pWait, pConfig, pServers = QPointer<CrateServers>(pServers), services, due, pParent] {
                if (!firstRunCanShow(pParent)) {
                    return;
                }
                pWait->stop();
                pWait->deleteLater();
                if (!pServers || !due(*pServers)) {
                    return;
                }
                markFirstRunOffered(pConfig);
                QDialog* pDialog = createFirstRunDialog(pServers, services, pParent);
                if (services.hasPages() && !pServers->sessions().isEmpty()) {
                    // A server is there already: straight to the sound page
                    if (auto* pPages = pDialog->findChild<QStackedWidget*>(
                                QStringLiteral("CrateFirstRunPages"))) {
                        showPage(pPages, pPages->widget(1), "crateFirstRunContinue");
                    }
                }
                pDialog->exec();
                delete pDialog;
            });
    pWait->start();
}

} // namespace crate
