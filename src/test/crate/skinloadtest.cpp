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
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>

#include "config.h"
#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "coreservices.h"
#include "crate/appidentity.h"
#include "crate/brand/brandtext.h"
#include "crate/cache/audiocache.h"
#include "crate/crateserverrows.h"
#include "crate/crateservers.h"
#include "crate/db/crateserverprofiles.h"
#include "crate/db/crateserverschema.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarystore.h"
#include "crate/recs/compatiblesmodel.h"
#include "crate/recs/compatiblesview.h"
#include "crate/recs/cratepathsource.h"
#include "crate/recs/recsjson.h"
#include "crate/smartmix/assistpanel.h"
#include "crate/smartmix/planclient.h"
#include "crate/smartmix/smartmixassist.h"
#include "crate/system/systemcheck.h"
#include "crate/ui/cachestatusmodel.h"
#include "crate/ui/cachestatusview.h"
#include "crate/ui/crateaddserverwidget.h"
#include "crate/ui/cratefirstrun.h"
#include "crate/ui/crateprefsections.h"
#include "crate/ui/delegates/sidebardelegate.h"
#include "crate/ui/dialogs/cratedialog.h"
#include "crate/ui/dialogs/dialogscrim.h"
#include "crate/ui/dialogs/preferenceschrome.h"
#include "crate/ui/dlgprefcrate.h"
#include "crate/ui/skin/crateserverstatus.h"
#include "crate/ui/skin/crateskin.h"
#include "crate/ui/skin/mixerstyle.h"
#include "crate/ui/skin/preferencescontrol.h"
#include "crate/ui/widgets/camelotwheel.h"
#include "crate/ui/widgets/transitionplanview.h"
#include "database/mixxxdb.h"
#include "dialog/dlgabout.h"
#include "library/crateserver/crateserverfeature.h"
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
#include "test/crate/fakecratenetwork.h"
#include "test/crate/offscreendecks.h"
#include "test/mixxxtest.h"
#include "track/cue.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/font.h"
#include "util/logging.h"
#include "util/versionstore.h"
#include "waveform/visualplayposition.h"
#include "waveform/waveform.h"
#include "widget/wglwidget.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wtracktableview.h"
#include "widget/wwidgetgroup.h"
#include "widget/wwidgetstack.h"

#ifdef MIXXX_USE_QOPENGL
#include <QOpenGLWindow>
#endif

namespace {

using crate::test::OffscreenDecks;
using crate::test::waitFor;

const QString kCrateDecks = QStringLiteral("CrateDecks");
const QString kLateNight = QStringLiteral("LateNight");

QString skinPath(const QString& skin) {
    return QDir(ConfigObject<ConfigValue>::computeResourcePath())
            .filePath(QStringLiteral("skins/") + skin);
}

/// Keeps the warnings that Qt and the inherited code log while it is alive, counts every
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
        db.setDatabaseName(settingsPath + QString::fromUtf8(crate::kDatabaseFileName));
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
        db.setDatabaseName(settingsPath + QString::fromUtf8(crate::kDatabaseFileName));
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
        const auto containers = crate::library::loadContainers(db, server);
        for (const auto& container : containers) {
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

/// Links `skin`, if it is one of upstream's, into the skins folder of the
/// profile at `settingsPath`: the only place it loads from (plan BR2).
void linkUpstreamSkin(const QString& settingsPath, const QString& skin) {
    if (skin == kCrateDecks) {
        return;
    }
    QDir profile(settingsPath);
    EXPECT_TRUE(profile.mkpath(QStringLiteral("skins")));
    EXPECT_TRUE(QFile::link(skinPath(skin), profile.filePath(QStringLiteral("skins/") + skin)));
}

class CrateSkinLoadTest : public MixxxTest {
  protected:
    /// Loads `skin` and returns its skin warnings. A skin of upstream's is
    /// first linked into the profile's own skins folder, the only place it
    /// loads from (plan BR2).
    QStringList loadSkin(const QString& skin, QString* pLoadedRootName = nullptr) {
        QTemporaryDir profile;
        EXPECT_TRUE(profile.isValid());
        linkUpstreamSkin(profile.path(), skin);
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

// LateNight is not offered nor installed, but the CrateDecks defaults and
// styles must not break the skins a user installs (plan BR2)
TEST_F(CrateSkinLoadTest, LateNightStillLoads) {
    const QStringList warnings = loadSkin(kLateNight);
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
}

/// The titles of the dialogs that show while it is alive.
class ShownDialogTitles : public QObject {
  public:
    QStringList titles;

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() == QEvent::Show) {
            if (auto* pDialog = qobject_cast<QDialog*>(pWatched)) {
                titles.append(pDialog->windowTitle());
            }
        }
        return QObject::eventFilter(pWatched, pEvent);
    }
};

// Plan "Own brand", BR.PR3: on a new profile with no music folder and no
// sound device, upstream asked for a music folder, warned of "No Output
// Devices" and asked whether to hide the menu bar, all before Crate's first
// start. The first start asks the first two in its own pages and the skin
// decides the third.
TEST_F(CrateSkinLoadTest, FirstStartAsksNoMixxxQuestions) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    ShownDialogTitles shown;
    application()->installEventFilter(&shown);
    decks.start(application());
    application()->removeEventFilter(&shown);
    ASSERT_NE(nullptr, decks.skin());
    EXPECT_TRUE(shown.titles.isEmpty()) << qPrintable(shown.titles.join(QStringLiteral(", ")));
    const UserSettingsPointer pConfig = decks.coreServices()->getSettings();
    EXPECT_EQ(1, pConfig->getValue<int>(ConfigKey("[Config]", "hide_menubar"), 0));
    EXPECT_EQ(0, pConfig->getValue<int>(ConfigKey("[Config]", "show_menubar_hint"), 1));
}

// Plan BR2: no file or folder of a new profile carries upstream's name (the
// settings, the library database and the log are named after Decks),
// and the log says Decks where a message of the inherited code says
// upstream's name.
TEST_F(CrateSkinLoadTest, SettingsFolderNamesNoMixxx) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    const QDir profileDir(profile.path());
    {
        OffscreenDecks decks(profile.path() + QLatin1Char('/'));
        decks.writeProfile(kCrateDecks);
        decks.start(application());
        ASSERT_NE(nullptr, decks.skin());
        qWarning() << "Mixxx shutdown complete with code" << 0;
        mixxx::Logging::flushLogFile();
    }
    QStringList named;
    QDirIterator it(profile.path(),
            QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = profileDir.relativeFilePath(it.next());
        if (path.contains(QLatin1String("mixxx"), Qt::CaseInsensitive)) {
            named.append(path);
        }
    }
    EXPECT_TRUE(named.isEmpty()) << qPrintable(named.join(QStringLiteral(", ")));
    EXPECT_TRUE(profileDir.exists(QStringLiteral(MIXXX_SETTINGS_FILE)));
    EXPECT_TRUE(profileDir.exists(QString::fromUtf8(crate::kDatabaseFileName)));
    // Only the first core services of a process open a log file (in their
    // settings folder): ctest runs each test in a process of its own.
    QFile log(profileDir.filePath(QString::fromUtf8(crate::kLogFileName)));
    if (!log.open(QIODevice::ReadOnly | QIODevice::Text)) {
        GTEST_SKIP() << "Another test of this process opened the log file";
    }
    const QString text = QString::fromUtf8(log.readAll());
    EXPECT_TRUE(text.contains(QLatin1String("Decks shutdown complete with code")));
    // Not the name it had before it was called Decks (crate::kDataFolderName)
    EXPECT_FALSE(text.contains(
            QString::fromUtf8(crate::kDataFolderName) + QStringLiteral(" shutdown")));
    QStringList lines;
    const QStringList logLines = text.split(QLatin1Char('\n'));
    for (const QString& line : logLines) {
        if (crate::namesMixxx(line)) {
            lines.append(line);
        }
    }
    EXPECT_TRUE(lines.isEmpty()) << qPrintable(lines.join(QLatin1Char('\n')));
}

// Plan "Own brand", BR.PR5 and BR2: upstream's skins carry the upstream
// logo, so only CrateDecks is offered, and the others do not load by name
// from the system folder either; one in the user's own folder does
TEST_F(CrateSkinLoadTest, OnlyCrateDecksIsOffered) {
    mixxx::skin::SkinLoader loader(config());
    QStringList offered;
    const auto systemSkins = loader.getSystemSkins();
    for (const auto& pSkin : systemSkins) {
        offered.append(pSkin->name());
    }
    EXPECT_EQ(QStringList{kCrateDecks}, offered);
    EXPECT_EQ(nullptr, loader.getSkin(kLateNight));
    EXPECT_NE(nullptr, loader.getSkin(kCrateDecks));
}

// Plan "Own brand", BR.PR6: the sidebar's section of upstream's own library is
// "Local", next to Crate's section (F3), which comes first
TEST_F(CrateSkinLoadTest, SidebarSectionsNameNoMixxx) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    auto* pSidebar = decks.skin()->findChild<WLibrarySidebar*>();
    ASSERT_NE(nullptr, pSidebar);
    auto* pDelegate = qobject_cast<crate::delegates::SidebarDelegate*>(pSidebar->itemDelegate());
    ASSERT_NE(nullptr, pDelegate);
    QStringList headers;
    for (int row = 0; row < pSidebar->model()->rowCount(); ++row) {
        const QString header = pDelegate->sectionHeader(pSidebar->model()->index(row, 0));
        if (!header.isEmpty()) {
            headers.append(header);
        }
    }
    EXPECT_TRUE(headers.contains(QStringLiteral("Local"))) << headers.join(u',').toStdString();
    for (const QString& header : std::as_const(headers)) {
        EXPECT_FALSE(header.contains(QStringLiteral("Mixxx"), Qt::CaseInsensitive))
                << header.toStdString();
    }
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
    const QWidgetList windows = QApplication::topLevelWidgets();
    for (QWidget* pWindow : windows) {
        if (pWindow->inherits("DlgPreferences")) {
            pPreferences = pWindow;
        }
    }
    ASSERT_NE(nullptr, pPreferences);

    // Its own frame and title row (GU1 mockups preferences-crate)
    EXPECT_TRUE(pPreferences->windowFlags().testFlag(Qt::FramelessWindowHint));
    auto* pTitle = pPreferences->findChild<QLabel*>(QStringLiteral("MonoDialogTitle"));
    ASSERT_NE(nullptr, pTitle);
    EXPECT_QSTRING_EQ(QStringLiteral("Decks · Preferences"), pTitle->text());

    // The Crate page last, with the Crate logo
    auto* pTree = pPreferences->findChild<QTreeWidget*>(QStringLiteral("contentsTreeWidget"));
    ASSERT_NE(nullptr, pTree);
    QTreeWidgetItem* pLast = pTree->topLevelItem(pTree->topLevelItemCount() - 1);
    EXPECT_QSTRING_EQ(QStringLiteral("Crate"), pLast->text(0));
    EXPECT_TRUE(pLast->data(0, crate::kPreferencesLogoRole).toBool());

    // The foot in the mockup's order; upstream's own buttons stay, hidden
    auto* pButtons = pPreferences->findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"));
    ASSERT_NE(nullptr, pButtons);
    EXPECT_TRUE(pButtons->isHidden());
    QStringList foot;
    QPushButton* pOk = nullptr;
    const QList<QPushButton*> buttons =
            pPreferences->findChildren<QPushButton*>(QStringLiteral("CratePrefsButton"));
    for (QPushButton* pButton : buttons) {
        foot.append(pButton->text());
        if (pButton->isDefault()) {
            pOk = pButton;
        }
    }
    // The mockup's labels (Diego, 2026-11-19), with upstream's mnemonics
    EXPECT_EQ((QStringList{QStringLiteral("Restore defaults"),
                      QStringLiteral("Help"),
                      QStringLiteral("Cancel"),
                      QStringLiteral("Apply"),
                      QStringLiteral("OK")}),
            foot);
    EXPECT_EQ(QKeySequence::mnemonic(QStringLiteral("&Ok")), pOk->shortcut());
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
    const QWidgetList windows = QApplication::topLevelWidgets();
    for (QWidget* pWindow : windows) {
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
    // Nor when upstream opens it modal, after a sound device fails. Checked
    // before events run: the DialogRejecter closes modal dialogs then
    pPreferences->setWindowModality(Qt::ApplicationModal);
    pPreferences->show();
    EXPECT_FALSE(dimmed());
    pPreferences->hide();
    pPreferences->setWindowModality(Qt::NonModal);

    // A confirmation as upstream's on exit dims the main window until it closes
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

/// Marks the four decks loaded, so that they show their controls instead
/// of the drop zone, and opens the three racks, at 1920 x 1080.
void openRacksOverLoadedDecks(OffscreenDecks* pDecks) {
    for (int deck = 1; deck <= 4; ++deck) {
        // track_loaded is read-only to everyone but the deck
        ControlObject* pLoaded = ControlObject::getControl(ConfigKey(
                QStringLiteral("[Channel%1]").arg(deck), QStringLiteral("track_loaded")));
        ASSERT_NE(nullptr, pLoaded);
        pLoaded->forceSet(1.0);
    }
    for (const ConfigKey& key : kRackKeys) {
        ControlObject::set(key, 1);
    }
    pDecks->mainWindow()->resize(1920, 1080);
}

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
    auto* pMicAux = decks.skin()->findChild<QWidget*>(QStringLiteral("CrateMicAuxRack"));
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

/// A click where the user sees the visible toolbar button `iconName`: the
/// event goes to whatever widget is under that point, as with a real mouse.
void clickToolbarButton(QWidget* pSkin, const QString& iconName) {
    QWidget* pButton = nullptr;
    // By prefix: deck buttons end in their size and deck pair (…SyncAC)
    const QList<QWidget*> buttons = pSkin->findChildren<QWidget*>(
            QRegularExpression(QStringLiteral("^%1").arg(QRegularExpression::escape(iconName))));
    for (QWidget* pCandidate : buttons) {
        if (pCandidate->isVisible() && !pButton) {
            pButton = pCandidate;
        }
    }
    ASSERT_NE(nullptr, pButton) << iconName.toStdString();
    QWidget* pWindow = pButton->window();
    const QPoint inWindow = pButton->mapTo(pWindow, pButton->rect().center());
    QWidget* pHit = pWindow->childAt(inWindow);
    ASSERT_NE(nullptr, pHit) << iconName.toStdString();
    EXPECT_TRUE(pHit == pButton || pButton->isAncestorOf(pHit))
            << iconName.toStdString() << " is covered by "
            << pHit->metaObject()->className() << " "
            << pHit->objectName().toStdString();
    const QPoint local = pHit->mapFrom(pWindow, inWindow);
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type,
                local,
                pHit->mapToGlobal(local),
                Qt::LeftButton,
                type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                Qt::NoModifier);
        QCoreApplication::sendEvent(pHit, &event);
    }
}

TEST_F(CrateSkinLoadTest, ToolbarButtonsOpenTheirRacks) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    const QList<std::pair<QString, ConfigKey>> buttons = {
            {QStringLiteral("TbIconFx"), kRackKeys[0]},
            {QStringLiteral("TbIconSamplers"), kRackKeys[1]},
            {QStringLiteral("TbIconMicAux"), kRackKeys[2]},
    };
    const ConfigKey fourDecks(QStringLiteral("[Skin]"), QStringLiteral("show_4decks"));
    // The full layout (the user's window) and the narrow one below 1366 px
    for (const QSize size : {QSize(1920, 1080), QSize(1280, 800)}) {
        decks.skin()->window()->resize(size);
        QCoreApplication::processEvents();
        for (const auto& [iconName, key] : buttons) {
            const double before = ControlObject::get(key);
            clickToolbarButton(decks.skin(), iconName);
            QCoreApplication::processEvents();
            EXPECT_NE(before, ControlObject::get(key))
                    << iconName.toStdString() << " at " << size.width() << " px";
        }
        // 2 Decks and 4 Decks select a value of show_4decks; they do not toggle
        clickToolbarButton(decks.skin(), QStringLiteral("TbIconDecks4"));
        QCoreApplication::processEvents();
        EXPECT_EQ(1.0, ControlObject::get(fourDecks)) << size.width() << " px";
        clickToolbarButton(decks.skin(), QStringLiteral("TbIconDecks2"));
        QCoreApplication::processEvents();
        EXPECT_EQ(0.0, ControlObject::get(fourDecks)) << size.width() << " px";
    }
    decks.skin()->window()->resize(1920, 1080);
    QCoreApplication::processEvents();
    clickToolbarButton(decks.skin(), QStringLiteral("TbIconMicAux"));
    QCoreApplication::processEvents();
    auto* pMicAux = decks.skin()->findChild<QWidget*>(QStringLiteral("CrateMicAuxRack"));
    ASSERT_NE(nullptr, pMicAux);
    EXPECT_TRUE(pMicAux->isVisible());
    EXPECT_GT(pMicAux->height(), 20);
}

/// The controls the user sees but cannot click: another widget is over their
/// centre. "Class objectName under Class objectName", one per control.
QStringList coveredControls(QWidget* pSkin) {
    QStringList covered;
    const QList<QWidget*> widgets = pSkin->findChildren<QWidget*>();
    for (QWidget* pWidget : widgets) {
        if (!pWidget->inherits("WWidget") || !pWidget->isVisible() ||
                pWidget->width() < 4 || pWidget->height() < 4 ||
                pWidget->testAttribute(Qt::WA_TransparentForMouseEvents)) {
            continue;
        }
        QWidget* pWindow = pWidget->window();
        const QPoint centre = pWidget->mapTo(pWindow, pWidget->rect().center());
        QWidget* pHit = pWindow->childAt(centre);
        if (pHit == pWidget || (pHit && pWidget->isAncestorOf(pHit))) {
            continue;
        }
        // Overlays from LateNight that take the click on purpose: a transparent
        // expand button over the FX and sampler headers, and a sampler's play
        // button over its preview indicator (same control)
        static const QStringList kIntendedOverlays = {
                QStringLiteral("FxExpandOverlay"),
                QStringLiteral("SamplerExpandOverlay"),
                QStringLiteral("PlaySampler"),
        };
        if (pHit && kIntendedOverlays.contains(pHit->objectName())) {
            continue;
        }
        // The same overlays after the racks' restyle: these controls sit under
        // a transparent button that takes the same click
        static const QStringList kCoveredOnPurpose = {
                QStringLiteral("CrateFxExpand"),
                QStringLiteral("CrateSamplerExpand"),
                QStringLiteral("CrateSamplerPlayIndicator"),
        };
        if (kCoveredOnPurpose.contains(pWidget->objectName())) {
            continue;
        }
        covered.append(QStringLiteral("%1 %2 under %3 %4")
                        .arg(QString::fromLatin1(pWidget->metaObject()->className()),
                                pWidget->objectName(),
                                pHit ? QString::fromLatin1(pHit->metaObject()->className())
                                     : QStringLiteral("nothing"),
                                pHit ? pHit->objectName() : QString()));
    }
    covered.removeDuplicates();
    return covered;
}

TEST_F(CrateSkinLoadTest, EveryVisibleControlTakesItsClicks) {
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
    decks.skin()->window()->resize(1920, 1080);
    // An empty deck shows its drop zone and hides every deck control
    for (int deck = 1; deck <= 4; ++deck) {
        // track_loaded is read-only to everyone but the deck
        ControlObject* pLoaded = ControlObject::getControl(ConfigKey(
                QStringLiteral("[Channel%1]").arg(deck), QStringLiteral("track_loaded")));
        ASSERT_NE(nullptr, pLoaded);
        pLoaded->forceSet(1.0);
    }
    const ConfigKey fourDecks(QStringLiteral("[Skin]"), QStringLiteral("show_4decks"));
    for (const double deckCount : {0.0, 1.0}) {
        ControlObject::set(fourDecks, deckCount);
        QCoreApplication::processEvents();
        int visibleSyncButtons = 0;
        static const QRegularExpression kSyncButton(QStringLiteral("^CrateDeckButtonFrameSync"));
        const QList<QWidget*> syncButtons = decks.skin()->findChildren<QWidget*>(kSyncButton);
        for (QWidget* pButton : syncButtons) {
            visibleSyncButtons += pButton->isVisible() ? 1 : 0;
        }
        // The deck controls are part of the inventory
        EXPECT_GE(visibleSyncButtons, deckCount > 0.0 ? 4 : 2);
        const QStringList covered = coveredControls(decks.skin());
        EXPECT_TRUE(covered.isEmpty())
                << (deckCount > 0.0 ? "4 decks" : "2 decks") << ", " << covered.size()
                << " covered:\n  " << covered.join(QStringLiteral("\n  ")).toStdString();
    }
}

TEST_F(CrateSkinLoadTest, EmptyDecksNameTheirLoadKey) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    // The keyboard mappings load the selected track into deck 1 and 2 with
    // Shift + Left and Shift + Right; decks 3 and 4 have no key
    QStringList hints;
    const QList<QLabel*> labels =
            decks.skin()->findChildren<QLabel*>(QStringLiteral("MonoDropHint"));
    for (const QLabel* pLabel : labels) {
        hints.append(pLabel->text());
    }
    hints.removeDuplicates();
    hints.sort();
    EXPECT_EQ(QStringList({QStringLiteral("Or select a track · Shift + ←"),
                      QStringLiteral("Or select a track · Shift + →"),
                      QStringLiteral("Or use Load to in a track's menu")}),
            hints);
}

/// The files of the racks' templates, from the three racks down
QStringList rackTemplateFiles() {
    const QDir skinDir(skinPath(kCrateDecks));
    static const QRegularExpression include(QStringLiteral("src=\"skins:CrateDecks/([^\"]+)\""));
    QStringList pending = {QStringLiteral("fx_rack.xml"),
            QStringLiteral("samplers_rack.xml"),
            QStringLiteral("mic_aux_rack.xml")};
    QStringList files;
    while (!pending.isEmpty()) {
        const QString file = pending.takeFirst();
        if (files.contains(file)) {
            continue;
        }
        files.append(file);
        QFile xml(skinDir.filePath(file));
        EXPECT_TRUE(xml.open(QIODevice::ReadOnly)) << file.toStdString();
        const QString text = QString::fromUtf8(xml.readAll());
        for (auto it = include.globalMatch(text); it.hasNext();) {
            pending.append(it.next().captured(1));
        }
    }
    return files;
}

/// The object names that LateNight's style, which the skin still loads
/// first (skin.xml), selects by id
QSet<QString> lateNightStyledNames() {
    QFile qss(QDir(skinPath(kCrateDecks)).filePath(QStringLiteral("style.qss")));
    EXPECT_TRUE(qss.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(qss.readAll());
    QSet<QString> names;
    static const QRegularExpression id(QStringLiteral("#([A-Za-z_][A-Za-z0-9_-]*)"));
    for (auto it = id.globalMatch(text); it.hasNext();) {
        names.insert(it.next().captured(1));
    }
    return names;
}

/// The skin's own widgets (not groups) in the open racks
QList<QWidget*> rackControls(QWidget* pSkin) {
    QList<QWidget*> controls;
    const QList<QWidget*> racks = pSkin->findChildren<QWidget*>(QStringLiteral("CrateRack"));
    for (QWidget* pRack : racks) {
        if (!pRack->isVisible()) {
            continue;
        }
        const QList<QWidget*> widgets = pRack->findChildren<QWidget*>();
        for (QWidget* pWidget : widgets) {
            if (pWidget->isVisible() && dynamic_cast<WBaseWidget*>(pWidget) &&
                    !qobject_cast<WWidgetGroup*>(pWidget) &&
                    !qobject_cast<WWidgetStack*>(pWidget)) {
                controls.append(pWidget);
            }
        }
    }
    return controls;
}

/// Shows the racks in every state they have: effect units folded and
/// expanded, samplers small and big, mic and aux inputs without and with a
/// device, with two and four decks. Calls `check` in each.
void forEachRackState(OffscreenDecks* pDecks, const std::function<void(const QString&)>& check) {
    openRacksOverLoadedDecks(pDecks);
    for (const bool expanded : {false, true}) {
        for (int unit = 1; unit <= 4; ++unit) {
            ControlObject::set(ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit),
                                       QStringLiteral("show_parameters")),
                    expanded ? 1 : 0);
            // An effect in the first slot shows its parameters
            ControlObject::set(ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1_Effect1]")
                                                 .arg(unit),
                                       QStringLiteral("effect_selector")),
                    1);
        }
        for (const char* samplers : {"expand_samplers_1-4",
                     "expand_samplers_1-8",
                     "expand_samplers_9-16"}) {
            ControlObject::set(ConfigKey("[LateNight]", samplers), expanded ? 1 : 0);
        }
        for (const QString& group : {QStringLiteral("[Microphone]"),
                     QStringLiteral("[Microphone2]"),
                     QStringLiteral("[Auxiliary1]"),
                     QStringLiteral("[Auxiliary2]")}) {
            if (ControlObject* pConfigured = ControlObject::getControl(
                        ConfigKey(group, QStringLiteral("input_configured")))) {
                pConfigured->forceSet(expanded ? 1 : 0);
            }
        }
        for (const int deckCount : {2, 4}) {
            ControlObject::set(ConfigKey("[Skin]", "show_4decks"), deckCount == 4 ? 1 : 0);
            waitFor([] { return false; }, 300);
            check(QStringLiteral("%1 decks%2")
                            .arg(deckCount)
                            .arg(expanded ? QStringLiteral(", expanded") : QString()));
        }
    }
}

TEST_F(CrateSkinLoadTest, RacksUseNoLateNightStyle) {
    // The racks' templates name no LateNight pixmap: every image they use
    // is generated from the tokens
    static const QRegularExpression kSchemeVariable(
            QStringLiteral("name=\"(Btn|Knob|Slider|Style)Scheme\""));
    static const QRegularExpression pixmap(
            QStringLiteral("skins:CrateDecks/(?!generated/)[^<\"]*\\.(svg|png)"));
    const QStringList files = rackTemplateFiles();
    EXPECT_GT(files.size(), 20);
    for (const QString& file : files) {
        QFile xml(QDir(skinPath(kCrateDecks)).filePath(file));
        ASSERT_TRUE(xml.open(QIODevice::ReadOnly)) << file.toStdString();
        const QString text = QString::fromUtf8(xml.readAll());
        EXPECT_FALSE(text.contains(QLatin1String("palemoon"))) << file.toStdString();
        EXPECT_FALSE(text.contains(kSchemeVariable)) << file.toStdString();
        EXPECT_FALSE(text.contains(pixmap)) << file.toStdString();
    }

    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    const QSet<QString> lateNight = lateNightStyledNames();
    ASSERT_TRUE(lateNight.contains(QStringLiteral("FxUnitLabel")));
    forEachRackState(&decks, [&](const QString& state) {
        const QList<QWidget*> racks =
                decks.skin()->findChildren<QWidget*>(QStringLiteral("CrateRack"));
        EXPECT_EQ(3, racks.size()) << state.toStdString();
        QMap<QString, int> crateNames;
        for (QWidget* pRack : racks) {
            const QList<QWidget*> widgets = pRack->findChildren<QWidget*>();
            for (QWidget* pWidget : widgets) {
                const QString name = pWidget->objectName();
                // LateNight's layout helpers (AlignCenter...) only align
                if (!pWidget->isVisible() || name.startsWith(QLatin1String("Align"))) {
                    continue;
                }
                EXPECT_FALSE(lateNight.contains(name))
                        << state.toStdString() << ": " << name.toStdString();
            }
        }
        // Every control is a Crate one
        const QList<QWidget*> controls = rackControls(decks.skin());
        for (QWidget* pControl : controls) {
            const QString name = pControl->objectName();
            EXPECT_TRUE(name.startsWith(QLatin1String("Crate")) ||
                    name.startsWith(QLatin1String("Mono")))
                    << state.toStdString() << ": " << pControl->metaObject()->className()
                    << " \"" << name.toStdString() << "\"";
            ++crateNames[name];
        }
        // The mixer's knobs, the decks' buttons and the admin's fields
        EXPECT_GE(crateNames.value(QStringLiteral("CrateRackKnob")), 6) << state.toStdString();
        EXPECT_GE(crateNames.value(QStringLiteral("CrateRackButtonOn")), 6)
                << state.toStdString();
        EXPECT_GE(crateNames.value(QStringLiteral("CrateRackPfl")), 2) << state.toStdString();
        EXPECT_GE(crateNames.value(QStringLiteral("CrateFxSelector")), 6) << state.toStdString();
        EXPECT_GE(crateNames.value(QStringLiteral("CrateSamplerPlay")), 4) << state.toStdString();
    });
}

TEST_F(CrateSkinLoadTest, RackControlsTakeTheirClicks) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    // Covered on purpose by a transparent button that takes the same
    // click: the effect unit's chevron and the samplers' (by the overlay
    // over the header), and the preview indicator (by the play button)
    const QStringList covered = {QStringLiteral("CrateFxExpand"),
            QStringLiteral("CrateSamplerExpand"),
            QStringLiteral("CrateSamplerPlayIndicator")};
    forEachRackState(&decks, [&](const QString& state) {
        QWidget* pWindow = decks.skin()->window();
        int checked = 0;
        const QList<QWidget*> controls = rackControls(decks.skin());
        for (QWidget* pControl : controls) {
            // Labels and meters only show
            if (qobject_cast<QLabel*>(pControl) ||
                    pControl->testAttribute(Qt::WA_TransparentForMouseEvents) ||
                    covered.contains(pControl->objectName()) ||
                    pControl->rect().isEmpty()) {
                continue;
            }
            const QPoint centre = pControl->mapTo(pWindow, pControl->rect().center());
            QWidget* pHit = pWindow->childAt(centre);
            EXPECT_TRUE(pHit == pControl || pControl->isAncestorOf(pHit))
                    << state.toStdString() << ": " << pControl->objectName().toStdString()
                    << " is under "
                    << (pHit ? pHit->objectName().toStdString() : std::string("nothing"));
            ++checked;
        }
        EXPECT_GT(checked, 40) << state.toStdString();
    });
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
    // Two channels and main with two decks, four and main with four, the
    // two toolbar lanes, and in the racks the 16 expanded samplers and the
    // four mic and four aux inputs
    EXPECT_EQ(2 + 2 + 4 + 2 + 2 + 16 + 4 + 4, meters);
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
    // And painted as the MonoLabel it is in the mockups, whether upstream's key
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
    QObject::connect(pPreferences, &QAction::triggered, [pOpened = &opened] { ++*pOpened; });
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
#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
    GTEST_SKIP() << "QFont::setFeature() needs Qt 6.7";
#endif
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
/// The system check's results on the hybrid laptop plan's XPS 16 (§1), for
/// the screenshots: the Arc held at 20 W, Hyprland 90 % busy, 3840 × 2400 at
/// 90 Hz.
QList<crate::system::CheckResult> hybridLaptopResults() {
    crate::system::Measurement m;
    crate::system::GpuInfo intel;
    intel.vendorId = QStringLiteral("0x8086");
    intel.driver = QStringLiteral("i915");
    intel.integrated = true;
    crate::system::GpuInfo nvidia;
    nvidia.vendorId = QStringLiteral("0x10de");
    nvidia.driver = QStringLiteral("nvidia");
    m.gpus = {intel, nvidia};
    m.render = {true, QStringLiteral("Mesa Intel(R) Arc(TM) Graphics (MTL)"), true};
    m.display = {true, 3840, 2400, 90};
    m.elapsedMs = 5000;
    crate::system::GpuClock clock;
    clock.actualMhz = 520;
    clock.maxMhz = 2350;
    clock.throttleReasons = {QStringLiteral("pl1")};
    clock.idleMs = 0;
    m.clocks = {clock};
    clock.idleMs = 400;
    m.clocks.append(clock);
    crate::system::PowerReading power;
    power.mmioPl1Watts = 20;
    power.msrPl1Watts = 115;
    m.power = {power};
    m.powerHelperAvailable = true;
    m.compositorFirst.pid = 3906;
    m.compositorFirst.name = QStringLiteral("Hyprland");
    m.compositorFirst.engineNs.insert(QStringLiteral("render"), 0);
    m.compositorLast = m.compositorFirst;
    m.compositorLast.engineNs.insert(QStringLiteral("render"), 4'500'000'000);
    m.audio = {{true, 22, 0}, {true, 31, 0}};
    return crate::system::evaluate(m);
}

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
        const QList<WLibrarySidebar*> sidebars = decks.skin()->findChildren<WLibrarySidebar*>();
        for (WLibrarySidebar* pSidebar : sidebars) {
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
        // and without it this build offers no waveform type at all
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
    const QList<WTrackTableView*> views = decks.skin()->findChildren<WTrackTableView*>();
    for (WTrackTableView* pView : views) {
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
    // As upstream confirms the exit while a deck plays
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

/// The decks' positions from their `playposition`: without an audio device
/// the engine reports none.
class ControlPositions : public crate::SmartMixEnvironment {
  public:
    double nowMs() const override {
        return static_cast<double>(m_clock.elapsed());
    }
    double enginePlayPosition(const QString& group) const override {
        return ControlObject::get(ConfigKey(group, QStringLiteral("playposition")));
    }
    QString trackKey(const QString& group) const override {
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
        return pTrack ? pTrack->getLocation() : QString();
    }

  private:
    QElapsedTimer m_clock{[] {
        QElapsedTimer clock;
        clock.start();
        return clock;
    }()};
};

/// A Smart Mix assistant whose plans come from a fake server that answers
/// with the assist fixture (a 96 s long blend with bass handoff), for the
/// tracks of `outgoingFile` and `incomingFile`, deck A → deck B. Not
/// mappable: the app's assistant has the controls.
class FakeServerAssist {
  public:
    FakeServerAssist(const QString& outgoingFile, const QString& incomingFile)
            : m_client(QUrl(QStringLiteral("https://dev.crate.local")),
                      &m_network,
                      [] {
                          crate::ClientOptions options;
                          options.deviceFingerprint = QStringLiteral("install-1");
                          return options;
                      }()),
              m_plans(&m_client) {
        m_client.setTokenSource(&m_tokens);
        crate::SmartMixAssist::Options options;
        options.trackOfFile = [outgoingFile, incomingFile](const QString& location)
                -> std::optional<crate::cache::TrackKey> {
            if (location == outgoingFile) {
                return crate::cache::TrackKey{QStringLiteral("srv"),
                        QStringLiteral("637bd7c0-380e-5bc7-b841-dc2234cdfaf7")};
            }
            if (location == incomingFile) {
                return crate::cache::TrackKey{QStringLiteral("srv"),
                        QStringLiteral("34137088-fcab-5f93-8866-31f1bf6d0871")};
            }
            return std::nullopt;
        };
        options.planClientFor = [this](const QString&) {
            return &m_plans;
        };
        options.capabilitiesOf = [](const QString&) -> std::optional<crate::Capabilities> {
            return crate::Capabilities::fromJson(QJsonDocument::fromJson(
                                                         R"({"smart_mix": {"available": true,
                    "planner_versions": ["smart-mix-v2", "smart-mix-v3"]},
                    "decks": {"available": true,
                    "min_client_version": "0.1.0", "contract_version": 1,
                    "features": ["beatmatch", "bass_handoff", "long_beatmatch"]}})"))
                    .value();
        };
        options.defaultEqOn = [](const QStringList&) {
            return true;
        };
        options.masterDeck = [] {
            return QStringLiteral("[Channel1]");
        };
        options.pEnvironment = std::make_unique<ControlPositions>();
        options.pDirector =
                std::make_unique<crate::SmartMixDirector>(std::make_unique<ControlPositions>());
        options.mappable = false;
        m_pAssist = std::make_unique<crate::SmartMixAssist>(std::move(options));
    }

    crate::SmartMixAssist* assist() const {
        return m_pAssist.get();
    }

    /// Answers the plan request with the assist fixture of `testDir`; true
    /// once the plan is ready.
    bool answerWithThePlan(const QDir& testDir) {
        QFile fixture(testDir.filePath(
                QStringLiteral("crate/data/transition-plans/synthetic-assist-long-blend.json")));
        if (!fixture.open(QIODevice::ReadOnly) ||
                !waitFor([this] { return !m_network.openReplies().isEmpty(); }, 5000)) {
            return false;
        }
        waitFor([] { return false; }, 400);
        const QJsonObject plan = QJsonDocument::fromJson(fixture.readAll()).object();
        const QList<crate::test::FakeReply*> replies = m_network.openReplies();
        for (crate::test::FakeReply* pReply : replies) {
            pReply->respond(200,
                    QJsonDocument(QJsonObject{
                                          {QStringLiteral("plannerVersion"),
                                                  QStringLiteral("smart-mix-v3")},
                                          {QStringLiteral("plans"), QJsonArray{plan}},
                                  })
                            .toJson());
        }
        return waitFor(
                [this] {
                    return m_pAssist->status() == crate::SmartMixAssist::Status::Ready;
                },
                5000);
    }

  private:
    crate::test::FakeNetworkAccessManager m_network;
    crate::test::FakeTokenSource m_tokens;
    crate::Client m_client;
    crate::PlanClient m_plans;
    std::unique_ptr<crate::SmartMixAssist> m_pAssist;
};

/// Dev-only harness, like Capture: the toolbar's Smart Mix button off and on,
/// and the Smart Mix assistant (plan F6.C-PR4) in the library pane with the
/// assist fixture's plan (a 96 s long blend with bass handoff) for deck A →
/// deck B, ready and prepared, at 1920 x 1080 and 1366 x 768. Needs
/// CRATE_SKIN_SCREENSHOTS=<output dir> and CRATE_SKIN_TRACKS=<dir> with two
/// tracks that last as the fixture's (427.3 s and 148.1 s); the plan comes
/// from a fake server.
TEST_F(CrateSkinScreenshots, SmartMix) {
    const QString outDir = qEnvironmentVariable("CRATE_SKIN_SCREENSHOTS");
    const QString tracksDir = qEnvironmentVariable("CRATE_SKIN_TRACKS");
    if (outDir.isEmpty() || tracksDir.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_SKIN_SCREENSHOTS=<dir> and CRATE_SKIN_TRACKS=<dir> to "
                        "render the Smart Mix assistant";
    }
    QDir().mkpath(outDir);
    const QFileInfoList files = QDir(tracksDir).entryInfoList(
            {QStringLiteral("*.mp3"), QStringLiteral("*.flac"), QStringLiteral("*.wav")},
            QDir::Files,
            QDir::Name);
    ASSERT_GE(files.size(), 2) << tracksDir.toStdString();
    const QString outgoingFile = files[0].absoluteFilePath();
    const QString incomingFile = files[1].absoluteFilePath();

    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    SoundSourceProxy::registerProviders();
    decks.writeProfile(kCrateDecks,
            {
                    {ConfigKey("[Library]", "SupportedFileExtensions"),
                            SoundSourceProxy::getSupportedFileSuffixes().join(u',')},
                    {ConfigKey("[Controls]", "PositionDisplay"), QStringLiteral("1")},
                    {ConfigKey("[Key]", "KeyNotation"), QStringLiteral("Lancelot")},
                    {ConfigKey("[Config]", "hide_menubar"), QStringLiteral("1")},
            });
    // A Crate server in the sidebar, for its Smart Mix item
    const QString rows = profile.filePath(QStringLiteral("crate-rows.tsv"));
    {
        QFile file(rows);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(
                "Lattice\tOndine Varga\tGlass\tMelodic Techno\t124\t8A\t427\t6\tfull\n"
                "Northbound Static\tKessler & Moor\tNorth\tProgressive House\t121.17\t9A"
                "\t148\t5\tfull\n");
    }
    ASSERT_TRUE(insertCrateLibrary(profile.path() + QLatin1Char('/'), rows));
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->menuBar()->hide();
    if (auto* pStatus = decks.skin()->findChild<crate::ServerStatus*>()) {
        pStatus->showStatus(QStringLiteral("dev.crate.local"),
                crate::ServerStatus::State::Connected);
    }

    // Deck A plays the outgoing track 7.7 s before the plan's entry; deck B
    // holds the incoming one, as the mockup
    auto pPlayerManager = decks.coreServices()->getPlayerManager();
    const QList<std::tuple<QString, QString, double, QString>> loads = {
            {QStringLiteral("[Channel1]"), outgoingFile, 124.0, QStringLiteral("8A")},
            {QStringLiteral("[Channel2]"), incomingFile, 121.17, QStringLiteral("9A")},
    };
    for (const auto& [group, file, bpm, key] : loads) {
        pPlayerManager->slotLoadLocationToPlayer(file, group, false);
    }
    for (const auto& [group, file, bpm, key] : loads) {
        const QString deck = group;
        ASSERT_TRUE(waitFor(
                [&deck] {
                    return ControlObject::get(ConfigKey(deck, "track_loaded")) > 0 &&
                            ControlObject::get(ConfigKey(deck, "duration")) > 0;
                },
                30000));
        if (TrackPointer pTrack = pPlayerManager->getPlayer(group)->getLoadedTrack()) {
            pTrack->trySetBpm(bpm);
            pTrack->setKeyText(key);
        }
    }
    const double outgoingMs = ControlObject::get(ConfigKey("[Channel1]", "duration")) * 1000.0;
    ASSERT_GT(outgoingMs, 340000.0) << "the outgoing track is shorter than the plan";
    // A hotcue on each track, as a DJ sets them, for the plan's timeline to
    // name: hotcue 3 on deck A where the plan swaps the basses, as the
    // mockup, and hotcue 2 on deck B 32 bars after its entry
    for (const auto& [group, hotcue, ms] : {std::tuple{QStringLiteral("[Channel1]"), 2, 378298.0},
                 std::tuple{QStringLiteral("[Channel2]"), 1, 503.0 + 32 * 4 * 60000.0 / 121.17}}) {
        if (TrackPointer pTrack = pPlayerManager->getPlayer(group)->getLoadedTrack()) {
            pTrack->createAndAddCue(mixxx::CueType::HotCue,
                    hotcue,
                    mixxx::audio::FramePos(ms * pTrack->getSampleRate().toDouble() / 1000.0),
                    mixxx::audio::kInvalidFramePos);
        }
    }
    ControlObject::set(ConfigKey("[Channel1]", "playposition"), (330298.0 - 7700.0) / outgoingMs);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
    ControlObject::set(ConfigKey("[Channel1]", "volume"), 1);
    ControlObject::set(ConfigKey("[Channel2]", "volume"), 0.2);

    // The assistant with a fake server that answers with the assist fixture
    FakeServerAssist server(outgoingFile, incomingFile);
    crate::SmartMixAssist& assist = *server.assist();
    auto* pPanel = decks.skin()->findChild<crate::AssistPanel*>();
    ASSERT_NE(nullptr, pPanel);
    pPanel->setAssist(&assist);
    ASSERT_TRUE(server.answerWithThePlan(getTestDir()));

    const auto capture = [&](const QString& name, const QSize& size) {
        // The analysis of the tones replaces the tempo and key of the mockup,
        // and may finish while the window settles: again before the capture,
        // with time for the plan's timeline to follow the grids
        const auto mockupTempo = [&] {
            for (const auto& [group, file, bpm, key] : loads) {
                if (TrackPointer pTrack = pPlayerManager->getPlayer(group)->getLoadedTrack()) {
                    pTrack->trySetBpm(bpm);
                    pTrack->setKeyText(key);
                }
            }
        };
        mockupTempo();
        decks.mainWindow()->resize(size);
        waitFor([] { return false; }, 1500);
        mockupTempo();
        waitFor([] { return false; }, 400);
        const QImage image = decks.skin()->grab().toImage();
        const QString file = QDir(outDir).filePath(name + QStringLiteral(".png"));
        EXPECT_TRUE(image.save(file)) << file.toStdString();
        // The toolbar's middle, where the Smart Mix button is
        const QImage toolbar = image.copy(QRect(0, 0, image.width() * 2 / 3, 64));
        EXPECT_TRUE(toolbar.save(QDir(outDir).filePath(name + QStringLiteral("-toolbar.png"))));
    };
    const ConfigKey showSmartMix = crate::library::CrateServerFeature::showSmartMixKey();
    ControlObject::set(showSmartMix, 0);
    capture(QStringLiteral("smartmix-off-1920"), QSize(1920, 1080));
    ControlObject::set(showSmartMix, 1);
    EXPECT_EQ(pPanel, decks.skin()->findChild<WLibrary*>()->currentWidget());
    capture(QStringLiteral("smartmix-on-1920"), QSize(1920, 1080));
    capture(QStringLiteral("smartmix-on-1366"), QSize(1366, 768));
    EXPECT_TRUE(assist.prepare());
    capture(QStringLiteral("smartmix-prepared-1920"), QSize(1920, 1080));
    capture(QStringLiteral("smartmix-prepared-1366"), QSize(1366, 768));
    assist.cancel();
    ControlObject::set(showSmartMix, 0);
    capture(QStringLiteral("smartmix-closed-1920"), QSize(1920, 1080));
}

/// The GU1 mockups' library (decks.js TRACKS): title, artist, genre, BPM,
/// key, duration, energy (1 to 10) and profile quality, for the Crate views'
/// screenshots.
struct MockupTrack {
    const char* title;
    const char* artist;
    const char* genre;
    double bpm;
    const char* key;
    const char* time;
    int energy;
    const char* quality;
};

const MockupTrack kMockupTracks[] = {
        {"Lattice", "Ondine Varga", "Melodic Techno", 124.00, "8A", "6:12", 7, "full"},
        {"Northbound Static",
                "Kessler & Moor",
                "Progressive House",
                123.50,
                "9A",
                "7:04",
                6,
                "full"},
        {"Glass Orchard", "Tomas Rielle", "Deep House", 124.00, "8B", "6:48", 6, "full"},
        {"Halcyon Drift", "Mirae", "Melodic Techno", 125.00, "7A", "5:56", 8, "partial"},
        {"Undertow (Extended Mix)",
                "Fenna Calder",
                "Organic House",
                122.00,
                "3A",
                "7:31",
                5,
                "full"},
        {"Cassiopeia", "Ruben Ost", "Techno", 126.00, "9B", "6:22", 8, "full"},
        {"Low Sun", "Aya Lindqvist", "Downtempo", 120.00, "10A", "5:40", 4, "none"},
        {"Paper Satellites", "Odd Harbour", "Melodic House", 124.00, "8A", "6:05", 7, "full"},
        {"Signal Bloom", "Theo Marlin", "Techno", 127.00, "4A", "6:58", 9, "full"},
        {"Meridian", "Kessler & Moor", "Progressive House", 123.00, "7B", "7:12", 6, "partial"},
        {"Night Ferry", "Lumen Ward", "Melodic Techno", 124.00, "8A", "6:44", 7, "full"},
        {"Saltwater Hymn", "Ines Moreau", "Organic House", 121.00, "2A", "6:30", 5, "full"},
        {"Thermal", "Dalia Kross", "Peak Techno", 128.00, "9A", "6:01", 9, "full"},
        {"Afterglow Index", "Pablo Serrat", "Deep House", 124.00, "5A", "7:20", 6, "full"},
        {"Quiet Machines", "Nils Aaberg", "Minimal", 122.50, "8A", "6:15", 5, "full"},
        {"Copper Line", "Mara Esteve", "Melodic House", 124.00, "9A", "6:37", 7, "full"},
        {"Tidal Arrays", "Jun Okabe", "Melodic Techno", 125.00, "8B", "6:50", 8, "full"},
        {"Lantern Season", "Ondine Varga", "Progressive House", 123.00, "7A", "7:02", 6, "full"},
        {"Even Keel", "Rosa Vance", "Melodic Techno", 124.00, "3A", "6:21", 8, "full"},
        {"Sodium Glow", "Kai Brennan", "Techno", 126.00, "10A", "5:48", 9, "partial"},
        {"Velvet Static", "Odd Harbour", "Deep House", 124.00, "8A", "6:28", 6, "full"},
        {"Wake Patterns", "Lea Fournier", "Progressive House", 123.50, "9A", "6:59", 7, "full"},
};

double mockupSeconds(const char* time) {
    const QStringList parts = QString::fromLatin1(time).split(u':');
    return parts.value(0).toInt() * 60 + parts.value(1).toInt();
}

/// The mockup's tracks as the rows of insertCrateLibrary(); the track at
/// index `i` gets the entity uid "uid-<i>".
bool writeMockupCrateRows(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    for (const MockupTrack& track : kMockupTracks) {
        file.write(QStringLiteral("%1\t%2\t\t%3\t%4\t%5\t%6\t%7\t%8\n")
                        .arg(QString::fromUtf8(track.title),
                                QString::fromUtf8(track.artist),
                                QString::fromUtf8(track.genre),
                                QString::number(track.bpm, 'f', 2),
                                QString::fromLatin1(track.key),
                                QString::number(mockupSeconds(track.time)),
                                QString::number(track.energy),
                                QString::fromLatin1(track.quality))
                        .toUtf8());
    }
    return true;
}

/// The server's answer to the compatible tracks of "Lattice" (uid-0), as
/// library-compatibles.html ranks them: track index, score and the
/// harmonic relationship the server gives (`same`, `adjacent`, `relative`;
/// the mockup's "Energy +7" has no relationship of its own, and "—" is
/// `incompatible`).
QByteArray mockupCompatiblesAnswer() {
    const struct {
        int track;
        double score;
        const char* relationship;
    } results[] = {
            {10, 0.94, "same"},
            {7, 0.91, "same"},
            {15, 0.88, "adjacent"},
            {2, 0.86, "relative"},
            {3, 0.83, "adjacent"},
            {16, 0.81, "relative"},
            {18, 0.79, "unknown"},
            {14, 0.77, "same"},
            {20, 0.76, "same"},
            {21, 0.74, "adjacent"},
            {17, 0.71, "adjacent"},
            {12, 0.62, "adjacent"},
            {13, 0.55, "incompatible"},
            {19, 0.52, "incompatible"},
    };
    QJsonArray items;
    for (const auto& result : results) {
        const MockupTrack& track = kMockupTracks[result.track];
        items.append(QJsonObject{
                {QStringLiteral("trackEntityUid"), QStringLiteral("uid-%1").arg(result.track)},
                {QStringLiteral("title"), QString::fromUtf8(track.title)},
                {QStringLiteral("artist"), QString::fromUtf8(track.artist)},
                {QStringLiteral("genre"), QString::fromUtf8(track.genre)},
                {QStringLiteral("score"), result.score},
                {QStringLiteral("confidence"), 0.9},
                {QStringLiteral("scoreBreakdown"),
                        QJsonObject{{QStringLiteral("harmonicRelationship"),
                                QString::fromLatin1(result.relationship)}}},
                {QStringLiteral("duration"), mockupSeconds(track.time)},
                {QStringLiteral("bpm"), track.bpm},
                {QStringLiteral("camelot"), QString::fromLatin1(track.key)},
                {QStringLiteral("energy"), track.energy / 10.0},
        });
    }
    return QJsonDocument(QJsonObject{
                                 {QStringLiteral("seedTrackEntityUid"), QStringLiteral("uid-0")},
                                 {QStringLiteral("context"), QStringLiteral("auto")},
                                 {QStringLiteral("items"), items},
                         })
            .toJson();
}

class HarnessStorageInfo : public crate::cache::StorageInfo {
  public:
    qint64 bytesAvailable(const QString&) const override {
        return 100 * crate::cache::AudioCache::kGiB;
    }
};

crate::test::FakeReply::Headers flacStreamHeaders(qint64 contentLength) {
    return {{QByteArrayLiteral("Content-Length"), QByteArray::number(contentLength)},
            {QByteArrayLiteral("ETag"), QByteArrayLiteral("\"e1\"")},
            {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("audio/flac")}};
}

/// An audio cache of the harness server's tracks whose streams come from a
/// fake network, for the cache view: complete files, an offline set,
/// downloads on their way and waiting. Nothing reaches a server.
class HarnessAudioCache {
  public:
    HarnessAudioCache()
            : m_client(QUrl(QStringLiteral("https://dev.crate.local")), &m_network, [] {
                  crate::ClientOptions options;
                  options.deviceFingerprint = QStringLiteral("install-1");
                  return options;
              }()) {
        m_client.setTokenSource(&m_tokens);
        crate::cache::AudioCache::Options options;
        options.directory = m_dir.path();
        options.clientFor = [this](const QString& serverId) -> crate::Client* {
            return serverId == kServer ? &m_client : nullptr;
        };
        options.limitBytes = 1000 * 1000 * 1000;
        options.pStorageInfo = std::make_shared<HarnessStorageInfo>();
        m_pCache = std::make_unique<crate::cache::AudioCache>(std::move(options));
    }

    crate::cache::AudioCache* cache() const {
        return m_pCache.get();
    }

    bool loaded() {
        return waitFor([this] { return m_pCache->isLoaded(); }, 5000);
    }

    /// The open stream of `uid`, or nullptr.
    crate::test::FakeReply* stream(const QString& uid) {
        const QString path = QStringLiteral("/api/tracks/by-entity/%1/stream").arg(uid);
        crate::test::FakeReply* pFound = nullptr;
        waitFor(
                [&] {
                    for (const auto& sent : std::as_const(m_network.sent)) {
                        if (sent.request.url().path() == path && sent.pReply &&
                                !sent.pReply->isFinished()) {
                            pFound = sent.pReply.data();
                        }
                    }
                    return pFound != nullptr;
                },
                5000);
        return pFound;
    }

    crate::cache::CacheTicket* ensure(int track, crate::cache::Priority priority, qint64 size) {
        crate::cache::TrackHints hints;
        hints.format = QStringLiteral("flac");
        hints.byteLength = size;
        return m_pCache->ensure(kServer, QStringLiteral("uid-%1").arg(track), priority, hints);
    }

    /// Downloads the whole track, `size` bytes.
    bool download(int track, qint64 size) {
        crate::cache::CacheTicket* pTicket = ensure(track, crate::cache::Priority::Prefetch, size);
        bool ready = false;
        QObject context;
        QObject::connect(pTicket, &crate::cache::CacheTicket::ready, &context, [&ready] {
            ready = true;
        });
        crate::test::FakeReply* pReply = stream(QStringLiteral("uid-%1").arg(track));
        if (!pReply) {
            return ready;
        }
        pReply->respond(200, QByteArray(size, 'x'), flacStreamHeaders(size));
        return waitFor([&ready] { return ready; }, 5000);
    }

    /// Starts the download of the track and stops it at `fraction`.
    bool startDownload(
            int track, crate::cache::Priority priority, qint64 size, double fraction) {
        ensure(track, priority, size);
        crate::test::FakeReply* pReply = stream(QStringLiteral("uid-%1").arg(track));
        if (!pReply) {
            return false;
        }
        pReply->respondHeaders(200, flacStreamHeaders(size));
        pReply->sendBody(QByteArray(static_cast<qsizetype>(size * fraction), 'x'));
        return true;
    }

    static inline const QString kServer = QStringLiteral("harness-server");

  private:
    QTemporaryDir m_dir;
    crate::test::FakeNetworkAccessManager m_network;
    crate::test::FakeTokenSource m_tokens;
    crate::Client m_client;
    std::unique_ptr<crate::cache::AudioCache> m_pCache;
};

/// Dev-only harness, like Capture: the Crate views of plan V3.PR2 for the
/// side-by-side comparison with the GU1 mockups (library-compatibles.html,
/// the sidebar of main-2deck.html), at 1920 x 1080 and 1366 x 768, with the
/// library pane at its height under the decks and maximised:
///
/// - `crateviews-sidebar-*`: the Crate section of the sidebar with the
///   mockups' library and its playlists expanded, the library open.
/// - `crateviews-compatibles-*`: the Compatibles view (plan F5.C-PR1) with
///   the mockup's 14 results for "Lattice" (8A, 124 BPM), the first one
///   selected and the fourth under the mouse.
/// - `crateviews-cache-*`: the cache view (plan F4.C-PR4) with complete
///   files, an offline set of the "Warm-up · Nov" playlist, two downloads on
///   their way and three waiting.
///
/// Needs CRATE_SKIN_SCREENSHOTS=<output dir>; the decks stay empty. The
/// tracks are the mockups' (decks.js TRACKS) and nothing reaches a server:
/// the compatibles are the answer the server would give, stored as
/// Compatibles stores it, and the cache's streams come from a fake network.
/// Without a server the app's Compatibles never gets to Ready, so the
/// harness hides the hint, and deck A is on air as in the mockup.
TEST_F(CrateSkinScreenshots, CrateViews) {
    const QString outDir = qEnvironmentVariable("CRATE_SKIN_SCREENSHOTS");
    if (outDir.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_SKIN_SCREENSHOTS=<dir> to render the Crate views";
    }
    QDir().mkpath(outDir);
    const bool openGl = QGuiApplication::platformName() != QLatin1String("offscreen");

    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    const QString settingsPath = profile.path() + QLatin1Char('/');
    OffscreenDecks decks(settingsPath);
    SoundSourceProxy::registerProviders();
    decks.writeProfile(kCrateDecks,
            {
                    {ConfigKey("[Library]", "SupportedFileExtensions"),
                            SoundSourceProxy::getSupportedFileSuffixes().join(u',')},
                    {ConfigKey("[Controls]", "PositionDisplay"), QStringLiteral("1")},
                    {ConfigKey("[Key]", "KeyNotation"), QStringLiteral("Lancelot")},
                    {ConfigKey("[Config]", "hide_menubar"), QStringLiteral("1")},
            });
    const QString rows = profile.filePath(QStringLiteral("crate-rows.tsv"));
    ASSERT_TRUE(writeMockupCrateRows(rows));
    ASSERT_TRUE(insertCrateLibrary(settingsPath, rows));
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->menuBar()->hide();
    if (auto* pStatus = decks.skin()->findChild<crate::ServerStatus*>()) {
        pStatus->showStatus(QStringLiteral("dev.crate.local"),
                crate::ServerStatus::State::Connected);
    }
    auto* pFeature = decks.coreServices()
                             ->getLibrary()
                             ->findChild<crate::library::CrateServerFeature*>();
    ASSERT_NE(nullptr, pFeature);
    auto* pSidebar = decks.skin()->findChild<WLibrarySidebar*>();
    ASSERT_NE(nullptr, pSidebar);
    auto* pSidebarModel = qobject_cast<SidebarModel*>(pSidebar->model());
    ASSERT_NE(nullptr, pSidebarModel);

    // The item of the Crate section whose label starts with `label`: the
    // local section has a "Playlists" of its own
    const auto sidebarItem = [pSidebarModel](const QString& label) {
        QList<QModelIndex> pending;
        for (int row = 0; row < pSidebarModel->rowCount(); ++row) {
            const QModelIndex index = pSidebarModel->index(row, 0);
            if (index.data().toString().startsWith(QLatin1String("Crate"))) {
                pending.append(index);
            }
        }
        while (!pending.isEmpty()) {
            const QModelIndex parent = pending.takeFirst();
            for (int row = 0; row < pSidebarModel->rowCount(parent); ++row) {
                const QModelIndex index = pSidebarModel->index(row, 0, parent);
                if (index.data().toString().startsWith(label)) {
                    return index;
                }
                pending.append(index);
            }
        }
        return QModelIndex();
    };
    // Opens the item as a click does
    const auto open = [&](const QString& label) {
        const QModelIndex index = sidebarItem(label);
        EXPECT_TRUE(index.isValid()) << label.toStdString();
        if (index.isValid()) {
            pSidebar->selectIndex(index, false);
            pSidebarModel->clicked(index);
        }
        waitFor([] { return false; }, 300);
    };
    const auto capture = [&](const QString& name, const QSize& size) {
        // The session, signed out without a server, replaces the status
        if (auto* pStatus = decks.skin()->findChild<crate::ServerStatus*>()) {
            pStatus->showStatus(QStringLiteral("dev.crate.local"),
                    crate::ServerStatus::State::Connected);
        }
        // Again once the layout settled: a view's minimum height from the
        // other size may have held the window taller
        decks.mainWindow()->resize(size);
        waitFor([] { return false; }, 1000);
        decks.mainWindow()->resize(size);
        waitFor([] { return false; }, 1000);
        QImage image = decks.skin()->grab().toImage();
        if (openGl) {
            paintOpenGlWindows(decks.skin(), &image);
        }
        const QString file = QDir(outDir).filePath(name + QStringLiteral(".png"));
        EXPECT_TRUE(image.save(file)) << file.toStdString();
    };
    // Each view at both sizes, under the decks and with the library maximised
    const auto captureView = [&](const QString& view) {
        for (const bool maximised : {false, true}) {
            ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), maximised ? 1 : 0);
            for (const QSize& size : {QSize(1920, 1080), QSize(1366, 768)}) {
                capture(QStringLiteral("crateviews-%1-%2%3")
                                .arg(view)
                                .arg(size.width())
                                .arg(maximised ? QStringLiteral("-maxlib") : QString()),
                        size);
            }
        }
        ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 0);
    };

    // The Crate section with its playlists expanded, the library open
    open(QStringLiteral("Library"));
    // The server's containers fill the sections once its tree is read
    EXPECT_TRUE(waitFor(
            [&] {
                const QModelIndex index = sidebarItem(QStringLiteral("Playlists"));
                return index.isValid() && pSidebarModel->rowCount(index) > 0;
            },
            10000));
    const QModelIndex playlists = sidebarItem(QStringLiteral("Playlists"));
    ASSERT_TRUE(playlists.isValid());
    pSidebar->expand(playlists);
    captureView(QStringLiteral("sidebar"));
    pSidebar->collapse(playlists);

    // Compatibles: the mockup's results for "Lattice", the first selected and
    // the fourth under the mouse
    open(QStringLiteral("Compatibles"));
    auto* pCompatiblesView = decks.skin()->findChild<crate::recs::CompatiblesView*>();
    ASSERT_NE(nullptr, pCompatiblesView);
    EXPECT_EQ(pCompatiblesView, decks.skin()->findChild<WLibrary*>()->currentWidget());
    {
        const auto compatibles = crate::recs::parseCompatibleTracks(mockupCompatiblesAnswer());
        ASSERT_TRUE(compatibles.ok()) << compatibles.error().message.toStdString();
        const QString name = QStringLiteral("crate-views-compatibles");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(settingsPath + QString::fromUtf8(crate::kDatabaseFileName));
            ASSERT_TRUE(db.open());
            const auto stored = crate::recs::storeCompatibles(db,
                    QStringLiteral("harness-server"),
                    QStringLiteral("uid-0"),
                    compatibles.value(),
                    QStringLiteral("2026-11-09T10:00:00Z"),
                    124.0);
            ASSERT_TRUE(stored.ok()) << stored.error().message.toStdString();
            pFeature->compatibles()->model()->setResults(
                    QStringLiteral("harness-server"), QStringLiteral("uid-0"), stored.value());
        }
        QSqlDatabase::removeDatabase(name);
    }
    // Deck A is on air, as in the mockup; without a server the app's
    // Compatibles never gets to Ready, so its hint goes
    pFeature->compatibles()->setMasterDeck(QStringLiteral("[Channel1]"));
    pCompatiblesView->hint()->hide();
    if (auto* pTable = pCompatiblesView->findChild<WTrackTableView*>()) {
        pTable->selectRow(0);
        waitFor([] { return false; }, 200);
        const QPoint at(pTable->viewport()->width() / 2,
                pTable->rowViewportPosition(3) + pTable->rowHeight(3) / 2);
        QMouseEvent move(QEvent::MouseMove,
                at,
                pTable->viewport()->mapToGlobal(at),
                Qt::NoButton,
                Qt::NoButton,
                Qt::NoModifier);
        QCoreApplication::sendEvent(pTable->viewport(), &move);
    }
    captureView(QStringLiteral("compatibles"));

    // The cache, 1 GB: ten complete files (0.5 GB), five of them the offline set of the
    // first playlist ("Warm-up · Nov"), two downloads on their way and three
    // waiting
    HarnessAudioCache cache;
    ASSERT_TRUE(cache.loaded());
    for (int track = 0; track < 10; ++track) {
        ASSERT_TRUE(cache.download(track, (38 + 3 * track) * 1000 * 1000)) << track;
        if (track < 5) {
            cache.cache()->pin(HarnessAudioCache::kServer,
                    QStringLiteral("uid-%1").arg(track),
                    QStringLiteral("offline-set:playlist:1"));
        }
    }
    ASSERT_TRUE(cache.startDownload(10, crate::cache::Priority::Deck, 54 * 1000 * 1000, 0.62));
    ASSERT_TRUE(cache.startDownload(11, crate::cache::Priority::Prefetch, 49 * 1000 * 1000, 0.35));
    cache.ensure(12, crate::cache::Priority::AutoDj, 52 * 1000 * 1000);
    cache.ensure(13, crate::cache::Priority::Prefetch, 58 * 1000 * 1000);
    cache.ensure(14, crate::cache::Priority::OfflineSet, 43 * 1000 * 1000);
    pFeature->cacheStatus()->setSources(cache.cache(), nullptr);
    open(QStringLiteral("Cache"));
    EXPECT_NE(nullptr,
            qobject_cast<crate::CacheStatusView*>(
                    decks.skin()->findChild<WLibrary*>()->currentWidget()));
    captureView(QStringLiteral("cache"));
    // The app's cache again before the harness's goes
    pFeature->cacheStatus()->setSources(nullptr, nullptr);
}

/// The Compatibles view (plan V3.PR2) at 1920 x 1080 and 1366 x 768, under
/// the decks and with the library maximised, with the GU1 mockup's results:
/// opening it leaves the window's size and minimum size as they were (its
/// 470 px wheel used to grow the window to 1149 px and hold it there), the
/// band or the strip shows by the pane's height, the search row gives way to
/// the view's head, and the table has the mockup's columns.
TEST_F(CrateSkinLoadTest, CompatiblesFitsThePaneAndKeepsTheWindowsSize) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    const QString settingsPath = profile.path() + QLatin1Char('/');
    OffscreenDecks decks(settingsPath);
    decks.writeProfile(kCrateDecks, {{ConfigKey("[Config]", "hide_menubar"), QStringLiteral("1")}});
    const QString rows = profile.filePath(QStringLiteral("crate-rows.tsv"));
    ASSERT_TRUE(writeMockupCrateRows(rows));
    ASSERT_TRUE(insertCrateLibrary(settingsPath, rows));
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->menuBar()->hide();
    auto* pFeature = decks.coreServices()
                             ->getLibrary()
                             ->findChild<crate::library::CrateServerFeature*>();
    ASSERT_NE(nullptr, pFeature);
    auto* pSidebar = decks.skin()->findChild<WLibrarySidebar*>();
    ASSERT_NE(nullptr, pSidebar);
    auto* pSidebarModel = qobject_cast<SidebarModel*>(pSidebar->model());
    ASSERT_NE(nullptr, pSidebarModel);
    auto* pLibrary = decks.skin()->findChild<WLibrary*>();
    ASSERT_NE(nullptr, pLibrary);
    auto* pView = decks.skin()->findChild<crate::recs::CompatiblesView*>();
    ASSERT_NE(nullptr, pView);
    QWidget* pSearchRow = decks.skin()->findChild<QWidget*>(QStringLiteral("CrateSearchRow"));
    ASSERT_NE(nullptr, pSearchRow);

    // Opens the Crate section's item `label`, as a click does
    const auto open = [&](const QString& label) {
        QModelIndex found;
        QList<QModelIndex> pending;
        for (int row = 0; row < pSidebarModel->rowCount(); ++row) {
            const QModelIndex index = pSidebarModel->index(row, 0);
            if (index.data().toString().startsWith(QLatin1String("Crate"))) {
                pending.append(index);
            }
        }
        while (!pending.isEmpty() && !found.isValid()) {
            const QModelIndex parent = pending.takeFirst();
            for (int row = 0; row < pSidebarModel->rowCount(parent); ++row) {
                const QModelIndex index = pSidebarModel->index(row, 0, parent);
                if (index.data().toString().startsWith(label)) {
                    found = index;
                    break;
                }
                pending.append(index);
            }
        }
        ASSERT_TRUE(found.isValid()) << label.toStdString();
        pSidebar->selectIndex(found, false);
        pSidebarModel->clicked(found);
        waitFor([] { return false; }, 300);
    };

    // The mockup's results for "Lattice", stored as the app stores them
    {
        const auto compatibles = crate::recs::parseCompatibleTracks(mockupCompatiblesAnswer());
        ASSERT_TRUE(compatibles.ok());
        const QString name = QStringLiteral("crate-compatibles-fit");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(settingsPath + QString::fromUtf8(crate::kDatabaseFileName));
            ASSERT_TRUE(db.open());
            const auto stored = crate::recs::storeCompatibles(db,
                    QStringLiteral("harness-server"),
                    QStringLiteral("uid-0"),
                    compatibles.value(),
                    QStringLiteral("2026-11-09T10:00:00Z"),
                    124.0);
            ASSERT_TRUE(stored.ok());
            pFeature->compatibles()->model()->setResults(
                    QStringLiteral("harness-server"), QStringLiteral("uid-0"), stored.value());
        }
        QSqlDatabase::removeDatabase(name);
    }

    for (const bool maximised : {false, true}) {
        ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), maximised ? 1 : 0);
        for (const QSize size : {QSize(1920, 1080), QSize(1366, 768)}) {
            const std::string at = std::to_string(size.width()) +
                    (maximised ? " px, library maximised" : " px");
            open(QStringLiteral("Library"));
            // Twice: the skin's breakpoints switch its layout once the
            // window has its new size, and the wide one is wider than 1366
            decks.mainWindow()->resize(size);
            waitFor([] { return false; }, 1000);
            decks.mainWindow()->resize(size);
            waitFor([] { return false; }, 1000);
            ASSERT_EQ(size, decks.mainWindow()->size()) << at;
            const QSize minimum = decks.mainWindow()->minimumSizeHint();
            const QSize pane = pLibrary->size();
            EXPECT_TRUE(pSearchRow->isVisible()) << at;

            open(QStringLiteral("Compatibles"));
            ASSERT_EQ(pView, pLibrary->currentWidget()) << at;
            EXPECT_EQ(size, decks.mainWindow()->size()) << at;
            EXPECT_EQ(minimum, decks.mainWindow()->minimumSizeHint()) << at;
            EXPECT_EQ(QSize(0, 0), pView->minimumSizeHint()) << at;
            EXPECT_EQ(QSize(0, 0), pView->sizeHint()) << at;
            // The search row's place goes to the library pane
            EXPECT_FALSE(pSearchRow->isVisible()) << at;
            EXPECT_GE(pLibrary->height(), pane.height()) << at;
            EXPECT_EQ(pLibrary->size(), pView->size()) << at;

            // The band where it fits, else the strip; the list under either,
            // to the bottom of the pane
            EXPECT_NE(pView->band()->isVisible(), pView->strip()->isVisible()) << at;
            const QRect table = pView->table()->geometry();
            EXPECT_EQ(pView->height(), table.bottom() + 1) << at;
            EXPECT_GT(table.height(), 0) << at;
            if (pView->band()->isVisible()) {
                EXPECT_GE(table.top(), pView->band()->geometry().bottom()) << at;
                EXPECT_GE(pView->wheel()->width(), 235) << at;
                EXPECT_LE(pView->wheel()->width(), 470) << at;
            }
            if (maximised && size.width() == 1920) {
                // The mockup's band, with its 470 px wheel
                EXPECT_TRUE(pView->band()->isVisible()) << at;
                EXPECT_EQ(470, pView->wheel()->width()) << at;
            }
            if (!maximised) {
                // Under the decks the pane is too short for the band
                EXPECT_TRUE(pView->strip()->isVisible()) << at;
            }
        }
    }
    ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 0);

    // The mockup's columns, in its order
    auto* pTable = qobject_cast<WTrackTableView*>(pView->table());
    ASSERT_NE(nullptr, pTable);
    QHeaderView* pHeader = pTable->horizontalHeader();
    QStringList shown;
    for (int visual = 0; visual < pHeader->count(); ++visual) {
        const int logical = pHeader->logicalIndex(visual);
        if (!pHeader->isSectionHidden(logical)) {
            shown.append(pTable->model()
                            ->headerData(logical, Qt::Horizontal, TrackModel::kHeaderNameRole)
                            .toString());
        }
    }
    EXPECT_EQ((QStringList{QStringLiteral("position"),
                      QStringLiteral("crate_score"),
                      QStringLiteral("title"),
                      QStringLiteral("artist"),
                      QStringLiteral("bpm"),
                      QStringLiteral("crate_bpm_delta"),
                      QStringLiteral("key"),
                      QStringLiteral("crate_harmonic"),
                      QStringLiteral("crate_energy_delta"),
                      QStringLiteral("duration"),
                      QStringLiteral("crate_cache"),
                      QStringLiteral("crate_liked")}),
            shown);

    // Back to the library: the search row returns
    open(QStringLiteral("Library"));
    EXPECT_TRUE(pSearchRow->isVisible());
}

/// The sidebar's 8 px scroll bar shows only when its items overflow (plan
/// V3.PR2): under the decks at 1366 x 768 they do, with the library
/// maximised at 1920 x 1080 they do not.
TEST_F(CrateSkinLoadTest, SidebarScrollBarShowsOnlyWhenItOverflows) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    const QString settingsPath = profile.path() + QLatin1Char('/');
    OffscreenDecks decks(settingsPath);
    decks.writeProfile(kCrateDecks, {{ConfigKey("[Config]", "hide_menubar"), QStringLiteral("1")}});
    const QString rows = profile.filePath(QStringLiteral("crate-rows.tsv"));
    ASSERT_TRUE(writeMockupCrateRows(rows));
    ASSERT_TRUE(insertCrateLibrary(settingsPath, rows));
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->menuBar()->hide();
    auto* pSidebar = decks.skin()->findChild<WLibrarySidebar*>();
    ASSERT_NE(nullptr, pSidebar);
    EXPECT_EQ(Qt::ScrollBarAsNeeded, pSidebar->verticalScrollBarPolicy());
    // The library's scroll bars: 8 px (tokens.json scrollbarWidth, in QSS)
    constexpr int kScrollbarWidth = 8;
    const struct {
        QSize size;
        bool maximised;
        bool overflows;
    } cases[] = {
            {QSize(1366, 768), false, true},
            {QSize(1920, 1080), true, false},
    };
    for (const auto& item : cases) {
        const std::string at = std::to_string(item.size.width()) +
                (item.maximised ? " px, library maximised" : " px");
        ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), item.maximised ? 1 : 0);
        for (int pass = 0; pass < 2; ++pass) {
            decks.mainWindow()->resize(item.size);
            waitFor([] { return false; }, 1000);
        }
        QScrollBar* pBar = pSidebar->verticalScrollBar();
        EXPECT_EQ(item.overflows, pBar->maximum() > 0) << at;
        EXPECT_EQ(item.overflows, pBar->isVisible()) << at;
        if (pBar->isVisible()) {
            EXPECT_EQ(kScrollbarWidth, pBar->width()) << at;
        }
    }
    ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 0);
}

/// Where `pWidget` is in `pAncestor`.
QRect rectIn(const QWidget* pWidget, const QWidget* pAncestor) {
    return QRect(pWidget->mapTo(pAncestor, QPoint(0, 0)), pWidget->size());
}

/// The Smart Mix assistant at 1920 x 1080 and 1366 x 768 with the library
/// pane at its default height (the height it has with the track table): the
/// window keeps its size, the actions stay whole at the foot of the
/// inspector with nothing under them, every detail of the plan can be seen
/// whole in the details above them, none behind the actions, and the plan's
/// timeline fits beside the inspector.
TEST_F(CrateSkinLoadTest, SmartMixDetailsStayAboveTheActions) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks, {{ConfigKey("[Config]", "hide_menubar"), QStringLiteral("1")}});
    // A Crate server in the sidebar, for its Smart Mix item
    const QString rows = profile.filePath(QStringLiteral("crate-rows.tsv"));
    {
        QFile file(rows);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("Lattice\tOndine Varga\tGlass\tMelodic Techno\t124\t8A\t427\t6\tfull\n");
    }
    ASSERT_TRUE(insertCrateLibrary(profile.path() + QLatin1Char('/'), rows));
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->menuBar()->hide();

    // The decks as the assistant sees them: deck A on air 30 s before the
    // plan's entry, deck B holding the next track
    const QString outgoingFile = profile.filePath(QStringLiteral("01-outgoing.mp3"));
    const QString incomingFile = profile.filePath(QStringLiteral("02-incoming.mp3"));
    const QList<std::tuple<QString, QString, QString, QString, double>> tracks = {
            {QStringLiteral("[Channel1]"),
                    outgoingFile,
                    QStringLiteral("Lattice"),
                    QStringLiteral("Ondine Varga"),
                    427.298},
            {QStringLiteral("[Channel2]"),
                    incomingFile,
                    QStringLiteral("Northbound Static"),
                    QStringLiteral("Kessler & Moor"),
                    148.052},
    };
    for (const auto& [group, file, title, artist, seconds] : tracks) {
        const TrackPointer pTrack = Track::newTemporary(file);
        pTrack->setTitle(title);
        pTrack->setArtist(artist);
        ControlObject::set(ConfigKey(group, "duration"), seconds);
        PlayerInfo::instance().setTrackInfo(group, pTrack);
    }
    ControlObject::set(ConfigKey("[Channel1]", "playposition"), (330298.0 - 30000.0) / 427298.0);
    FakeServerAssist server(outgoingFile, incomingFile);
    auto* pPanel = decks.skin()->findChild<crate::AssistPanel*>();
    ASSERT_NE(nullptr, pPanel);
    pPanel->setAssist(server.assist());
    ASSERT_TRUE(server.answerWithThePlan(getTestDir()));

    auto* pLibrary = decks.skin()->findChild<WLibrary*>();
    ASSERT_NE(nullptr, pLibrary);
    const ConfigKey showSmartMix = crate::library::CrateServerFeature::showSmartMixKey();
    for (const QSize size : {QSize(1920, 1080), QSize(1366, 768)}) {
        const std::string at = std::to_string(size.width()) + " px";
        ControlObject::set(showSmartMix, 0);
        decks.mainWindow()->resize(size);
        waitFor([] { return false; }, 300);
        const QSize pane = pLibrary->size();
        ControlObject::set(showSmartMix, 1);
        waitFor([] { return false; }, 300);
        ASSERT_EQ(pPanel, pLibrary->currentWidget()) << at;

        // The window and the library pane keep their sizes
        EXPECT_EQ(size, decks.mainWindow()->size()) << at;
        EXPECT_EQ(pane, pLibrary->size()) << at;
        EXPECT_EQ(pane, pPanel->size()) << at;

        // The actions: whole, at the foot, nothing under them
        QWidget* const pActions = pPanel->actions();
        const QRect actions = rectIn(pActions, pPanel);
        EXPECT_TRUE(pPanel->rect().contains(actions)) << at;
        EXPECT_GE(actions.bottom(), pPanel->rect().bottom() - 1) << at;
        for (QPushButton* pButton : {pPanel->prepareButton(),
                     pPanel->executeButton(),
                     pPanel->cancelButton()}) {
            EXPECT_TRUE(pButton->isVisible()) << at;
            EXPECT_TRUE(actions.contains(rectIn(pButton, pPanel)))
                    << pButton->text().toStdString() << " at " << at;
        }

        // The details: above the actions, each one seen whole when scrolled
        // to
        QScrollArea* const pDetails = pPanel->details();
        QWidget* const pViewport = pDetails->viewport();
        const QRect viewport = rectIn(pViewport, pPanel);
        EXPECT_LE(viewport.bottom(), actions.top()) << at;
        EXPECT_GT(viewport.height(), 0) << at;
        int details = 0;
        const QList<QWidget*> widgets = pDetails->widget()->findChildren<QWidget*>();
        for (QWidget* pDetail : widgets) {
            if (!pDetail->isVisibleTo(pDetails->widget()) ||
                    !(qobject_cast<QLabel*>(pDetail) ||
                            pDetail->inherits("crate::widgets::LevelRing"))) {
                continue;
            }
            ++details;
            pDetails->ensureWidgetVisible(pDetail, 0, 0);
            QCoreApplication::processEvents();
            const QRect detail = rectIn(pDetail, pViewport);
            const std::string what = pDetail->objectName().toStdString() + " \"" +
                    (qobject_cast<QLabel*>(pDetail)
                                    ? qobject_cast<QLabel*>(pDetail)->text().toStdString()
                                    : std::string()) +
                    "\" at " + at;
            EXPECT_TRUE(pViewport->rect().contains(detail)) << what;
            EXPECT_FALSE(rectIn(pDetail, pPanel).intersects(actions)) << what;
        }
        // Eyebrow, mode, description, state, the readings and the rows
        EXPECT_GE(details, 25) << at;
        pDetails->verticalScrollBar()->setValue(0);

        // The plan's timeline (plan V3.PR1b): whole in the panel, left of
        // the inspector and under the host, at least compact. Where the
        // column has no room for both, it takes the host's place. At
        // 1920 x 1080 it has its lanes
        auto* const pTimeline = pPanel->planView();
        ASSERT_TRUE(pTimeline->isVisible()) << at;
        EXPECT_TRUE(pTimeline->showsTimeline()) << at;
        const QRect timeline = rectIn(pTimeline, pPanel);
        EXPECT_TRUE(pPanel->rect().contains(timeline)) << at;
        EXPECT_GE(timeline.height(), pTimeline->minimumSizeHint().height()) << at;
        EXPECT_FALSE(timeline.intersects(rectIn(pDetails->parentWidget(), pPanel))) << at;
        EXPECT_FALSE(timeline.intersects(actions)) << at;
        if (pPanel->host()->isVisible()) {
            EXPECT_GE(timeline.top(), rectIn(pPanel->host(), pPanel).bottom()) << at;
        }
        if (size.width() == 1920) {
            EXPECT_TRUE(pPanel->host()->isVisible()) << at;
            EXPECT_TRUE(pTimeline->showsLanes()) << at;
        }
    }
}

/// Dev-only harness, like Capture: the window at 1920 x 1080 with the FX,
/// sampler and Mic · Aux racks open over loaded decks, with two and four
/// decks: folded, and with the effect units and the samplers expanded and
/// the mic and aux inputs configured. Runs only with
/// CRATE_SKIN_SCREENSHOTS=<output dir>.
TEST_F(CrateSkinScreenshots, Racks) {
    const QString outDir = qEnvironmentVariable("CRATE_SKIN_SCREENSHOTS");
    if (outDir.isEmpty()) {
        GTEST_SKIP() << "Set CRATE_SKIN_SCREENSHOTS=<dir> to render the racks";
    }
    QDir().mkpath(outDir);
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks,
            {{ConfigKey("[Config]", "hide_menubar"), QStringLiteral("1")}});
    decks.start(application());
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->menuBar()->hide();
    openRacksOverLoadedDecks(&decks);
    // A few knobs off their default, and something on, to see the states
    const QList<std::pair<ConfigKey, double>> values = {
            {ConfigKey("[EffectRack1_EffectUnit1]", "mix"), 0.7},
            {ConfigKey("[EffectRack1_EffectUnit1]", "super1"), 0.35},
            {ConfigKey("[EffectRack1_EffectUnit1_Effect1]", "enabled"), 1},
            {ConfigKey("[EffectRack1_EffectUnit1_Effect1]", "meta"), 0.6},
            {ConfigKey("[EffectRack1_EffectUnit2]", "focused_effect"), 2},
            {ConfigKey("[Sampler1]", "pregain"), 0.7},
            {ConfigKey("[Sampler1]", "sync_enabled"), 1},
            {ConfigKey("[Sampler2]", "pfl"), 1},
            {ConfigKey("[Microphone]", "pregain"), 0.6},
    };
    for (const auto& [key, parameter] : values) {
        if (ControlObject* pControl = ControlObject::getControl(key)) {
            pControl->setParameter(parameter);
        }
    }
    // An effect in each slot of the first two units, so that their
    // parameters show
    for (int unit = 1; unit <= 2; ++unit) {
        for (int effect = 1; effect <= 3; ++effect) {
            ControlObject::set(ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]")
                                                 .arg(unit)
                                                 .arg(effect),
                                       QStringLiteral("effect_selector")),
                    effect);
        }
    }
    for (const bool expanded : {false, true}) {
        for (int unit = 1; unit <= 4; ++unit) {
            ControlObject::set(ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit),
                                       QStringLiteral("show_parameters")),
                    expanded ? 1 : 0);
        }
        for (const char* samplers : {"expand_samplers_1-4",
                     "expand_samplers_1-8",
                     "expand_samplers_9-16"}) {
            ControlObject::set(ConfigKey("[LateNight]", samplers), expanded ? 1 : 0);
        }
        // Without sound devices no input is configured: the expanded shots
        // show the units of configured inputs (input_configured is
        // read-only to everyone but the sound manager)
        for (const QString& group : {QStringLiteral("[Microphone]"),
                     QStringLiteral("[Microphone2]"),
                     QStringLiteral("[Auxiliary1]"),
                     QStringLiteral("[Auxiliary2]")}) {
            if (ControlObject* pConfigured = ControlObject::getControl(
                        ConfigKey(group, QStringLiteral("input_configured")))) {
                pConfigured->forceSet(expanded ? 1 : 0);
            }
        }
        ControlObject::set(ConfigKey("[Microphone]", "talkover"), expanded ? 1 : 0);
        ControlObject::set(ConfigKey("[Auxiliary1]", "main_mix"), expanded ? 1 : 0);
        for (const int deckCount : {2, 4}) {
            ControlObject::set(ConfigKey("[Skin]", "show_4decks"), deckCount == 4 ? 1 : 0);
            waitFor([] { return false; }, 1500);
            const QImage image = decks.skin()->grab().toImage();
            const QString file = QDir(outDir).filePath(
                    QStringLiteral("racks-%1deck-1920%2.png")
                            .arg(deckCount)
                            .arg(expanded ? QStringLiteral("-expanded") : QString()));
            EXPECT_TRUE(image.save(file)) << file.toStdString();
        }
    }
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
/// on the Crate page (also with "Add a server" open and at the mockup's
/// 1366 x 768) and two of upstream's pages, Decks' own dialogs (plan
/// V3.PR3), About, the first-start dialog, the splash screen and the track
/// and column menus. Each one is
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
    home.state = crate::ServerRow::State::SignedIn;
    dev.reachability = crate::Reachability::Online;
    home.reachability = crate::Reachability::Offline;
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
    // "This computer", at the foot of the page, with the plan's XPS results
    pCratePage->systemSection()->setDiscreteGpuOffered(true);
    pCratePage->systemCheck()->showResults(hybridLaptopResults());
    for (QWidget* pAncestor = pCratePage->parentWidget(); pAncestor;
            pAncestor = pAncestor->parentWidget()) {
        if (auto* pScroll = qobject_cast<QScrollArea*>(pAncestor)) {
            pScroll->ensureWidgetVisible(pCratePage->systemSection(), 0, 0);
            break;
        }
    }
    saveOver(pPreferences, withServer, QStringLiteral("crate-prefs-crate-system"));
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
    // V3.PR3: Add a server open, and the page at the mockup's 1366 x 768,
    // from the top of the page
    pTree->setCurrentItem(pCrateItem);
    for (QWidget* pAncestor = pCratePage->parentWidget(); pAncestor;
            pAncestor = pAncestor->parentWidget()) {
        if (auto* pScroll = qobject_cast<QScrollArea*>(pAncestor)) {
            pScroll->ensureVisible(0, 0);
            break;
        }
    }
    if (auto* pAdd = pCratePage->findChild<QPushButton*>(QStringLiteral("crateAddServer"))) {
        pAdd->click();
        saveOver(pPreferences, withServer, QStringLiteral("crate-prefs-crate-add"));
        if (auto* pCancel = pCratePage->findChild<QPushButton*>(QStringLiteral("crateCancel"))) {
            pCancel->click();
        }
    }
    decks.mainWindow()->resize(QSize(1366, 768));
    const QImage small = mainWindow();
    pPreferences->resize(QSize(1180, 704));
    saveOver(pPreferences, small, QStringLiteral("crate-prefs-crate-1366"));
    decks.mainWindow()->resize(QSize(1920, 1080));
    pPreferences->resize(QSize(1180, 940));
    pPreferences->hide();

    // Decks' own dialogs (plan V3.PR3): Music Path, the red confirmations
    // of "Clear cache" and "Sign out and remove", a download and the export
    // to Crate, open as their callers open them: modal, over the scrim
    decks.answerModalDialogs(false);
    {
        const auto capture = [&](crate::CrateDialog* pDialog, const QString& name) {
            pDialog->open();
            saveOver(pDialog, withServer, name);
            pDialog->reject();
        };
        crate::recs::CratePathDialog path(QStringLiteral("Depeche Mode - I Feel Loved"),
                QStringLiteral("Adult. - Lovely Love"),
                decks.mainWindow());
        capture(&path, QStringLiteral("crate-dialog-path"));
        std::unique_ptr<crate::CrateDialog> pClear(crate::createConfirmation(
                crate::DlgPrefCrate::clearCacheConfirmation(), decks.mainWindow()));
        capture(pClear.get(), QStringLiteral("crate-dialog-clear-cache"));
        std::unique_ptr<crate::CrateDialog> pRemove(crate::createConfirmation(
                crate::DlgPrefCrate::removeServerConfirmation(QStringLiteral("dev.crate.local")),
                decks.mainWindow()));
        capture(pRemove.get(), QStringLiteral("crate-dialog-remove-server"));
        crate::Confirmation offline;
        offline.context = QStringLiteral("Download for offline use");
        offline.title = QStringLiteral("Keep it on this computer?");
        offline.text = QStringLiteral(
                "Download 42 tracks, 1.2 GB, and keep them on this computer?");
        offline.action = QStringLiteral("Download");
        std::unique_ptr<crate::CrateDialog> pOffline(
                crate::createConfirmation(offline, decks.mainWindow()));
        capture(pOffline.get(), QStringLiteral("crate-dialog-offline"));
        crate::Choice exportTo;
        exportTo.context = QStringLiteral("Export to Crate");
        exportTo.title = QStringLiteral("Export a playlist");
        exportTo.text = QStringLiteral("Only its Crate tracks of this server go to Crate.");
        exportTo.label = QStringLiteral("Playlist");
        exportTo.items = {QStringLiteral("Warm-up"), QStringLiteral("Peak time")};
        exportTo.action = QStringLiteral("Export");
        std::unique_ptr<crate::CrateDialog> pExport(
                crate::createChoice(exportTo, decks.mainWindow()));
        capture(pExport.get(), QStringLiteral("crate-dialog-export"));
    }
    decks.answerModalDialogs(true);

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

    // The first start's system page, with the plan's XPS results
    {
        crate::FirstRunServices services;
        services.outputs = [] {
            return QList<crate::FirstRunOutput>{
                    {QStringLiteral("0"), QStringLiteral("Built-in Audio")}};
        };
        services.mainOutput = [] {
            return QStringLiteral("0");
        };
        QTemporaryDir emptyRoot;
        services.systemProbes = [&emptyRoot](const crate::CratePreferences*,
                                        const crate::system::PowerHelper*) {
            crate::system::Probes probes;
            probes.root = emptyRoot.path();
            probes.durationMs = 0;
            return probes;
        };
        std::unique_ptr<QDialog> pSystem(crate::createFirstRunDialog(
                crate::CrateServers::instance(), services, decks.mainWindow()));
        pSystem->open();
        for (const char* button : {"crateCancel", "crateFirstRunContinue"}) {
            auto* pPages = pSystem->findChild<QStackedWidget*>(
                    QStringLiteral("CrateFirstRunPages"));
            ASSERT_NE(nullptr, pPages);
            auto* pButton = pPages->currentWidget()->findChild<QPushButton*>(QLatin1String(button));
            ASSERT_NE(nullptr, pButton) << button;
            pButton->click();
        }
        auto* pCheck = pSystem->findChild<crate::system::SystemCheck*>();
        ASSERT_NE(nullptr, pCheck);
        pCheck->showResults(hybridLaptopResults());
        saveOver(pSystem.get(), withoutServer, QStringLiteral("crate-firstrun-system"));
        pSystem->reject();
    }
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
    const QList<WTrackTableView*> views = decks.skin()->findChildren<WTrackTableView*>();
    for (WTrackTableView* pView : views) {
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

    // The Auto DJ and Analyze views of the library, with their button bars,
    // and the Crate section's cache view (F4.C-PR4), here without a cache
    if (auto* pLibraryWidget = decks.skin()->findChild<WLibrary*>()) {
        for (const auto& [view, name] :
                {std::pair{Library::kAutoDJViewName, QStringLiteral("crate-library-autodj")},
                        std::pair{QStringLiteral("Analysis"),
                                QStringLiteral("crate-library-analyze")},
                        std::pair{crate::library::CrateServerFeature::kCacheViewName,
                                QStringLiteral("crate-library-cache")}}) {
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
            linkUpstreamSkin(settingsPath, skin);
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
