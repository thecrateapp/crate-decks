#include <gtest/gtest.h>

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "preferences/configobject.h"

// The visible strings of the surfaces that AGENTS.md §5.2 lets Crate Decks
// rename (title, About, the first-run menu bar dialog and the preferences)
// name the application "Crate Decks", not "Mixxx". The test reads the
// translatable strings from the sources: <string> elements of the .ui files
// and tr() literals of the C++ files.
//
// TEST_GAP: visible strings that are not translatable (QStringLiteral) are
// not checked; the one in these surfaces that named the application (the
// Serato metadata export warning) was changed by hand.

namespace {

/// "Mixxx" where it refers to Mixxx the project, which stays: Mixxx versions,
/// the "Mixxx" cue modes, the Controller Engine of Mixxx mappings, Mixxx's
/// hardware guide, manual, wiki and forums, and the credits in About. Since
/// the own brand (BR.PR1), the brand layer rewrites them where they show.
const QRegularExpression kMixxxProjectReference(QStringLiteral(
        "Mixxx(?= versions| [0-9]| %[0-9]| mode| Controller Engine"
        "| DJ Hardware Guide| (?i:user manual)| Development Team|</a>"
        "|&nbsp;| Wiki| Controller Forums)"));

const QRegularExpression kTrCall(QStringLiteral(
        "\\btr\\(\\s*((?:\"(?:[^\"\\\\]|\\\\.)*\"\\s*)+)"));
const QRegularExpression kStringLiteral(QStringLiteral("\"((?:[^\"\\\\]|\\\\.)*)\""));

QDir sourceDir() {
    return QDir(QDir(ConfigObject<ConfigValue>::computeResourcePath())
                    .filePath(QStringLiteral("../src")));
}

QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ADD_FAILURE() << "Cannot read " << qPrintable(path);
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

QStringList uiStrings(const QString& path) {
    QStringList strings;
    QDomDocument ui;
    if (!ui.setContent(readFile(path))) {
        ADD_FAILURE() << "Cannot parse " << qPrintable(path);
        return strings;
    }
    const QDomNodeList elements = ui.elementsByTagName(QStringLiteral("string"));
    for (int i = 0; i < elements.size(); ++i) {
        const QDomElement element = elements.at(i).toElement();
        if (element.attribute(QStringLiteral("notr")) != QStringLiteral("true")) {
            strings.append(element.text());
        }
    }
    return strings;
}

/// The literals passed to tr(), with adjacent literals joined.
QStringList trStrings(const QString& source) {
    QStringList strings;
    auto calls = kTrCall.globalMatch(source);
    while (calls.hasNext()) {
        QString joined;
        auto literals = kStringLiteral.globalMatch(calls.next().captured(1));
        while (literals.hasNext()) {
            joined += literals.next().captured(1);
        }
        strings.append(joined.replace(QStringLiteral("\\\""), QStringLiteral("\"")));
    }
    return strings;
}

/// The body of `signature` in `source`, up to the closing brace at column 0.
QString functionBody(const QString& source, const QString& signature) {
    const int start = source.indexOf(signature);
    if (start < 0) {
        ADD_FAILURE() << "Cannot find " << qPrintable(signature);
        return QString();
    }
    const int end = source.indexOf(QStringLiteral("\n}\n"), start);
    return source.mid(start, end - start);
}

void expectNoMixxxAsApplicationName(const QString& file, const QStringList& strings) {
    for (const QString& string : strings) {
        QString withoutProjectReferences = string;
        withoutProjectReferences.remove(kMixxxProjectReference);
        EXPECT_FALSE(withoutProjectReferences.contains(QStringLiteral("Mixxx")))
                << qPrintable(file) << ": " << qPrintable(string);
    }
}

TEST(CrateVisibleStringsTest, UiFilesNameTheApplicationCrateDecks) {
    const QDir src = sourceDir();
    QStringList files = QDir(src.filePath(QStringLiteral("preferences/dialog")))
                                .entryList({QStringLiteral("*.ui")}, QDir::Files);
    for (QString& file : files) {
        file.prepend(QStringLiteral("preferences/dialog/"));
    }
    files << QStringLiteral("controllers/dlgprefcontrollerdlg.ui")
          << QStringLiteral("controllers/dlgprefcontrollersdlg.ui")
          << QStringLiteral("dialog/dlgaboutdlg.ui");
    ASSERT_GT(files.size(), 10);

    for (const QString& file : std::as_const(files)) {
        expectNoMixxxAsApplicationName(file, uiStrings(src.filePath(file)));
    }
}

TEST(CrateVisibleStringsTest, TrStringsNameTheApplicationCrateDecks) {
    const QDir src = sourceDir();
    QStringList files = QDir(src.filePath(QStringLiteral("preferences/dialog")))
                                .entryList({QStringLiteral("*.cpp")}, QDir::Files);
    for (QString& file : files) {
        file.prepend(QStringLiteral("preferences/dialog/"));
    }
    files << QStringLiteral("controllers/dlgprefcontroller.cpp")
          << QStringLiteral("controllers/dlgprefcontrollers.cpp")
          << QStringLiteral("dialog/dlgabout.cpp");
    ASSERT_GT(files.size(), 10);

    for (const QString& file : std::as_const(files)) {
        expectNoMixxxAsApplicationName(file, trStrings(readFile(src.filePath(file))));
    }

    // Of the main window, only the menu bar dialog of the first start.
    const QString mainWindow = QStringLiteral("mixxxmainwindow.cpp");
    const QString menuBarDialog =
            functionBody(readFile(src.filePath(mainWindow)),
                    QStringLiteral("void MixxxMainWindow::alwaysHideMenuBarDlg()"));
    const QStringList menuBarStrings = trStrings(menuBarDialog);
    EXPECT_GE(menuBarStrings.size(), 3);
    expectNoMixxxAsApplicationName(mainWindow, menuBarStrings);
}

TEST(CrateVisibleStringsTest, ProjectReferencesStay) {
    // The scan keeps these, so that credits and links to Mixxx are not lost.
    for (const QString& string : {
                 QStringLiteral("Mixxx 2.7 Development Team"),
                 QStringLiteral("Mixxx mode (no blinking)"),
                 QStringLiteral("Mixxx DJ Hardware Guide"),
                 QStringLiteral("Refer to the Mixxx User Manual for details."),
                 QStringLiteral("a newer Mixxx Controller Engine"),
         }) {
        QString withoutProjectReferences = string;
        withoutProjectReferences.remove(kMixxxProjectReference);
        EXPECT_FALSE(withoutProjectReferences.contains(QStringLiteral("Mixxx")))
                << qPrintable(string);
    }
    QString applicationName = QStringLiteral("Mixxx must be restarted");
    applicationName.remove(kMixxxProjectReference);
    EXPECT_TRUE(applicationName.contains(QStringLiteral("Mixxx")));
}

} // namespace
