// The design policy of Decks' own views (plan V3.PR2, design 14): every
// colour comes from the tokens (res/crate-design/tokens.json, through the
// generated vars.xml, the delegates' and instruments' styles and the
// generated QSS), and no widget uses a QGraphicsEffect (design 14.5: glows
// are painted). The tests read the sources of Decks' views: src/crate/ui/,
// and also src/crate/recs/, src/crate/smartmix/ and src/library/crateserver/,
// whose views (Compatibles, the Smart Mix assistant, the Crate section)
// paint too.

#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QStringView>

#include "preferences/configobject.h"

namespace {

const QStringList kScannedDirs = {
        QStringLiteral("crate/ui"),
        QStringLiteral("crate/recs"),
        QStringLiteral("crate/smartmix"),
        QStringLiteral("library/crateserver"),
};

QDir sourceDir() {
    return QDir(QDir(ConfigObject<ConfigValue>::computeResourcePath())
                    .filePath(QStringLiteral("../src")));
}

/// `code` with its comments blanked out (newlines kept, so that lines keep
/// their numbers); string and character literals stay unless
/// `blankStrings`.
QString withoutComments(const QString& code, bool blankStrings = false) {
    QString out = code;
    enum class State {
        Code,
        Line,
        Block,
        String,
        Char,
    };
    State state = State::Code;
    for (qsizetype i = 0; i < out.size(); ++i) {
        const QChar c = out.at(i);
        const QChar next = i + 1 < out.size() ? out.at(i + 1) : QChar();
        switch (state) {
        case State::Code:
            if (c == QLatin1Char('/') && next == QLatin1Char('/')) {
                state = State::Line;
                out[i] = QLatin1Char(' ');
            } else if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
                state = State::Block;
                out[i] = QLatin1Char(' ');
            } else if (c == QLatin1Char('"')) {
                state = State::String;
            } else if (c == QLatin1Char('\'')) {
                state = State::Char;
            }
            break;
        case State::Line:
            if (c == QLatin1Char('\n')) {
                state = State::Code;
            } else {
                out[i] = QLatin1Char(' ');
            }
            break;
        case State::Block:
            if (c == QLatin1Char('*') && next == QLatin1Char('/')) {
                out[i] = QLatin1Char(' ');
                out[i + 1] = QLatin1Char(' ');
                ++i;
                state = State::Code;
            } else if (c != QLatin1Char('\n')) {
                out[i] = QLatin1Char(' ');
            }
            break;
        case State::String:
        case State::Char:
            if (c == QLatin1Char('\\')) {
                if (blankStrings) {
                    out[i] = QLatin1Char(' ');
                    if (i + 1 < out.size() && out.at(i + 1) != QLatin1Char('\n')) {
                        out[i + 1] = QLatin1Char(' ');
                    }
                }
                ++i;
            } else if ((state == State::String && c == QLatin1Char('"')) ||
                    (state == State::Char && c == QLatin1Char('\''))) {
                state = State::Code;
            } else if (blankStrings && c != QLatin1Char('\n')) {
                out[i] = QLatin1Char(' ');
            }
            break;
        }
    }
    return out;
}

int lineOf(const QString& code, qsizetype offset) {
    return static_cast<int>(QStringView(code).left(offset).count(QLatin1Char('\n'))) + 1;
}

/// Where `code` writes a colour of its own: QColor from a "#…" string or a
/// number, qRgb()/qRgba() with numbers, Qt's fixed colours, and a style
/// sheet set from a literal with a colour in it. A transparent pixmap fill
/// (QPixmap::fill(Qt::transparent)) is not a colour: it clears a cache.
/// Comments do not count.
QStringList colourViolations(const QString& source) {
    static const QRegularExpression kColourOfLiteral(QStringLiteral(
            R"(\bQColor(?:\s+\w+)?\s*[({]\s*(?:QStringLiteral\s*\(\s*)?(?:u|QLatin1String\s*\(\s*)?"#)"));
    static const QRegularExpression kColourOfNumber(
            QStringLiteral(R"(\bQColor(?:\s+\w+)?\s*[({]\s*(?:0x[0-9a-fA-F]+|\d+\s*,))"));
    static const QRegularExpression kRgb(QStringLiteral(R"(\bqRgba?\s*\(\s*\d)"));
    static const QRegularExpression kQtColour(QStringLiteral(
            R"(\bQt::(?:white|black|red|darkRed|green|darkGreen|blue|darkBlue|cyan|darkCyan|)"
            R"(magenta|darkMagenta|yellow|darkYellow|gray|darkGray|lightGray|transparent|)"
            R"(color0|color1)\b)"));
    static const QRegularExpression kTransparentFill(
            QStringLiteral(R"(\.fill\s*\(\s*Qt::transparent\s*\))"));
    static const QRegularExpression kStyleSheetLiteral(QStringLiteral(
            R"re(setStyleSheet\s*\(\s*(?:QStringLiteral\s*\(\s*|QLatin1String\s*\(\s*|u)?"([^"]*))re"));
    static const QRegularExpression kCssColour(QStringLiteral(
            R"((?:#[0-9a-fA-F]{3,8}\b|\brgba?\s*\(|\b(?:color|background)\s*:\s*[a-z]+\s*;))"));

    const QString code = withoutComments(source);
    // Without the strings too, for what is code only
    const QString bare = withoutComments(source, true);
    QStringList found;
    const auto add = [&](qsizetype offset, const QString& what) {
        found.append(QStringLiteral("line %1: %2").arg(lineOf(code, offset)).arg(what));
    };
    {
        auto it = kColourOfLiteral.globalMatch(code);
        while (it.hasNext()) {
            const auto match = it.next();
            add(match.capturedStart(), match.captured());
        }
    }
    for (const QRegularExpression* pPattern : {&kColourOfNumber, &kRgb}) {
        auto it = pPattern->globalMatch(bare);
        while (it.hasNext()) {
            const auto match = it.next();
            add(match.capturedStart(), match.captured());
        }
    }
    auto qtColours = kQtColour.globalMatch(bare);
    while (qtColours.hasNext()) {
        const auto match = qtColours.next();
        // A transparent pixmap fill is allowed
        const qsizetype lineStart = bare.lastIndexOf(QLatin1Char('\n'), match.capturedStart()) + 1;
        const qsizetype lineEnd = bare.indexOf(QLatin1Char('\n'), match.capturedStart());
        const QString line = bare.mid(lineStart, lineEnd < 0 ? -1 : lineEnd - lineStart);
        if (match.captured() == QLatin1String("Qt::transparent") &&
                line.contains(kTransparentFill)) {
            continue;
        }
        add(match.capturedStart(), match.captured());
    }
    auto sheets = kStyleSheetLiteral.globalMatch(code);
    while (sheets.hasNext()) {
        const auto match = sheets.next();
        if (match.captured(1).contains(kCssColour)) {
            add(match.capturedStart(), match.captured());
        }
    }
    return found;
}

/// Where `code` uses a QGraphicsEffect; comments do not count.
QStringList graphicsEffectViolations(const QString& source) {
    static const QRegularExpression kEffect(QStringLiteral(
            R"(\bQGraphics\w*Effect\b|\bsetGraphicsEffect\s*\()"));
    const QString code = withoutComments(source);
    QStringList found;
    auto it = kEffect.globalMatch(code);
    while (it.hasNext()) {
        const auto match = it.next();
        found.append(QStringLiteral("line %1: %2")
                        .arg(lineOf(code, match.capturedStart()))
                        .arg(match.captured()));
    }
    return found;
}

/// Every source of the scanned directories, by its path under src/.
QStringList scannedSources() {
    const QDir src = sourceDir();
    QStringList files;
    for (const QString& dir : kScannedDirs) {
        QDirIterator it(src.filePath(dir),
                {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                QDir::Files,
                QDirIterator::Subdirectories);
        while (it.hasNext()) {
            files.append(src.relativeFilePath(it.next()));
        }
    }
    files.sort();
    return files;
}

QString readSource(const QString& relativePath) {
    QFile file(sourceDir().filePath(relativePath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ADD_FAILURE() << "Cannot read " << qPrintable(relativePath);
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

TEST(CrateDesignPolicyTest, NoHardcodedColours) {
    const QStringList files = scannedSources();
    // The views are there to be read
    ASSERT_GT(files.size(), 40);
    ASSERT_TRUE(files.contains(QStringLiteral("crate/recs/compatiblesview.cpp")));
    for (const QString& file : files) {
        const QStringList found = colourViolations(readSource(file));
        EXPECT_TRUE(found.isEmpty()) << qPrintable(file) << ": "
                                     << qPrintable(found.join(QStringLiteral("; ")));
    }
}

TEST(CrateDesignPolicyTest, NoGraphicsEffects) {
    const QStringList files = scannedSources();
    for (const QString& file : files) {
        const QStringList found = graphicsEffectViolations(readSource(file));
        EXPECT_TRUE(found.isEmpty()) << qPrintable(file) << ": "
                                     << qPrintable(found.join(QStringLiteral("; ")));
    }
}

TEST(CrateDesignPolicyTest, CheckersFindWhatTheyLookFor) {
    // Literal colours, in every form the views could write one
    for (const char* code : {
                 R"(QColor accent("#06b6d4");)",
                 R"(const QColor c = QColor(QStringLiteral("#fff"));)",
                 R"(painter.setPen(QColor(0x06b6d4));)",
                 R"(painter.setPen(QColor(6, 182, 212));)",
                 R"(const QColor rim{0x67e8f9};)",
                 R"(image.setPixel(0, 0, qRgb(6, 182, 212));)",
                 R"(painter.fillRect(rect, Qt::white);)",
                 R"(QColor fill = Qt::transparent;)",
                 R"(pLabel->setStyleSheet(QStringLiteral("color: #67e8f9;"));)",
                 R"(pLabel->setStyleSheet("background: rgba(0, 0, 0, 0.5);");)",
         }) {
        EXPECT_FALSE(colourViolations(QString::fromUtf8(code)).isEmpty()) << code;
    }
    // What is not a colour of its own
    for (const char* code : {
                 R"(m_scale.fill(Qt::transparent);)",
                 R"(// Qt::white would be a literal colour)",
                 R"(/* QColor("#fff") */ int x = 0;)",
                 R"(static const QRegularExpression kColour(QStringLiteral("color:#[0-9a-fA-F]{6};"));)",
                 R"(pWidget->setStyleSheet(styleSheet);)",
                 R"(pDialog->setStyleSheet(absoluteSkinUrls(style, resourcePath));)",
                 R"(painter.setPen(m_style.accentHi);)",
                 R"(const QString text = "Qt::white in a string";)",
         }) {
        EXPECT_TRUE(colourViolations(QString::fromUtf8(code)).isEmpty())
                << code << ": " << qPrintable(colourViolations(QString::fromUtf8(code)).join(';'));
    }
    EXPECT_FALSE(graphicsEffectViolations(
            QStringLiteral("pWidget->setGraphicsEffect(new QGraphicsDropShadowEffect);"))
                    .isEmpty());
    EXPECT_FALSE(graphicsEffectViolations(QStringLiteral("#include <QGraphicsEffect>")).isEmpty());
    EXPECT_TRUE(graphicsEffectViolations(
            QStringLiteral("/// painted with QPainter, never a QGraphicsEffect"))
                    .isEmpty());
}

} // namespace
