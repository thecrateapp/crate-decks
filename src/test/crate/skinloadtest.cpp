#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "coreservices.h"
#include "crate/crateserverrows.h"
#include "crate/crateservers.h"
#include "crate/db/crateserverprofiles.h"
#include "crate/db/crateserverschema.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarystore.h"
#include "crate/ui/crateaddserverwidget.h"
#include "crate/ui/cratefirstrun.h"
#include "crate/ui/crateprefsections.h"
#include "crate/ui/dialogs/dialogscrim.h"
#include "crate/ui/dialogs/preferenceschrome.h"
#include "crate/ui/dlgprefcrate.h"
#include "crate/ui/skin/crateserverstatus.h"
#include "crate/ui/skin/crateskin.h"
#include "crate/ui/skin/mixerstyle.h"
#include "crate/ui/skin/preferencescontrol.h"
#include "database/mixxxdb.h"
#include "dialog/dlgabout.h"
#include "library/library.h"
#include "library/sidebarmodel.h"
#include "library/trackcollectionmanager.h"
#include "library/trackmodel.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "mixxxmainwindow.h"
#include "preferences/colorpalettesettings.h"
#include "preferences/configobject.h"
#include "skin/legacy/launchimage.h"
#include "skin/skinloader.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxtest.h"
#include "track/cue.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/cmdlineargs.h"
#include "util/font.h"
#include "util/versionstore.h"
#include "waveform/visualplayposition.h"
#include "waveform/waveform.h"
#include "widget/wglwidget.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wtracktableview.h"

#ifdef MIXXX_USE_QOPENGL
#include <QOpenGLWindow>
#endif

namespace {

const QString kCrateDecks = QStringLiteral("CrateDecks");
const QString kLateNight = QStringLiteral("LateNight");

QString skinPath(const QString& skin) {
    return QDir(ConfigObject<ConfigValue>::computeResourcePath())
            .filePath(QStringLiteral("skins/") + skin);
}

/// Keeps the warnings that Qt and Mixxx log while it is alive, counts every
/// message, and passes them on to the handler that was installed before.
class WarningCollector {
  public:
    WarningCollector() {
        s_pCollector = this;
        s_previous = qInstallMessageHandler(&WarningCollector::handle);
    }
    ~WarningCollector() {
        qInstallMessageHandler(s_previous);
        s_pCollector = nullptr;
    }

    /// How many messages of any level it saw.
    int messageCount() const {
        return m_messageCount;
    }

    /// The warnings of skin parsing and styling: LegacySkinParser and
    /// SkinContext (SKIN_WARNING), templates, QSS and property connections.
    QStringList skinWarnings() const {
        QStringList result;
        for (const QString& warning : m_warnings) {
            if (warning.contains(QLatin1String("Skin parsing failed")) ||
                    warning.contains(QLatin1String("LegacySkinParser")) ||
                    warning.contains(QLatin1String("Could not open template")) ||
                    warning.contains(QLatin1String("Could not parse")) ||
                    warning.contains(QLatin1String("does not have a property named")) ||
                    warning.contains(QLatin1String("was not defined for widget")) ||
                    warning.contains(QLatin1String("Cannot read the skin variables"))) {
                result.append(warning);
            }
        }
        return result;
    }

  private:
    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message) {
        if (s_pCollector) {
            ++s_pCollector->m_messageCount;
            if (type == QtWarningMsg || type == QtCriticalMsg) {
                s_pCollector->m_warnings.append(message);
            }
        }
        if (s_previous) {
            s_previous(type, context, message);
        }
    }

    static inline WarningCollector* s_pCollector = nullptr;
    static inline QtMessageHandler s_previous = nullptr;
    QStringList m_warnings;
    int m_messageCount = 0;
};

/// Times the skin load from the log: from LegacySkinParser's "loading skin"
/// to SkinLoader's "Loaded skin", that is, parsing and building the skin.
class SkinLoadClock {
  public:
    SkinLoadClock() {
        s_pClock = this;
        s_previous = qInstallMessageHandler(&SkinLoadClock::handle);
        m_timer.start();
    }
    ~SkinLoadClock() {
        qInstallMessageHandler(s_previous);
        s_pClock = nullptr;
    }
    qint64 elapsedMs() const {
        return (m_startNs >= 0 && m_endNs >= m_startNs) ? (m_endNs - m_startNs) / 1000000 : -1;
    }

  private:
    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message) {
        if (s_pClock) {
            if (message.startsWith(QLatin1String("LegacySkinParser loading skin"))) {
                s_pClock->m_startNs = s_pClock->m_timer.nsecsElapsed();
            } else if (message.startsWith(QLatin1String("Loaded skin"))) {
                s_pClock->m_endNs = s_pClock->m_timer.nsecsElapsed();
            }
        }
        if (s_previous) {
            s_previous(type, context, message);
        }
    }

    static inline SkinLoadClock* s_pClock = nullptr;
    static inline QtMessageHandler s_previous = nullptr;
    QElapsedTimer m_timer;
    qint64 m_startNs = -1;
    qint64 m_endNs = -1;
};

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
bool waitFor(const std::function<bool()>& done, int timeoutMs) {
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

/// A track of the library of a disposable profile.
struct LibraryRow {
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString key;
    double bpm = 0;
    double duration = 0;
};

/// Adds `rows` to the library of the profile at `settingsPath`, creating
/// the library's schema first if no start has. Their files do not exist.
bool insertLibraryRows(const QString& settingsPath, const QList<LibraryRow>& rows) {
    const QString name = QStringLiteral("crate-library-rows");
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(settingsPath + QStringLiteral("mixxxdb.sqlite"));
        ok = db.open();
        ok = ok && MixxxDb::initDatabaseSchema(db);
        ok = ok && db.transaction();
        QSqlQuery location(db);
        location.prepare(QStringLiteral(
                "INSERT INTO track_locations (location, filename, directory, "
                "filesize, fs_deleted, needs_verification) "
                "VALUES (?, ?, ?, 1000000, 0, 0)"));
        QSqlQuery track(db);
        track.prepare(QStringLiteral(
                "INSERT INTO library (artist, title, album, genre, bpm, key, key_id, "
                "duration, location, mixxx_deleted, filetype) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 'flac')"));
        for (int i = 0; ok && i < rows.size(); ++i) {
            const LibraryRow& row = rows[i];
            const QString file = QStringLiteral("track-%1.flac").arg(i);
            location.addBindValue(QString(QStringLiteral("/nonexistent/crate/") + file));
            location.addBindValue(file);
            location.addBindValue(QStringLiteral("/nonexistent/crate"));
            ok = location.exec();
            track.addBindValue(row.artist);
            track.addBindValue(row.title);
            track.addBindValue(row.album);
            track.addBindValue(row.genre);
            track.addBindValue(row.bpm);
            track.addBindValue(row.key);
            track.addBindValue(row.key.isEmpty()
                            ? 0
                            : static_cast<int>(KeyUtils::guessKeyFromText(row.key)));
            track.addBindValue(row.duration);
            track.addBindValue(location.lastInsertId());
            ok = ok && track.exec();
        }
        ok = ok && db.commit();
        if (!ok) {
            qWarning() << "Library rows:" << db.lastError().text() << track.lastError().text();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

/// The rows of a tab-separated file: title, artist, album, genre, BPM, key
/// and duration in seconds, one track a line.
QList<LibraryRow> readLibraryRows(const QString& path) {
    QList<LibraryRow> rows;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return rows;
    }
    while (!file.atEnd()) {
        const QStringList fields = QString::fromUtf8(file.readLine()).trimmed().split(u'\t');
        if (fields.size() < 7) {
            continue;
        }
        rows.append({fields[0],
                fields[1],
                fields[2],
                fields[3],
                fields[5],
                fields[4].toDouble(),
                fields[6].toDouble()});
    }
    return rows;
}

/// Gives the profile at `settingsPath` a Crate server, dev.crate.local, as if
/// Decks had synced its library (plan F3.C-PR2): the tracks of a
/// tab-separated file (readLibraryRows()'s columns, then energy from 1 to 10
/// and profile quality), the mockups' 48 playlists and 12 crates, a few
/// genres and moods, recent and likes with every track, and the mockups'
/// library count. Nothing reaches a server: the sidebar and the tables are
/// painted from the profile.
bool insertCrateLibrary(const QString& settingsPath, const QString& rowsPath) {
    const QString name = QStringLiteral("crate-library-crate");
    const QString server = QStringLiteral("harness-server");
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(settingsPath + QStringLiteral("mixxxdb.sqlite"));
        ok = db.open() && MixxxDb::initDatabaseSchema(db) &&
                crate::db::createCrateServerSchema(db);
        crate::ServerProfile profile;
        profile.id = server;
        profile.baseUrl = QUrl(QStringLiteral("https://dev.crate.local"));
        ok = ok && crate::db::saveServerProfile(db, profile) &&
                crate::db::saveLastCapabilities(db,
                        server,
                        R"({"decks": {"available": true, "min_client_version": "0.1.0",
                            "contract_version": 1, "features": ["library_cursor"]}})");

        crate::library::ContainerTree tree;
        // The whole library, as the server's tree counts it
        tree.libraryTrackCount = 136512;
        const auto add = [&tree](const QString& kind, const QString& id, const QString& name) {
            crate::library::RemoteContainer container;
            container.kind = kind;
            container.remoteId = id;
            container.name = name;
            container.etag = kind + id;
            tree.containers.append(container);
        };
        const QStringList playlists = {QStringLiteral("Warm-up · Nov"),
                QStringLiteral("Peak hour"),
                QStringLiteral("Closing")};
        for (int i = 0; i < 48; ++i) {
            add(QStringLiteral("playlist"),
                    QString::number(i + 1),
                    i < playlists.size() ? playlists[i] : QStringLiteral("Set %1").arg(i + 1));
        }
        for (int i = 0; i < 12; ++i) {
            add(QStringLiteral("crate"),
                    QStringLiteral("crate-%1").arg(i),
                    QStringLiteral("Crate %1").arg(i + 1));
        }
        for (const QString& genre : {QStringLiteral("Melodic Techno"),
                     QStringLiteral("Deep House")}) {
            add(QStringLiteral("genre"), genre.toLower().replace(u' ', u'-'), genre);
        }
        for (const QString& mood : {QStringLiteral("Euphoric"), QStringLiteral("Dark")}) {
            add(QStringLiteral("mood"), mood.toLower(), mood);
        }
        add(QStringLiteral("recent"), QStringLiteral("recent"), QStringLiteral("Recent"));
        add(QStringLiteral("likes"), QStringLiteral("me"), QStringLiteral("Likes"));
        ok = ok &&
                crate::library::storeContainerTree(db, server, tree) ==
                        crate::library::WriteStatus::Ok;

        QList<crate::library::RemoteTrack> tracks;
        QFile file(rowsPath);
        ok = ok && file.open(QIODevice::ReadOnly | QIODevice::Text);
        while (ok && !file.atEnd()) {
            const QStringList fields = QString::fromUtf8(file.readLine()).trimmed().split(u'\t');
            if (fields.size() < 9) {
                continue;
            }
            crate::library::RemoteTrack track;
            track.entityUid = QStringLiteral("uid-%1").arg(tracks.size());
            track.title = fields[0];
            track.artist = fields[1];
            track.album = fields[2];
            track.genre = fields[3];
            track.bpm = fields[4].toDouble();
            track.camelot = fields[5];
            track.durationMs = static_cast<qint64>(fields[6].toDouble() * 1000);
            track.energy = fields[7].toDouble() / 10;
            track.profileQuality = fields[8] == QLatin1String("none") ? QString() : fields[8];
            track.format = QStringLiteral("flac");
            track.playable = true;
            tracks.append(track);
        }
        ok = ok && !tracks.isEmpty() &&
                crate::library::storeTracks(db,
                        server,
                        tracks,
                        QStringLiteral("2026-11-09T10:00:00Z")) ==
                        crate::library::WriteStatus::Ok;
        QStringList uids;
        for (const auto& track : std::as_const(tracks)) {
            uids.append(track.entityUid);
        }
        for (const auto& container : crate::library::loadContainers(db, server)) {
            QStringList stored;
            if (container.kind == QLatin1String("likes")) {
                stored = uids;
            } else if (container.kind == QLatin1String("playlist") &&
                    container.remoteId == QLatin1String("1")) {
                stored = uids.mid(0, 12);
            } else {
                continue;
            }
            ok = ok &&
                    crate::library::replaceContainerTracks(db,
                            container.id,
                            stored,
                            {{{QString(),
                                     QStringLiteral("\"h\""),
                                     static_cast<int>(stored.size())}},
                                    container.etag,
                                    QStringLiteral("2026-11-09T10:00:00Z")}) ==
                            crate::library::WriteStatus::Ok;
        }
        if (!ok) {
            qWarning() << "Crate library:" << db.lastError().text();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
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
        auto config = UserSettingsPointer(new UserSettings(m_settingsPath + "mixxx.cfg"));
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

class CrateSkinLoadTest : public MixxxTest {
  protected:
    /// Loads `skin` and returns its skin warnings.
    QStringList loadSkin(const QString& skin, QString* pLoadedRootName = nullptr) {
        QTemporaryDir profile;
        EXPECT_TRUE(profile.isValid());
        OffscreenDecks decks(profile.path() + QLatin1Char('/'));
        decks.writeProfile(skin);
        std::unique_ptr<WarningCollector> pWarnings;
        decks.start(application(), [&pWarnings] {
            pWarnings = std::make_unique<WarningCollector>();
        });
        // Deferred restyles and the first paint
        waitFor([] { return false; }, 500);
        if (pLoadedRootName && decks.skin()) {
            *pLoadedRootName = decks.skin()->objectName();
        }
        EXPECT_NE(nullptr, decks.skin());
        EXPECT_GT(pWarnings->messageCount(), 0) << "The log handler saw nothing";
        return pWarnings->skinWarnings();
    }
};

TEST_F(CrateSkinLoadTest, CrateDecksSkinLoadsWithoutParserWarnings) {
    const QStringList warnings = loadSkin(kCrateDecks);
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
}

TEST_F(CrateSkinLoadTest, IsDefaultSkin) {
    mixxx::skin::SkinLoader loader(config());
    EXPECT_QSTRING_EQ(kCrateDecks, loader.getDefaultSkinName());
    const auto pSkin = loader.getSkin(kCrateDecks);
    ASSERT_TRUE(pSkin);
    EXPECT_TRUE(pSkin->isValid());
    // A single scheme (plan V2.PR1)
    EXPECT_EQ(QStringList{QStringLiteral("Crate")}, pSkin->colorschemes());
}

TEST_F(CrateSkinLoadTest, LateNightStillLoads) {
    const QStringList warnings = loadSkin(kLateNight);
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
}

TEST_F(CrateSkinLoadTest, ToolbarOpensPreferences) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    const auto preferences = [] {
        const QList<QWidget*> windows = QApplication::topLevelWidgets();
        for (QWidget* pWindow : windows) {
            if (pWindow->inherits("DlgPreferences")) {
                return pWindow;
            }
        }
        return static_cast<QWidget*>(nullptr);
    };
    QWidget* pPreferences = preferences();
    ASSERT_NE(nullptr, pPreferences);
    EXPECT_FALSE(pPreferences->isVisible());

    // A click on the toolbar's settings button
    auto* pButton = decks.skin()->findChild<QWidget*>(QStringLiteral("TbIconSettings"));
    ASSERT_NE(nullptr, pButton);
    const QPoint centre = pButton->rect().center();
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type,
                centre,
                pButton->mapToGlobal(centre),
                Qt::LeftButton,
                type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                Qt::NoModifier);
        QCoreApplication::sendEvent(pButton, &event);
    }
    EXPECT_TRUE(pPreferences->isVisible());
    EXPECT_EQ(0.0, ControlObject::get(crate::showPreferencesKey()));
    pPreferences->hide();
}

TEST_F(CrateSkinLoadTest, PreferencesHaveTheMockupsDialog) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    QWidget* pPreferences = nullptr;
    for (QWidget* pWindow : QApplication::topLevelWidgets()) {
        if (pWindow->inherits("DlgPreferences")) {
            pPreferences = pWindow;
        }
    }
    ASSERT_NE(nullptr, pPreferences);

    // Its own frame and title row (GU1 mockups preferences-crate)
    EXPECT_TRUE(pPreferences->windowFlags().testFlag(Qt::FramelessWindowHint));
    auto* pTitle = pPreferences->findChild<QLabel*>(QStringLiteral("MonoDialogTitle"));
    ASSERT_NE(nullptr, pTitle);
    EXPECT_QSTRING_EQ(QStringLiteral("Crate Decks · Preferences"), pTitle->text());

    // The Crate page last, with the Crate logo
    auto* pTree = pPreferences->findChild<QTreeWidget*>(QStringLiteral("contentsTreeWidget"));
    ASSERT_NE(nullptr, pTree);
    QTreeWidgetItem* pLast = pTree->topLevelItem(pTree->topLevelItemCount() - 1);
    EXPECT_QSTRING_EQ(QStringLiteral("Crate"), pLast->text(0));
    EXPECT_TRUE(pLast->data(0, crate::kPreferencesLogoRole).toBool());

    // The foot in the mockup's order; Mixxx's own buttons stay, hidden
    auto* pButtons = pPreferences->findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"));
    ASSERT_NE(nullptr, pButtons);
    EXPECT_TRUE(pButtons->isHidden());
    QStringList foot;
    QPushButton* pOk = nullptr;
    for (QPushButton* pButton :
            pPreferences->findChildren<QPushButton*>(QStringLiteral("CratePrefsButton"))) {
        foot.append(pButton->text());
        if (pButton->isDefault()) {
            pOk = pButton;
        }
    }
    EXPECT_EQ((QStringList{QStringLiteral("Restore Defaults"),
                      QStringLiteral("Help"),
                      QStringLiteral("Cancel"),
                      QStringLiteral("Apply"),
                      QStringLiteral("Ok")}),
            foot);
    ASSERT_NE(nullptr, pOk);
    pPreferences->show();
    EXPECT_TRUE(pPreferences->isVisible());
    pOk->click();
    EXPECT_FALSE(pPreferences->isVisible());
}

TEST_F(CrateSkinLoadTest, OnlyBlockingDialogsDimTheMainWindow) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->show();
    QCoreApplication::processEvents();
    const auto dimmed = [] {
        const crate::DialogScrim* pScrim = crate::blockingDialogScrim();
        return pScrim && pScrim->isActive();
    };
    QWidget* pPreferences = nullptr;
    for (QWidget* pWindow : QApplication::topLevelWidgets()) {
        if (pWindow->inherits("DlgPreferences")) {
            pPreferences = pWindow;
        }
    }
    ASSERT_NE(nullptr, pPreferences);

    // Preferences from the toolbar: the DJ adjusts something mid-set,
    // watching the decks (design 14.3)
    ControlObject::set(crate::showPreferencesKey(), 1);
    ASSERT_TRUE(pPreferences->isVisible());
    EXPECT_FALSE(dimmed());
    pPreferences->hide();
    // Nor when Mixxx opens it modal, after a sound device fails. Checked
    // before events run: the DialogRejecter closes modal dialogs then
    pPreferences->setWindowModality(Qt::ApplicationModal);
    pPreferences->show();
    EXPECT_FALSE(dimmed());
    pPreferences->hide();
    pPreferences->setWindowModality(Qt::NonModal);

    // A confirmation as Mixxx's on exit dims the main window until it closes
    QMessageBox confirm(QMessageBox::Question,
            QStringLiteral("Confirm Exit"),
            QStringLiteral("A deck is currently playing. Exit Mixxx?"),
            QMessageBox::Yes | QMessageBox::No,
            decks.mainWindow());
    confirm.open();
    EXPECT_TRUE(dimmed());
    EXPECT_TRUE(waitFor([&confirm] { return !confirm.isVisible(); }, 5000));
    EXPECT_FALSE(dimmed());
}

/// The settings and controls of the racks that the skin folds (design 14.3)
const QList<ConfigKey> kRackKeys = {
        ConfigKey(QStringLiteral("[Skin]"), QStringLiteral("show_effectrack")),
        ConfigKey(QStringLiteral("[Skin]"), QStringLiteral("show_samplers")),
        ConfigKey(QStringLiteral("[Skin]"), QStringLiteral("show_microphones")),
};

TEST_F(CrateSkinLoadTest, RacksStartFolded) {
    // A first start: SkinControls creates the rack controls at 1 before the
    // skin loads, and the skin's defaults must still apply
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    for (const ConfigKey& key : kRackKeys) {
        EXPECT_EQ(0.0, ControlObject::get(key)) << key.item.toStdString();
    }
    auto* pMicAux = decks.skin()->findChild<QWidget*>(QStringLiteral("MicAuxRack"));
    ASSERT_NE(nullptr, pMicAux);
    EXPECT_FALSE(pMicAux->isVisible());
}

TEST_F(CrateSkinLoadTest, RacksKeepTheUsersChoice) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    OffscreenDecks::Settings settings;
    for (const ConfigKey& key : kRackKeys) {
        settings.append({key, QStringLiteral("1")});
    }
    decks.writeProfile(kCrateDecks, settings);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    for (const ConfigKey& key : kRackKeys) {
        EXPECT_EQ(1.0, ControlObject::get(key)) << key.item.toStdString();
    }
}

/// The widget of `type` named `name` inside the strip of deck `letter`
/// (two decks), found by the strip's deck letter.
template<typename T>
T* findInStrip(QWidget* pSkin, const QString& letter, const QString& name) {
    const QList<QWidget*> strips = pSkin->findChildren<QWidget*>(QStringLiteral("CrateStrip"));
    for (QWidget* pStrip : strips) {
        const QList<QLabel*> labels = pStrip->findChildren<QLabel*>();
        const bool isDeck = std::any_of(labels.begin(), labels.end(), [&letter](QLabel* pLabel) {
            return pLabel->objectName().startsWith(QLatin1String("CrateStripLetter")) &&
                    pLabel->text() == letter;
        });
        if (isDeck) {
            return pStrip->findChild<T*>(name);
        }
    }
    return nullptr;
}

/// The brightest pixel of `image` around `centre`
QRgb brightestAround(const QImage& image, QPoint centre, int reach) {
    QRgb brightest = qRgb(0, 0, 0);
    for (int y = centre.y() - reach; y <= centre.y() + reach; ++y) {
        for (int x = centre.x() - reach; x <= centre.x() + reach; ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qGray(pixel) > qGray(brightest)) {
                brightest = pixel;
            }
        }
    }
    return brightest;
}

TEST_F(CrateSkinLoadTest, KnobArcsTakeTheDeckColour) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->resize(1920, 1080);
    // The high EQ of both decks all the way up: the arc runs from 12
    // o'clock past 3 o'clock, where only the arc is drawn
    for (const char* group : {"[EqualizerRack1_[Channel1]_Effect1]",
                 "[EqualizerRack1_[Channel2]_Effect1]"}) {
        ControlObject::getControl(ConfigKey(group, "parameter3"))->setParameter(1);
    }
    waitFor([] { return false; }, 300);

    const crate::SkinVariables variables = crate::readSkinVariables(
            QDir(skinPath(kCrateDecks)).filePath(QStringLiteral("generated/vars.xml")));
    const double radius = variables.value(QStringLiteral("CrateKnobArcRadius")).toDouble();
    ASSERT_GT(radius, 0);
    for (const auto& [letter, colourName] :
            {std::pair{QStringLiteral("A"), QStringLiteral("CrateDeckAC")},
                    std::pair{QStringLiteral("B"), QStringLiteral("CrateDeckBD")}}) {
        // The second knob of a strip is the high EQ
        auto* pStrip = findInStrip<QWidget>(
                decks.skin(), letter, QStringLiteral("CrateStripKnobs"));
        ASSERT_NE(nullptr, pStrip) << letter.toStdString();
        const QList<QWidget*> knobs = pStrip->findChildren<QWidget*>(QStringLiteral("CrateKnob"));
        ASSERT_EQ(5, knobs.size()) << letter.toStdString();
        QWidget* pHigh = knobs[1];
        // On the skin's background: grab() would fill it with the palette's
        QImage image(pHigh->size(), QImage::Format_RGB32);
        image.fill(QColor(variables.value(QStringLiteral("CrateBgApp"))));
        pHigh->render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
        const QPoint threeOClock(qRound(image.width() / 2.0 + radius), image.height() / 2);
        const QRgb arc = brightestAround(image, threeOClock, 1);
        const QColor deck(variables.value(colourName));
        EXPECT_NEAR(deck.red(), qRed(arc), 40) << letter.toStdString();
        EXPECT_NEAR(deck.green(), qGreen(arc), 40) << letter.toStdString();
        EXPECT_NEAR(deck.blue(), qBlue(arc), 40) << letter.toStdString();
    }
}

TEST_F(CrateSkinLoadTest, FaderHandleFillsWhileItMoves) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->resize(1920, 1080);
    waitFor([] { return false; }, 300);
    auto* pFader = decks.skin()->findChild<QWidget*>(QStringLiteral("CrateFaderChannelBD"));
    ASSERT_NE(nullptr, pFader);
    const crate::SkinVariables variables = crate::readSkinVariables(
            QDir(skinPath(kCrateDecks)).filePath(QStringLiteral("generated/vars.xml")));
    const crate::FaderStyle style = crate::FaderStyle::fromVariables(variables);
    const QColor deck(variables.value(QStringLiteral("CrateDeckBD")));
    const auto handleFill = [pFader, &style](double parameter) {
        const int edge = crate::faderHandleEdge(
                pFader->height(), style.handleLength, parameter, false);
        // Inside the outline, away from the centre line
        return pFader->grab().toImage().pixel(pFader->width() / 2 - 5, edge + 3);
    };
    // At rest the handle is the outline over the background
    ControlObject::getControl(ConfigKey("[Channel2]", "volume"))->setParameter(0.3);
    waitFor([] { return false; }, style.activeHoldMs + 300);
    EXPECT_LT(qGray(handleFill(0.3)), 60);
    // Moving, it fills with the deck colour, and empties after the hold
    ControlObject::getControl(ConfigKey("[Channel2]", "volume"))->setParameter(0.6);
    waitFor([] { return false; }, 50);
    const QRgb moving = handleFill(0.6);
    EXPECT_NEAR(deck.red(), qRed(moving), 10);
    EXPECT_NEAR(deck.blue(), qBlue(moving), 10);
    waitFor([] { return false; }, style.activeHoldMs + 300);
    EXPECT_LT(qGray(handleFill(0.6)), 60);
    // With sync, the pitch fader stays filled
    ControlObject::set(ConfigKey("[Channel2]", "sync_enabled"), 1);
    waitFor([] { return false; }, style.activeHoldMs + 300);
    auto* pPitch = decks.skin()->findChild<QWidget*>(QStringLiteral("CrateFaderPitchBD"));
    ASSERT_NE(nullptr, pPitch);
    const double rate = ControlObject::getControl(ConfigKey("[Channel2]", "rate"))->getParameter();
    const int edge = crate::faderHandleEdge(pPitch->height(), style.handleLength, rate, false);
    const QRgb synced = pPitch->grab().toImage().pixel(pPitch->width() / 2 - 5, edge + 3);
    EXPECT_NEAR(deck.red(), qRed(synced), 10);
}

TEST_F(CrateSkinLoadTest, MetersIgnoreTheMouse) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    const QList<QWidget*> widgets = decks.skin()->findChildren<QWidget*>();
    int meters = 0;
    for (QWidget* pWidget : widgets) {
        if (pWidget->objectName().startsWith(QLatin1String("CrateMeter"))) {
            ++meters;
            EXPECT_TRUE(pWidget->testAttribute(Qt::WA_TransparentForMouseEvents))
                    << pWidget->objectName().toStdString();
        }
    }
    // Two channels and main with two decks, four and main with four, and
    // the two toolbar lanes
    EXPECT_EQ(2 + 2 + 4 + 2 + 2, meters);
}

/// The texts that `pLayout` shows, in its order, without blanks.
void appendShownTexts(QLayout* pLayout, const QWidget* pTop, QStringList* pTexts) {
    for (int i = 0; i < pLayout->count(); ++i) {
        QLayoutItem* pItem = pLayout->itemAt(i);
        QWidget* pWidget = pItem->widget();
        if (!pWidget) {
            if (pItem->layout()) {
                appendShownTexts(pItem->layout(), pTop, pTexts);
            }
            continue;
        }
        if (!pWidget->isVisibleTo(pTop)) {
            continue;
        }
        if (auto* pLabel = qobject_cast<QLabel*>(pWidget)) {
            const QString text = pLabel->text().trimmed();
            if (!text.isEmpty()) {
                pTexts->append(text);
            }
        } else if (pWidget->layout()) {
            appendShownTexts(pWidget->layout(), pTop, pTexts);
        }
    }
}

/// The MonoLabel of the header of deck `letter`, while it shows.
QWidget* deckHeadMeta(QWidget* pSkin, const QString& letter) {
    const QList<QWidget*> heads = pSkin->findChildren<QWidget*>(QStringLiteral("CrateDeckHead"));
    for (QWidget* pHead : heads) {
        const QList<QLabel*> labels = pHead->findChildren<QLabel*>();
        const bool isDeck = std::any_of(labels.begin(), labels.end(), [&letter](QLabel* pLabel) {
            return pLabel->objectName().startsWith(QLatin1String("CrateDeckLetter")) &&
                    pLabel->text() == letter;
        });
        auto* pMeta = pHead->findChild<QWidget*>(QStringLiteral("CrateDeckHeadMeta"));
        if (isDeck && pHead->isVisible() && pMeta && pMeta->layout()) {
            return pMeta;
        }
    }
    return nullptr;
}

/// What the MonoLabel of the header of deck `letter` reads, one text per
/// label ("Deck A", "·", "8A", "·", "124.00", "BPM").
QStringList deckHeadTexts(QWidget* pSkin, const QString& letter) {
    QStringList texts;
    if (QWidget* pMeta = deckHeadMeta(pSkin, letter)) {
        appendShownTexts(pMeta->layout(), pMeta, &texts);
    }
    return texts;
}

/// How many pixels `pWidget` paints itself: no background, no parents.
int paintedPixels(QWidget* pWidget) {
    QImage image(pWidget->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    pWidget->render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    int painted = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            painted += qAlpha(image.pixel(x, y)) > 128 ? 1 : 0;
        }
    }
    return painted;
}

TEST_F(CrateSkinLoadTest, DeckHeadHidesEmptyFields) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    const QString group = QStringLiteral("[Channel1]");
    // forceSet: track_loaded is read-only, and key has a request handler
    const auto set = [&group](const char* item, double value) {
        ControlObject* pControl = ControlObject::getControl(group, QString::fromLatin1(item));
        ASSERT_NE(nullptr, pControl) << item;
        pControl->forceSet(value);
    };
    // The texts of the header joined by "|", which reads well in a failure
    const auto head = [&decks] {
        waitFor([] { return false; }, 100);
        return deckHeadTexts(decks.skin(), QStringLiteral("A")).join(u'|');
    };
    const double key = KeyUtils::keyToNumericValue(mixxx::track::io::key::A_MINOR);

    // A loaded track whose key the header does not show: none, or not
    // shown yet (visual_key follows key on the GUI tick, which only runs
    // with the engine). Neither the key nor its separator remain
    set("track_loaded", 1);
    set("file_bpm", 151.14);
    set("key", 0);
    set("visual_key", 0);
    EXPECT_QSTRING_EQ(QStringLiteral("Deck A|·|151.14|BPM"), head());
    set("key", key);
    EXPECT_QSTRING_EQ(QStringLiteral("Deck A|·|151.14|BPM"), head());

    // With its key, between two separators
    set("visual_key", key);
    const QStringList withKey = head().split(u'|');
    ASSERT_EQ(6, withKey.size()) << withKey.join(u'|').toStdString();
    EXPECT_QSTRING_EQ(QStringLiteral("Deck A|·"), withKey.mid(0, 2).join(u'|'));
    EXPECT_FALSE(withKey[2].isEmpty());
    EXPECT_QSTRING_EQ(QStringLiteral("·|151.14|BPM"), withKey.mid(3).join(u'|'));
    // And painted as the MonoLabel it is in the mockups, whether Mixxx's key
    // colours are on (its default: WKey adds a bar of the key's colour) or
    // off
    QWidget* pMeta = deckHeadMeta(decks.skin(), QStringLiteral("A"));
    ASSERT_NE(nullptr, pMeta);
    auto* pKey = pMeta->findChild<QLabel*>(QStringLiteral("MonoDeckHeadKey"));
    ASSERT_NE(nullptr, pKey);
    ColorPaletteSettings colours(decks.coreServices()->getSettings());
    colours.setKeyColorsEnabled(false);
    const int painted = paintedPixels(pKey);
    EXPECT_GT(painted, 10);
    colours.setKeyColorsEnabled(true);
    EXPECT_EQ(painted, paintedPixels(pKey));
    // In its own case: "Am" is A minor, "AM" would read as A major
    EXPECT_EQ(QFont::MixedCase, pKey->font().capitalization());

    // Without a BPM the same rule: no "0.00 BPM", no separator
    set("file_bpm", 0);
    EXPECT_QSTRING_EQ(QStringLiteral("Deck A|·|") + withKey[2], head());
    set("key", 0);
    set("visual_key", 0);
    EXPECT_QSTRING_EQ(QStringLiteral("Deck A"), head());

    // An empty deck
    set("track_loaded", 0);
    EXPECT_QSTRING_EQ(QStringLiteral("Deck A|·|No track"), head());
}

/// The "Artist · Genre" row under the title of deck `letter`, while it shows.
QWidget* deckArtistRow(QWidget* pSkin, const QString& letter) {
    const QList<QWidget*> rows =
            pSkin->findChildren<QWidget*>(QStringLiteral("CrateDeckArtistRow"));
    for (QWidget* pRow : rows) {
        if (!pRow->isVisible() || !pRow->layout()) {
            continue;
        }
        // The first parent with a deck letter is the row's deck
        for (QWidget* pParent = pRow->parentWidget(); pParent; pParent = pParent->parentWidget()) {
            const QList<QLabel*> labels = pParent->findChildren<QLabel*>();
            const auto letterLabel = std::find_if(labels.begin(), labels.end(), [](QLabel* pLabel) {
                return pLabel->objectName().startsWith(QLatin1String("CrateDeckLetter"));
            });
            if (letterLabel != labels.end()) {
                if ((*letterLabel)->text() == letter) {
                    return pRow;
                }
                break;
            }
        }
    }
    return nullptr;
}

TEST_F(CrateSkinLoadTest, DeckArtistRowHidesEmptyFields) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    // As the mockups: a narrow window squeezes the title column instead
    decks.mainWindow()->resize(1920, 1080);
    const QString group = QStringLiteral("[Channel1]");
    BaseTrackPlayer* pPlayer = decks.coreServices()->getPlayerManager()->getPlayer(group);
    ASSERT_NE(nullptr, pPlayer);
    ControlObject* pTrackLoaded = ControlObject::getControl(group, QStringLiteral("track_loaded"));
    ASSERT_NE(nullptr, pTrackLoaded);
    // Loads `pTrack` as the end of a load does, without its audio: the
    // labels follow the player, Crate follows PlayerInfo
    const auto load = [&](const TrackPointer& pTrack) {
        pTrackLoaded->forceSet(1);
        emit pPlayer->newTrackLoaded(pTrack);
        PlayerInfo::instance().setTrackInfo(group, pTrack);
    };
    // The texts of the row joined by "|", which reads well in a failure
    const auto row = [&decks] {
        waitFor([] { return false; }, 100);
        QStringList texts;
        if (QWidget* pRow = deckArtistRow(decks.skin(), QStringLiteral("A"))) {
            appendShownTexts(pRow->layout(), pRow, &texts);
        }
        return texts.join(u'|');
    };
    const auto rowHeight = [&decks] {
        QWidget* pRow = deckArtistRow(decks.skin(), QStringLiteral("A"));
        return pRow ? pRow->height() : -1;
    };
    const auto trackWith = [](const QString& artist, const QString& genre) {
        TrackPointer pTrack = Track::newTemporary();
        pTrack->setTitle(QStringLiteral("Strut"));
        pTrack->setArtist(artist);
        pTrack->updateGenre(genre);
        return pTrack;
    };

    load(trackWith(QStringLiteral("SNAYX"), QStringLiteral("Alternative")));
    EXPECT_QSTRING_EQ(QStringLiteral("SNAYX|·|Alternative"), row());
    const int height = rowHeight();
    EXPECT_GT(height, 0);

    // A missing field hides with the separator: never "SNAYX ·"
    load(trackWith(QStringLiteral("SNAYX"), QString()));
    EXPECT_QSTRING_EQ(QStringLiteral("SNAYX"), row());
    load(trackWith(QString(), QStringLiteral("Alternative")));
    EXPECT_QSTRING_EQ(QStringLiteral("Alternative"), row());
    load(trackWith(QStringLiteral(" "), QStringLiteral(" ")));
    EXPECT_QSTRING_EQ(QString(), row());
    // The row keeps its line, so that the title above does not move: Qt
    // measures an empty line 1 px shorter than one with glyphs
    EXPECT_NEAR(height, rowHeight(), 1);

    // An edit of the loaded track shows at once. A track of the library:
    // only a track with an id tells the labels that it changed
    const QString file = profile.filePath(QStringLiteral("edited.flac"));
    ASSERT_TRUE(QFile::copy(getTestFile(u".flac"), file));
    const TrackPointer pTrack =
            decks.coreServices()->getTrackCollectionManager()->getOrAddTrack(
                    TrackRef::fromFilePath(file));
    ASSERT_NE(nullptr, pTrack);
    ASSERT_TRUE(pTrack->getId().isValid());
    pTrack->setArtist(QStringLiteral("SNAYX"));
    pTrack->updateGenre(QString());
    load(pTrack);
    EXPECT_QSTRING_EQ(QStringLiteral("SNAYX"), row());
    pTrack->updateGenre(QStringLiteral("Alternative"));
    EXPECT_QSTRING_EQ(QStringLiteral("SNAYX|·|Alternative"), row());
    pTrack->setArtist(QString());
    EXPECT_QSTRING_EQ(QStringLiteral("Alternative"), row());

    PlayerInfo::instance().setTrackInfo(group, TrackPointer());
}

class CrateSkinTest : public MixxxTest {
  protected:
    static void SetUpTestSuite() {
        // The same call as CoreServices at startup: the admin's fonts
        FontUtils::initializeFonts(ConfigObject<ConfigValue>::computeResourcePath());
    }
};

TEST_F(CrateSkinTest, ReadsTheGeneratedVariables) {
    const crate::SkinVariables variables = crate::readSkinVariables(
            QDir(skinPath(kCrateDecks)).filePath(QStringLiteral("generated/vars.xml")));
    const crate::SkinBreakpoints breakpoints = crate::SkinBreakpoints::fromVariables(variables);
    // The 1366 x 768 layout of the GU1 mockups
    EXPECT_TRUE(breakpoints.isValid());
    EXPECT_GT(breakpoints.narrowBelow, 1366);
    EXPECT_LT(breakpoints.narrowBelow, 1920);
    EXPECT_GT(breakpoints.shortBelow, 768);
    EXPECT_LT(breakpoints.shortBelow, 1080);
    // Another skin has no generated variables
    EXPECT_TRUE(crate::readSkinVariables(
            QDir(skinPath(kLateNight)).filePath(QStringLiteral("generated/vars.xml")))
                    .isEmpty());
}

TEST_F(CrateSkinTest, SkinDefaultsReachControlsThatExist) {
    QTemporaryDir skin;
    ASSERT_TRUE(skin.isValid());
    const QDir skinDir(skin.path());
    ASSERT_TRUE(skinDir.mkpath(QStringLiteral("generated")));
    QFile vars(skinDir.filePath(QStringLiteral("generated/vars.xml")));
    ASSERT_TRUE(vars.open(QIODevice::WriteOnly));
    vars.write("<Template/>\n");
    vars.close();
    QFile manifest(skinDir.filePath(QStringLiteral("skin.xml")));
    ASSERT_TRUE(manifest.open(QIODevice::WriteOnly));
    manifest.write(
            "<skin><manifest><attributes>\n"
            "<attribute persist=\"true\" config_key=\"[Test],folded\">0</attribute>\n"
            "<attribute persist=\"true\" config_key=\"[Test],chosen\">0</attribute>\n"
            "<attribute config_key=\"[Test],session\">0</attribute>\n"
            "<attribute persist=\"true\" config_key=\"[Test],absent\">0</attribute>\n"
            "</attributes></manifest></skin>\n");
    manifest.close();

    const ConfigKey folded(QStringLiteral("[Test]"), QStringLiteral("folded"));
    const ConfigKey chosen(QStringLiteral("[Test]"), QStringLiteral("chosen"));
    const ConfigKey session(QStringLiteral("[Test]"), QStringLiteral("session"));
    const ConfigKey absent(QStringLiteral("[Test]"), QStringLiteral("absent"));
    // Controls created before the skin, as SkinControls does, at 1
    ControlObject foldedControl(folded);
    ControlObject chosenControl(chosen);
    ControlObject sessionControl(session);
    foldedControl.set(1);
    chosenControl.set(1);
    sessionControl.set(1);
    // The user chose before: that setting stays
    config()->set(chosen, ConfigValue(QStringLiteral("1")));

    crate::applySkinDefaults(config(), skin.path());
    EXPECT_EQ(0.0, foldedControl.get());
    EXPECT_QSTRING_EQ(QStringLiteral("0"), config()->getValueString(folded));
    EXPECT_EQ(1.0, chosenControl.get());
    EXPECT_QSTRING_EQ(QStringLiteral("1"), config()->getValueString(chosen));
    // Attributes that do not persist are the parser's, every load
    EXPECT_EQ(1.0, sessionControl.get());
    // A control that does not exist yet is the parser's too
    EXPECT_TRUE(config()->getValueString(absent).isEmpty());

    // Other skins (no generated variables) are left as they are
    foldedControl.set(1);
    config()->set(folded, ConfigValue(QString()));
    crate::applySkinDefaults(config(), skinPath(kLateNight));
    QFile::remove(skinDir.filePath(QStringLiteral("generated/vars.xml")));
    crate::applySkinDefaults(config(), skin.path());
    EXPECT_EQ(1.0, foldedControl.get());
}

TEST_F(CrateSkinTest, LayoutClassFollowsTheBreakpoints) {
    const crate::SkinBreakpoints breakpoints{1500, 800};
    EXPECT_EQ((crate::SkinLayoutClass{false, false}),
            crate::skinLayoutClassFor(QSize(1920, 1080), breakpoints));
    EXPECT_EQ((crate::SkinLayoutClass{true, true}),
            crate::skinLayoutClassFor(QSize(1366, 768), breakpoints));
    EXPECT_EQ((crate::SkinLayoutClass{true, false}),
            crate::skinLayoutClassFor(QSize(1440, 900), breakpoints));
    EXPECT_EQ((crate::SkinLayoutClass{false, true}),
            crate::skinLayoutClassFor(QSize(1920, 720), breakpoints));
    // Without breakpoints there is a single layout
    EXPECT_EQ((crate::SkinLayoutClass{false, false}),
            crate::skinLayoutClassFor(QSize(800, 600), crate::SkinBreakpoints{}));
}

TEST_F(CrateSkinTest, MonoLabelStyleHasCapitalsAndTracking) {
    // The generated style of the skin, as the skin loads it
    QFile style(QDir(skinPath(kCrateDecks)).filePath(QStringLiteral("generated/style.qss")));
    ASSERT_TRUE(style.open(QIODevice::ReadOnly));
    QWidget skin;
    skin.setStyleSheet(QString::fromUtf8(style.readAll()));
    auto* pLabel = new QLabel(QStringLiteral("Deck A"), &skin);
    pLabel->setObjectName(QStringLiteral("MonoDeckHead"));
    pLabel->ensurePolished();
    const QFont font = pLabel->font();
    EXPECT_QSTRING_EQ(QStringLiteral("JetBrains Mono"), font.family());
    // The size the GU1 mockups paint: 0.6875rem on a 15 px root, given in
    // points at QSS's 96 dpi because QFont rounds pixel sizes
    constexpr double kMockupLabel = 0.6875 * 15;
    EXPECT_NEAR(kMockupLabel, font.pointSizeF() * 96 / 72, 0.001);
    // MonoLabel.tsx: uppercase with 0.14 em of tracking
    EXPECT_EQ(QFont::AllUppercase, font.capitalization());
    EXPECT_EQ(QFont::AbsoluteSpacing, font.letterSpacingType());
    EXPECT_NEAR(0.14 * kMockupLabel, font.letterSpacing(), 0.02);
}

TEST_F(CrateSkinTest, ToolbarButtonOpensPreferences) {
    // The main window's menu bar and its Preferences action, as
    // WMainMenuBar makes it, and the control the skin's manifest declares
    QWidget window;
    auto* pMenuBar = new QMenuBar(&window);
    auto* pOther = new QAction(QStringLiteral("About"), pMenuBar);
    pOther->setMenuRole(QAction::AboutRole);
    auto* pPreferences = new QAction(QStringLiteral("Preferences"), pMenuBar);
    pPreferences->setMenuRole(QAction::PreferencesRole);
    int opened = 0;
    QObject::connect(pPreferences, &QAction::triggered, [&opened] { ++opened; });
    ControlPushButton control(crate::showPreferencesKey());
    auto* pSkin = new QWidget(&window);
    EXPECT_EQ(pPreferences, crate::findPreferencesAction(&window));

    new crate::PreferencesControl(pSkin);
    control.set(1);
    EXPECT_EQ(1, opened);
    // A push: back to 0, ready for the next click
    EXPECT_EQ(0.0, control.get());
    control.set(0);
    EXPECT_EQ(1, opened);
    control.set(1);
    EXPECT_EQ(2, opened);

    // Without a Preferences action nothing happens
    QWidget alone;
    EXPECT_EQ(nullptr, crate::findPreferencesAction(&alone));
    EXPECT_EQ(nullptr, crate::findPreferencesAction(nullptr));
}

TEST_F(CrateSkinTest, DeckNumberHasTabularFigures) {
    QFont display(QStringLiteral("Bricolage Grotesque"));
    display.setPixelSize(34);
    display.setWeight(QFont::Bold);
    const QFont number = crate::deckNumberFont(display);
    const QFontMetricsF metrics(number);
    // Every digit has the same advance, so the BPM does not jitter
    const qreal one = metrics.horizontalAdvance(QStringLiteral("111.11"));
    const qreal eight = metrics.horizontalAdvance(QStringLiteral("888.88"));
    EXPECT_NEAR(one, eight, 0.01);
    EXPECT_FALSE(QFontMetricsF(display).horizontalAdvance(QStringLiteral("111.11")) ==
            QFontMetricsF(display).horizontalAdvance(QStringLiteral("888.88")));
}

TEST_F(CrateSkinTest, ServerStatusWithoutServers) {
    crate::ServerStatus status(nullptr);
    EXPECT_EQ(crate::ServerStatus::State::None, status.state());
    EXPECT_QSTRING_EQ(QStringLiteral("none"), status.stateName());

    QWidget skin;
    auto* pLabel = new QLabel(&skin);
    pLabel->setObjectName(QStringLiteral("MonoCrateStatus"));
    auto* pDot = new QLabel(&skin);
    pDot->setObjectName(QStringLiteral("CrateStatusDot"));
    crate::bindServerStatus(&status, &skin);
    EXPECT_QSTRING_EQ(status.text(), pLabel->text());

    status.showStatus(QStringLiteral("dev.crate.local"), crate::ServerStatus::State::Connected);
    EXPECT_TRUE(pLabel->text().contains(QLatin1String("dev.crate.local")));
    EXPECT_QSTRING_EQ(QStringLiteral("connected"), pLabel->property("crateState").toString());
    EXPECT_QSTRING_EQ(QStringLiteral("connected"), pDot->property("crateState").toString());
}

TEST_F(CrateSkinTest, PreparedSkinSetsTheLayoutClass) {
    const ConfigKey narrowKey(QStringLiteral("[CrateDecks]"), QStringLiteral("layout_narrow"));
    const ConfigKey shortKey(QStringLiteral("[CrateDecks]"), QStringLiteral("layout_short"));
    ControlObject narrow(narrowKey);
    ControlObject isShort(shortKey);
    QWidget skin;
    skin.resize(1920, 1080);
    skin.show();
    crate::prepareLoadedSkin(&skin, skinPath(kCrateDecks));
    EXPECT_EQ(0.0, narrow.get());
    EXPECT_EQ(0.0, isShort.get());
    skin.resize(1366, 768);
    QCoreApplication::processEvents();
    EXPECT_EQ(1.0, narrow.get());
    EXPECT_EQ(1.0, isShort.get());
}

// TEST_GAP: whether the skin looks like the approved GU1 mockups is checked
// by eye. CrateSkinScreenshots renders it offscreen for that comparison.

/// main.cpp shares the OpenGL contexts of all windows, which the waveforms
/// need, before it creates the application; mixxx-test does not. The
/// screenshot harness sets it before main() creates the test application.
[[maybe_unused]] const bool kShareOpenGlContexts = [] {
    if (!qEnvironmentVariableIsEmpty("CRATE_SKIN_SCREENSHOTS")) {
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    }
    return true;
}();

/// QWidget::grab() leaves out native child windows, and with OpenGL the
/// waveforms are QOpenGLWindows in window containers: paints each one's
/// framebuffer over its widget.
void paintOpenGlWindows(QWidget* pRoot, QImage* pImage) {
#ifdef MIXXX_USE_QOPENGL
    QPainter painter(pImage);
    // WGLWidget has no Q_OBJECT, so findChildren cannot look it up
    const QList<QWidget*> widgets = pRoot->findChildren<QWidget*>();
    for (QWidget* pChild : widgets) {
        auto* pWidget = dynamic_cast<WGLWidget*>(pChild);
        if (!pWidget) {
            continue;
        }
        QOpenGLWindow* pWindow = pWidget->getOpenGLWindow();
        if (!pWidget->isVisibleTo(pRoot) || !pWindow || !pWindow->isExposed()) {
            continue;
        }
        const QImage frame = pWindow->grabFramebuffer();
        if (!frame.isNull()) {
            painter.drawImage(QRect(pWidget->mapTo(pRoot, QPoint(0, 0)), pWidget->size()), frame);
        }
    }
#else
    Q_UNUSED(pRoot);
    Q_UNUSED(pImage);
#endif
}

/// The top-level widget that inherits `className`, or nullptr.
QWidget* topLevelOf(const char* className) {
    const QList<QWidget*> windows = QApplication::topLevelWidgets();
    for (QWidget* pWindow : windows) {
        if (pWindow->inherits(className)) {
            return pWindow;
        }
    }
    return nullptr;
}

/// `pWidget` over `background`, the main window as grabbed, at `at` or
/// centred, as the screen shows it where it opens: with the scrim between
/// them while a blocking dialog dims the main window. grab() sees one
/// window only, so the harness composes the three.
QImage overMainWindow(const QImage& background, QWidget* pWidget, QPoint at = QPoint(-1, -1)) {
    QImage over = background.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&over);
    const crate::DialogScrim* pScrim = crate::blockingDialogScrim();
    if (pScrim && pScrim->isActive()) {
        painter.fillRect(over.rect(), pScrim->colour());
    }
    const QImage widget = pWidget->grab().toImage();
    if (at.x() < 0) {
        at = QPoint((over.width() - widget.width()) / 2, (over.height() - widget.height()) / 2);
    }
    painter.drawImage(at, widget);
    return over;
}

/// Dev-only harness: renders the real main window to PNG at 1920 x 1080 and
/// 1366 x 768, with 2 and 4 decks, for the side-by-side comparison with the
/// GU1 mockups; then deck A with a track without a key, and over the decks
/// the first start and a confirmation with the scrim and Preferences
/// without it. Runs only with CRATE_SKIN_SCREENSHOTS=<output dir>;
/// CRATE_SKIN_TRACKS=<dir> loads the first audio files of that directory into
/// decks A, B and C, CRATE_SKIN_LIBRARY_ROWS=<file> fills the library
/// with the tracks of a tab-separated file (readLibraryRows()) and
/// CRATE_SKIN_CRATE_LIBRARY=<file> adds a Crate server with a synced library
/// (insertCrateLibrary()). Never point
/// it at audio that belongs in the repository.
/// Offscreen there is no OpenGL and the scrolling waveforms stay empty;
/// tools/crate-design/skin-screenshots.sh runs it with OpenGL in a headless
/// Wayland compositor.
class CrateSkinScreenshots : public MixxxTest {};

TEST_F(CrateSkinScreenshots, Capture) {
    const QString outDir = qEnvironmentVariable("CRATE_SKIN_SCREENSHOTS");
    if (outDir.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_SKIN_SCREENSHOTS=<dir> to render the skin";
    }
    QDir().mkpath(outDir);
    const QString skin = qEnvironmentVariable("CRATE_SKIN_NAME", kCrateDecks);
    QStringList tracks;
    const QString tracksDir = qEnvironmentVariable("CRATE_SKIN_TRACKS");
    if (!tracksDir.isEmpty()) {
        const QFileInfoList files = QDir(tracksDir).entryInfoList(
                {QStringLiteral("*.flac"),
                        QStringLiteral("*.mp3"),
                        QStringLiteral("*.mp4"),
                        QStringLiteral("*.m4a"),
                        QStringLiteral("*.wav")},
                QDir::Files,
                QDir::Name);
        for (const QFileInfo& file : files) {
            tracks.append(file.absoluteFilePath());
        }
    }

    // The "filtered" waveform and overview of the mockups; with OpenGL, its
    // all-shader version (WaveformWidgetBackend::AllShader)
    const bool openGl = QGuiApplication::platformName() != QLatin1String("offscreen");
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    // A first start rescans the library when the file types it knows
    // change, and marks the rows without a file as missing: give the
    // profile the current types, as a start would have saved them
    SoundSourceProxy::registerProviders();
    decks.writeProfile(skin,
            {
                    {ConfigKey("[Library]", "SupportedFileExtensions"),
                            SoundSourceProxy::getSupportedFileSuffixes().join(u',')},
                    {ConfigKey("[Waveform]", "WaveformType"), QStringLiteral("7")},
                    {ConfigKey("[Waveform]", "use_hardware_acceleration"),
                            openGl ? QStringLiteral("3") : QStringLiteral("0")},
                    {ConfigKey("[Waveform]", "WaveformOverviewType"), QStringLiteral("0")},
                    {ConfigKey("[Waveform]", "beatGridAlpha"), QStringLiteral("20")},
                    // Time remaining and Lancelot keys ("8A"), as in the
                    // mockups
                    {ConfigKey("[Controls]", "PositionDisplay"), QStringLiteral("1")},
                    {ConfigKey("[Key]", "KeyNotation"), QStringLiteral("Lancelot")},
                    {ConfigKey("[Config]", "hide_menubar"), QStringLiteral("1")},
            });
    const QString libraryRows = qEnvironmentVariable("CRATE_SKIN_LIBRARY_ROWS");
    if (!libraryRows.isEmpty()) {
        const QList<LibraryRow> rows = readLibraryRows(libraryRows);
        EXPECT_FALSE(rows.isEmpty()) << libraryRows.toStdString();
        EXPECT_TRUE(insertLibraryRows(profile.path() + QLatin1Char('/'), rows));
    }
    const QString crateLibrary = qEnvironmentVariable("CRATE_SKIN_CRATE_LIBRARY");
    if (!crateLibrary.isEmpty()) {
        EXPECT_TRUE(insertCrateLibrary(profile.path() + QLatin1Char('/'), crateLibrary));
    }
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    // The skin alone fills the window, as with the menu bar hidden
    decks.mainWindow()->menuBar()->hide();

    if (auto* pStatus = decks.skin()->findChild<crate::ServerStatus*>()) {
        pStatus->showStatus(QStringLiteral("dev.crate.local"),
                crate::ServerStatus::State::Connected);
    }

    // CRATE_SKIN_CRATE_OPEN=<label> opens that item of the sidebar, as a
    // click does: a Crate playlist instead of the Crate library
    const QString crateOpen = qEnvironmentVariable("CRATE_SKIN_CRATE_OPEN");
    if (!crateOpen.isEmpty()) {
        for (WLibrarySidebar* pSidebar : decks.skin()->findChildren<WLibrarySidebar*>()) {
            auto* pModel = qobject_cast<SidebarModel*>(pSidebar->model());
            if (!pModel) {
                continue;
            }
            QList<QModelIndex> pending = {QModelIndex()};
            QModelIndex found;
            while (!pending.isEmpty() && !found.isValid()) {
                const QModelIndex parent = pending.takeFirst();
                for (int row = 0; row < pModel->rowCount(parent); ++row) {
                    const QModelIndex index = pModel->index(row, 0, parent);
                    if (index.data().toString().startsWith(crateOpen)) {
                        found = index;
                        break;
                    }
                    pending.append(index);
                }
            }
            EXPECT_TRUE(found.isValid()) << crateOpen.toStdString();
            if (found.isValid()) {
                pSidebar->selectIndex(found, false);
                pModel->clicked(found);
                waitFor([] { return false; }, 300);
            }
        }
    }

    // A playing and leading, B cued near its start, C in the middle, D empty
    const QList<std::pair<QString, double>> positions = {
            {QStringLiteral("[Channel1]"), 0.41},
            {QStringLiteral("[Channel2]"), 0.06},
            {QStringLiteral("[Channel3]"), 0.18},
    };
    auto pPlayerManager = decks.coreServices()->getPlayerManager();
    for (int i = 0; i < positions.size() && i < tracks.size(); ++i) {
        pPlayerManager->slotLoadLocationToPlayer(tracks[i], positions[i].first, false);
    }
    for (int i = 0; i < positions.size() && i < tracks.size(); ++i) {
        const QString group = positions[i].first;
        EXPECT_TRUE(waitFor(
                [&group] {
                    return ControlObject::get(ConfigKey(group, "track_loaded")) > 0;
                },
                30000))
                << group.toStdString();
        BaseTrackPlayer* pPlayer = pPlayerManager->getPlayer(group);
        // The waveform of the overview comes from the analysis on load
        waitFor(
                [pPlayer] {
                    TrackPointer pTrack = pPlayer->getLoadedTrack();
                    if (!pTrack) {
                        return true;
                    }
                    ConstWaveformPointer pWaveform = pTrack->getWaveformSummary();
                    return pWaveform && pWaveform->getCompletion() >= pWaveform->getDataSize();
                },
                60000);
        // A few hotcues, as in the mockups; without an audio device the
        // engine does not run, so they go straight to the track
        if (TrackPointer pTrack = pPlayer->getLoadedTrack()) {
            const double samples = pTrack->getDuration() * pTrack->getSampleRate().value() * 2;
            for (int hotcue = 0; hotcue < 4; hotcue += (i == 1 ? 2 : 1)) {
                pTrack->createAndAddCue(mixxx::CueType::HotCue,
                        hotcue,
                        samples * (0.02 + 0.16 * hotcue),
                        -1.0);
            }
        }
        ControlObject::set(ConfigKey(group, "playposition"), positions[i].second);
        // Without an audio device no engine callback tells the scrolling
        // waveforms where the deck is: give them the position, paused (no
        // audio buffer, so no interpolation). Offscreen there is no OpenGL,
        // and without it this Mixxx offers no waveform type at all
        // (WaveformWidgetFactory::addHandle in a build with QML): the
        // scrolling waveforms stay empty there.
        auto pVisual = VisualPlayPosition::getVisualPlayPosition(group);
        for (int buffer = 0; buffer < 3; ++buffer) {
            pVisual->set(positions[i].second,
                    0.0,
                    0.0,
                    positions[i].second,
                    0.0,
                    SlipModeState::Disabled,
                    false,
                    false,
                    false,
                    0.0,
                    0.0,
                    0.0,
                    0.0);
        }
    }
    ControlObject::set(ConfigKey("[Channel1]", "sync_enabled"), 1);
    ControlObject::set(ConfigKey("[Channel2]", "sync_enabled"), 1);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
    ControlObject::set(ConfigKey("[Channel1]", "beatloop_size"), 4);
    ControlObject::set(ConfigKey("[Channel2]", "beatloop_size"), 8);
    ControlObject::set(ConfigKey("[Channel3]", "beatloop_size"), 16);
    // The mixer of the mockups (decks.js channelStrip and mixer): knob and
    // fader positions as parameters, B in the headphones
    const QList<std::pair<ConfigKey, double>> mixer = {
            {ConfigKey("[EqualizerRack1_[Channel1]_Effect1]", "parameter3"), 0.56},
            {ConfigKey("[EqualizerRack1_[Channel1]_Effect1]", "parameter1"), 0.38},
            {ConfigKey("[EqualizerRack1_[Channel2]_Effect1]", "parameter1"), 0.2},
            {ConfigKey("[Channel1]", "volume"), 0.86},
            {ConfigKey("[Channel2]", "volume"), 0.18},
            {ConfigKey("[Channel3]", "volume"), 0},
            {ConfigKey("[Channel4]", "volume"), 0},
            {ConfigKey("[Channel2]", "pfl"), 1},
            {ConfigKey("[Master]", "gain"), 0.62},
            {ConfigKey("[Master]", "headMix"), 0.3},
            {ConfigKey("[Master]", "headGain"), 0.55},
            {ConfigKey("[Master]", "crossfader"), 0.5},
    };
    for (const auto& [key, parameter] : mixer) {
        if (ControlObject* pControl = ControlObject::getControl(key)) {
            pControl->setParameter(parameter);
        }
    }

    // The library as the mockups show it: sorted by BPM, the third row
    // selected and the seventh under the mouse
    waitFor([] { return false; }, 500);
    for (WTrackTableView* pView : decks.skin()->findChildren<WTrackTableView*>()) {
        if (!pView->isVisible() || !pView->model() || pView->model()->rowCount() == 0) {
            continue;
        }
        for (int column = 0; column < pView->model()->columnCount(); ++column) {
            if (pView->model()->headerData(column, Qt::Horizontal, TrackModel::kHeaderNameRole) ==
                    QStringLiteral("bpm")) {
                pView->sortByColumn(column, Qt::AscendingOrder);
            }
        }
        pView->selectRow(std::min(2, pView->model()->rowCount() - 1));
        waitFor([] { return false; }, 200);
        const int hovered = std::min(6, pView->model()->rowCount() - 1);
        const QPoint at(pView->viewport()->width() / 2,
                pView->rowViewportPosition(hovered) + pView->rowHeight(hovered) / 2);
        QMouseEvent move(QEvent::MouseMove,
                at,
                pView->viewport()->mapToGlobal(at),
                Qt::NoButton,
                Qt::NoButton,
                Qt::NoModifier);
        QCoreApplication::sendEvent(pView->viewport(), &move);
    }

    // CRATE_SKIN_GEOMETRY=1 also lists the rectangles of the skin's own
    // widgets next to each PNG, to measure them against the mockups
    const bool geometry = !qEnvironmentVariableIsEmpty("CRATE_SKIN_GEOMETRY");
    const auto capture = [&](const QString& name) {
        // The key the decks show follows the track's on each waveform tick
        // with the engine running (DeckVisuals::process); without an audio
        // device the harness does it
        for (int deck = 1; deck <= 4; ++deck) {
            const QString group = QStringLiteral("[Channel%1]").arg(deck);
            ControlObject::set(ConfigKey(group, QStringLiteral("visual_key")),
                    ControlObject::get(ConfigKey(group, QStringLiteral("key"))));
        }
        // Layouts, deferred restyles and the waveforms' timers
        waitFor([] { return false; }, 1500);
        QImage image = decks.skin()->grab().toImage();
        if (openGl) {
            paintOpenGlWindows(decks.skin(), &image);
        }
        const QString file = QDir(outDir).filePath(name + QStringLiteral(".png"));
        EXPECT_TRUE(image.save(file)) << file.toStdString();
        if (!geometry) {
            return image;
        }
        QFile rects(QDir(outDir).filePath(name + QStringLiteral(".geometry.txt")));
        if (!rects.open(QIODevice::WriteOnly)) {
            return image;
        }
        const QList<QWidget*> widgets = decks.skin()->findChildren<QWidget*>();
        for (QWidget* pWidget : widgets) {
            const QString objectName = pWidget->objectName();
            if (pWidget->isVisible() &&
                    (objectName.startsWith(QLatin1String("Crate")) ||
                            objectName.startsWith(QLatin1String("Mono")) ||
                            objectName.startsWith(QLatin1String("DeckNumber")))) {
                const QPoint at = pWidget->mapTo(decks.skin(), QPoint(0, 0));
                // name, x, y, width, height, then the size hint
                rects.write(QStringLiteral("%1 %2 %3 %4 %5 %6 %7\n")
                                .arg(objectName)
                                .arg(at.x())
                                .arg(at.y())
                                .arg(pWidget->width())
                                .arg(pWidget->height())
                                .arg(pWidget->sizeHint().width())
                                .arg(pWidget->sizeHint().height())
                                .toUtf8());
            }
        }
        return image;
    };
    // The rows that wait for their data (phrase rule, Smart Mix line,
    // Crate's track state) are hidden: the "-preview" shots show them laid
    // out, empty, to compare with the mockups
    const QList<QSize> sizes = {QSize(1920, 1080), QSize(1366, 768)};
    for (const bool preview : {false, true}) {
        for (const char* placeholder :
                {"show_phrase_rule", "show_smartmix_line", "show_track_state"}) {
            ControlObject::set(ConfigKey("[CrateDecks]", placeholder), preview ? 1 : 0);
        }
        for (const int deckCount : {2, 4}) {
            ControlObject::set(ConfigKey("[Skin]", "show_4decks"), deckCount == 4 ? 1 : 0);
            for (const QSize& size : sizes) {
                decks.mainWindow()->resize(size);
                capture(QStringLiteral("%1-%2deck-%3%4")
                                .arg(skin.toLower(), QString::number(deckCount))
                                .arg(size.width())
                                .arg(preview ? QStringLiteral("-preview") : QString()));
            }
        }
    }
    // The maximised library, with two and four decks
    for (const char* placeholder :
            {"show_phrase_rule", "show_smartmix_line", "show_track_state"}) {
        ControlObject::set(ConfigKey("[CrateDecks]", placeholder), 0);
    }
    ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 1);
    for (const int deckCount : {2, 4}) {
        ControlObject::set(ConfigKey("[Skin]", "show_4decks"), deckCount == 4 ? 1 : 0);
        for (const QSize& size : sizes) {
            decks.mainWindow()->resize(size);
            capture(QStringLiteral("%1-%2deck-%3-maxlib")
                            .arg(skin.toLower(), QString::number(deckCount))
                            .arg(size.width()));
        }
    }
    ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 0);

    // The deck menu of deck A, open
    ControlObject::set(ConfigKey("[Skin]", "show_4decks"), 0);
    ControlObject::set(ConfigKey("[CrateDecks]", "deck1_menu"), 1);
    decks.mainWindow()->resize(QSize(1920, 1080));
    capture(skin.toLower() + QStringLiteral("-2deck-1920-menu"));
    ControlObject::set(ConfigKey("[CrateDecks]", "deck1_menu"), 0);

    // Deck A with a track without a key: its header drops the key and its
    // separator ("DECK A · 151.14 BPM")
    if (TrackPointer pTrack =
                    pPlayerManager->getPlayer(QStringLiteral("[Channel1]"))->getLoadedTrack()) {
        pTrack->resetKeys();
    }
    const QImage decksImage = capture(skin.toLower() + QStringLiteral("-2deck-1920-nokey"));

    // Over the decks, waveforms included: the blocking dialogs dim them,
    // Preferences does not (design 14.3)
    decks.answerModalDialogs(false);
    const auto saveOver = [&](QWidget* pWidget, const QString& name) {
        waitFor([] { return false; }, 1200);
        const QString file = QDir(outDir).filePath(name + QStringLiteral(".png"));
        EXPECT_TRUE(overMainWindow(decksImage, pWidget).save(file)) << file.toStdString();
    };
    const auto dimmed = [] {
        const crate::DialogScrim* pScrim = crate::blockingDialogScrim();
        return pScrim && pScrim->isActive();
    };
    if (auto* pStatus = decks.skin()->findChild<crate::ServerStatus*>()) {
        pStatus->showStatus(QString(), crate::ServerStatus::State::None);
    }
    std::unique_ptr<QDialog> pFirstRun(
            crate::createFirstRunDialog(crate::CrateServers::instance(), decks.mainWindow()));
    pFirstRun->open();
    EXPECT_TRUE(dimmed());
    if (auto* pAddress = pFirstRun->findChild<QLineEdit*>(QStringLiteral("crateServerAddress"))) {
        pAddress->setText(QStringLiteral("dev.crate.local"));
        pAddress->setFocus();
    }
    saveOver(pFirstRun.get(), QStringLiteral("crate-scrim-firstrun-over"));
    pFirstRun->reject();
    EXPECT_FALSE(dimmed());
    // As Mixxx confirms the exit while a deck plays
    QMessageBox confirm(QMessageBox::Question,
            QStringLiteral("Confirm Exit"),
            QStringLiteral("A deck is currently playing. Exit Mixxx?"),
            QMessageBox::Yes | QMessageBox::No,
            decks.mainWindow());
    confirm.open();
    EXPECT_TRUE(dimmed());
    saveOver(&confirm, QStringLiteral("crate-scrim-confirm-over"));
    confirm.done(QMessageBox::No);
    ControlObject::set(crate::showPreferencesKey(), 1);
    if (QWidget* pPreferences = topLevelOf("DlgPreferences")) {
        pPreferences->resize(QSize(1180, 940));
        EXPECT_FALSE(dimmed());
        saveOver(pPreferences, QStringLiteral("crate-noscrim-prefs-over"));
        pPreferences->hide();
    }
    decks.answerModalDialogs(true);
}

/// The last menu that showed. In a Wayland compositor a popup opened
/// without a real click may close at once or not show; the harness renders
/// the menu all the same.
class LastMenu : public QObject {
  public:
    QPointer<QMenu> menu;

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() == QEvent::Show) {
            if (auto* pMenu = qobject_cast<QMenu*>(pWatched)) {
                menu = pMenu;
            }
        }
        return QObject::eventFilter(pWatched, pEvent);
    }
};

/// Dev-only harness: renders the dialogs of plan V2.PR5 to PNG for the side
/// by side with the GU1 mockups (preferences-crate, first-run): Preferences
/// on the Crate page and two of Mixxx's pages, About, the first-start
/// dialog, the splash screen and the track and column menus. Each one is
/// saved alone and over the main window, where it opens. Runs only with
/// CRATE_SKIN_SCREENSHOTS=<output dir>, like Capture; the Crate page shows
/// the mockup's two servers and its cache and Smart Mix sections, which wait
/// for plans F4 and F6, laid out.
TEST_F(CrateSkinScreenshots, Dialogs) {
    const QString outDir = qEnvironmentVariable("CRATE_SKIN_SCREENSHOTS");
    if (outDir.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_SKIN_SCREENSHOTS=<dir> to render the dialogs";
    }
    QDir().mkpath(outDir);
    const bool openGl = QGuiApplication::platformName() != QLatin1String("offscreen");
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    SoundSourceProxy::registerProviders();
    decks.writeProfile(kCrateDecks,
            {
                    {ConfigKey("[Library]", "SupportedFileExtensions"),
                            SoundSourceProxy::getSupportedFileSuffixes().join(u',')},
                    {ConfigKey("[Waveform]", "WaveformType"), QStringLiteral("7")},
                    {ConfigKey("[Waveform]", "use_hardware_acceleration"),
                            openGl ? QStringLiteral("3") : QStringLiteral("0")},
                    {ConfigKey("[Config]", "hide_menubar"), QStringLiteral("1")},
            });
    const QString libraryRows = qEnvironmentVariable("CRATE_SKIN_LIBRARY_ROWS");
    if (!libraryRows.isEmpty()) {
        EXPECT_TRUE(insertLibraryRows(
                profile.path() + QLatin1Char('/'), readLibraryRows(libraryRows)));
    }
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->menuBar()->hide();
    decks.mainWindow()->resize(QSize(1920, 1080));
    auto* pStatus = decks.skin()->findChild<crate::ServerStatus*>();

    const auto settle = [] {
        waitFor([] { return false; }, 1200);
    };
    const auto mainWindow = [&] {
        settle();
        QImage image = decks.skin()->grab().toImage();
        if (openGl) {
            paintOpenGlWindows(decks.skin(), &image);
        }
        return image;
    };
    const auto save = [&](const QImage& image, const QString& name) {
        const QString file = QDir(outDir).filePath(name + QStringLiteral(".png"));
        EXPECT_TRUE(image.save(file)) << file.toStdString();
    };
    // The dialog alone and over the main window, centred, as it opens
    const auto saveOver = [&](QWidget* pWidget,
                                  const QImage& background,
                                  const QString& name,
                                  QPoint at = QPoint(-1, -1)) {
        settle();
        save(pWidget->grab().toImage(), name);
        save(overMainWindow(background, pWidget, at), name + QStringLiteral("-over"));
    };

    // Preferences, from the toolbar's button, at the mockup's size
    if (pStatus) {
        pStatus->showStatus(QStringLiteral("dev.crate.local"),
                crate::ServerStatus::State::Connected);
    }
    const QImage withServer = mainWindow();
    ControlObject::set(crate::showPreferencesKey(), 1);
    QWidget* pPreferences = topLevelOf("DlgPreferences");
    ASSERT_NE(nullptr, pPreferences);
    ASSERT_TRUE(pPreferences->isVisible());
    pPreferences->resize(QSize(1180, 940));
    auto* pTree = pPreferences->findChild<QTreeWidget*>(QStringLiteral("contentsTreeWidget"));
    ASSERT_NE(nullptr, pTree);
    QTreeWidgetItem* pCrateItem = pTree->topLevelItem(pTree->topLevelItemCount() - 1);
    pTree->setCurrentItem(pCrateItem);
    auto* pCratePage = pPreferences->findChild<crate::DlgPrefCrate*>();
    ASSERT_NE(nullptr, pCratePage);
    const QStringList features = {QStringLiteral("library"),
            QStringLiteral("analysis"),
            QStringLiteral("beatmatch"),
            QStringLiteral("bass handoff"),
            QStringLiteral("long beatmatch"),
            QStringLiteral("cues")};
    crate::ServerRow dev;
    dev.name = QStringLiteral("dev.crate.local");
    dev.meta = {QStringLiteral("https://dev.crate.local"),
            QStringLiteral("signed in as diego@crate.local"),
            QStringLiteral("136 512 tracks")};
    dev.state = crate::ServerRow::State::SignedIn;
    crate::ServerRow home;
    home.name = QStringLiteral("home.crate.lan");
    home.meta = {QStringLiteral("http://192.168.1.20:8585"),
            QStringLiteral("local network, no HTTPS"),
            QStringLiteral("last seen 2 h ago")};
    home.state = crate::ServerRow::State::SignedOut;
    for (const QString& feature : features) {
        dev.features.append({feature, true});
        home.features.append({feature,
                !feature.contains(QLatin1String("long")) &&
                        feature != QLatin1String("cues")});
    }
    pCratePage->showServerRows({dev, home});
    pCratePage->cacheSection()->setUsage(18'400'000'000, 40'000'000'000, 1204, 31);
    pCratePage->cacheSection()->setLocation(QStringLiteral("~/.cratedecks/cache"));
    pCratePage->cacheSection()->prewarm()->setChecked(true);
    pCratePage->cacheSection()->show();
    pCratePage->smartMixSection()->show();
    saveOver(pPreferences, withServer, QStringLiteral("crate-prefs-crate"));
    for (int i = 0; i < pTree->topLevelItemCount(); ++i) {
        const QString page = pTree->topLevelItem(i)->text(0);
        if (page == QLatin1String("Library") || page == QLatin1String("Decks") ||
                page == QLatin1String("Sound Hardware")) {
            pTree->setCurrentItem(pTree->topLevelItem(i));
            saveOver(pPreferences,
                    withServer,
                    QStringLiteral("crate-prefs-") + page.toLower().replace(u' ', u'-'));
        }
    }
    pPreferences->hide();

    // About
    DlgAbout about;
    about.show();
    saveOver(&about, withServer, QStringLiteral("crate-about"));
    about.hide();

    // The first start: no server, the address typed in the focused field
    if (pStatus) {
        pStatus->showStatus(QString(), crate::ServerStatus::State::None);
    }
    const QImage withoutServer = mainWindow();
    // Modal, as main.cpp opens it: it dims the main window
    std::unique_ptr<QDialog> pFirstRun(
            crate::createFirstRunDialog(crate::CrateServers::instance(), decks.mainWindow()));
    decks.answerModalDialogs(false);
    pFirstRun->open();
    if (auto* pAddress = pFirstRun->findChild<QLineEdit*>(QStringLiteral("crateServerAddress"))) {
        pAddress->setText(QStringLiteral("dev.crate.local"));
        pAddress->setFocus();
    }
    saveOver(pFirstRun.get(), withoutServer, QStringLiteral("crate-firstrun"));
    pFirstRun->reject();
    decks.answerModalDialogs(true);

    // The splash screen, as the main window shows it while it starts
    {
        mixxx::skin::SkinLoader loader(decks.coreServices()->getSettings());
        std::unique_ptr<LaunchImage> pSplash(loader.loadLaunchImage(nullptr));
        ASSERT_TRUE(pSplash);
        pSplash->resize(QSize(1920, 1080));
        pSplash->progress(42, QString());
        pSplash->show();
        settle();
        save(pSplash->grab().toImage(), QStringLiteral("crate-splash"));
    }

    // The track menu and the column chooser of the library
    if (pStatus) {
        pStatus->showStatus(QStringLiteral("dev.crate.local"),
                crate::ServerStatus::State::Connected);
    }
    const QImage library = mainWindow();
    LastMenu lastMenu;
    application()->installEventFilter(&lastMenu);
    for (WTrackTableView* pView : decks.skin()->findChildren<WTrackTableView*>()) {
        if (!pView->isVisible() || !pView->model() || pView->model()->rowCount() == 0) {
            continue;
        }
        const QList<std::pair<QWidget*, QString>> targets = {
                {pView->viewport(), QStringLiteral("crate-menu-track")},
                {pView->horizontalHeader()->viewport(), QStringLiteral("crate-menu-columns")},
        };
        for (const auto& [pTarget, name] : targets) {
            const QPoint at(pTarget->width() / 3, std::min(pTarget->height() / 2, 40));
            if (pTarget == pView->viewport()) {
                pView->selectRow(2);
            }
            lastMenu.menu = nullptr;
            QContextMenuEvent event(QContextMenuEvent::Mouse, at, pTarget->mapToGlobal(at));
            QCoreApplication::sendEvent(pTarget, &event);
            settle();
            // The track menu belongs to the table, the column chooser to its
            // header
            QMenu* pMenu = lastMenu.menu ? lastMenu.menu.data()
                                         : pTarget->parentWidget()->findChild<QMenu*>();
            EXPECT_NE(nullptr, pMenu) << name.toStdString();
            if (pMenu) {
                pMenu->adjustSize();
                saveOver(pMenu, library, name, pTarget->mapTo(decks.skin(), at));
                pMenu->hide();
            }
        }
        break;
    }
    application()->removeEventFilter(&lastMenu);

    // The Auto DJ and Analyze views of the library, with their button bars
    if (auto* pLibraryWidget = decks.skin()->findChild<WLibrary*>()) {
        for (const auto& [view, name] :
                {std::pair{Library::kAutoDJViewName, QStringLiteral("crate-library-autodj")},
                        std::pair{QStringLiteral("Analysis"),
                                QStringLiteral("crate-library-analyze")}}) {
            pLibraryWidget->switchToView(view);
            save(mainWindow(), name);
        }
    }
}

/// Dev-only harness for Q14 (plan V2, performance): skin load time and
/// scrolling 10,000 library rows, for CrateDecks and LateNight. Runs only
/// with CRATE_SKIN_Q14=1 and prints its numbers.
class CrateSkinPerformance : public MixxxTest {
  protected:
    struct Numbers {
        qint64 loadMs = 0;
        qint64 skinMs = 0;
        double meanFrameMs = 0;
        double p95FrameMs = 0;
        int rows = 0;
    };

    static bool addLibraryRows(const QString& settingsPath, int count) {
        QList<LibraryRow> rows;
        rows.reserve(count);
        for (int i = 0; i < count; ++i) {
            rows.append({QStringLiteral("Title of track %1").arg(i),
                    QStringLiteral("Artist %1").arg(i % 97),
                    QStringLiteral("Album %1").arg(i % 31),
                    QStringLiteral("Genre %1").arg(i % 12),
                    QString(),
                    118.0 + (i % 140) / 10.0,
                    300.0 + i % 200});
        }
        return insertLibraryRows(settingsPath, rows);
    }

    Numbers measure(const QString& skin, int rows) {
        Numbers numbers;
        QTemporaryDir profile;
        EXPECT_TRUE(profile.isValid());
        const QString settingsPath = profile.path() + QLatin1Char('/');
        {
            // A first start creates the library schema
            OffscreenDecks decks(settingsPath);
            decks.writeProfile(skin);
            decks.start(application());
        }
        EXPECT_TRUE(addLibraryRows(settingsPath, rows));
        OffscreenDecks decks(settingsPath);
        {
            std::unique_ptr<SkinLoadClock> pClock;
            numbers.loadMs = decks.start(application(), [&pClock] {
                pClock = std::make_unique<SkinLoadClock>();
            });
            numbers.skinMs = pClock->elapsedMs();
        }
        decks.mainWindow()->resize(1920, 1080);
        waitFor([] { return false; }, 1000);
        WTrackTableView* pView = nullptr;
        waitFor(
                [&decks, &pView] {
                    const auto views = decks.skin()->findChildren<WTrackTableView*>();
                    for (WTrackTableView* pCandidate : views) {
                        if (pCandidate->isVisible() && pCandidate->model() &&
                                pCandidate->model()->rowCount() > 0) {
                            pView = pCandidate;
                            return true;
                        }
                    }
                    return false;
                },
                30000);
        if (!pView) {
            ADD_FAILURE() << "No library view with rows";
            return numbers;
        }
        while (pView->model()->canFetchMore(QModelIndex())) {
            pView->model()->fetchMore(QModelIndex());
        }
        numbers.rows = pView->model()->rowCount();
        QScrollBar* pBar = pView->verticalScrollBar();
        std::vector<double> frames;
        QElapsedTimer timer;
        // Scroll the whole list by small steps, as a wheel does, painting
        // every step as the screen would
        for (int value = 0; value <= pBar->maximum(); value += 3 * pBar->singleStep()) {
            timer.start();
            pBar->setValue(value);
            pView->viewport()->repaint();
            frames.push_back(timer.nsecsElapsed() / 1e6);
        }
        if (!frames.empty()) {
            double sum = 0;
            for (double frame : frames) {
                sum += frame;
            }
            numbers.meanFrameMs = sum / frames.size();
            std::sort(frames.begin(), frames.end());
            numbers.p95FrameMs = frames[static_cast<size_t>(frames.size() * 0.95)];
        }
        return numbers;
    }
};

TEST_F(CrateSkinPerformance, Q14) {
    if (qEnvironmentVariable("CRATE_SKIN_Q14") != QLatin1String("1")) {
        GTEST_SKIP() << "Set CRATE_SKIN_Q14=1 to measure skin load and library scrolling";
    }
    constexpr int kRows = 10000;
    for (const QString& skin : {kLateNight, kCrateDecks}) {
        const Numbers numbers = measure(skin, kRows);
        // Printed, not logged: the test log level hides info messages
        std::cout << qPrintable(QStringLiteral(
                             "Q14 %1: skin %2 ms (main window with skin %3 ms); "
                             "scrolling %4 rows: %5 ms per frame on average, %6 ms at p95")
                                     .arg(skin)
                                     .arg(numbers.skinMs)
                                     .arg(numbers.loadMs)
                                     .arg(numbers.rows)
                                     .arg(numbers.meanFrameMs, 0, 'f', 2)
                                     .arg(numbers.p95FrameMs, 0, 'f', 2))
                  << std::endl;
        EXPECT_EQ(kRows, numbers.rows);
    }
}

} // namespace
