#include "crate/brand/brandtext.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QTextDocumentFragment>

#include "config.h"
#include "crate/appidentity.h"

namespace crate {

namespace {

const QString kInventoryPath = QStringLiteral(":/crate-brand/mixxx-strings.json");

/// "Mixxx" as a word the user reads, in any language: not after a capital
/// Latin letter, a dot, a slash, a backslash, "@" or ":" (~/.mixxx,
/// www.mixxx.org, skins:Mixxx), nor after "Mini" or "Be", the hardware and
/// the author named after it (Yaeltex MiniMixxx, BeMixxx); not before a
/// Latin letter, a digit or "@", a dot, slash or backslash that goes on, or
/// "::" (MixxxControl, mixxx.cfg, mixxx::audio). Lowercase "mixxx-" before
/// an e-mail is an address (mixxx-devel@). Translations glue the name to
/// other scripts, digits and prefixes (Chinese "启动Mixxx时", Tamil "Mixxxஐ",
/// Shona "muMixxx"), decline it (Czech "Mixxxu", Finnish "Mixxx:in",
/// "mixxxistä"), build compounds (Finnish "mixxx-asetuksissa") and misspell
/// its capitals ("MIxxx", "MixXX"). The groups: the capitalised name, the
/// lowercase name, its declension, a possessive "'s" and a version after it
/// ("Mixxx 2.1", "Mixxx 2.0+").
const QRegularExpression kNamesMixxx(
        QStringLiteral("(?<![A-Z_./\\\\@:])(?<!Mini)(?<!Be)"
                       "(?:(M[Ii][Xx][Xx][Xx])|(mixxx)(?!-[A-Za-z0-9_.-]*@))"
                       "(:\\p{Ll}+|\\p{Ll}*)"
                       "(['\\x{2019}]s)?"
                       "(\\s+v?\\d+\\.\\d+(?:\\.\\d+)*(?:\\.x)?\\+?)?"
                       "(?![A-Za-z0-9_@]|[./\\\\][A-Za-z0-9_]|::)"),
        QRegularExpression::UseUnicodePropertiesOption);

const QString kCrateDecks = QStringLiteral("Crate Decks");

/// Mixxx's names for the files and the folder of its settings, which the
/// texts of the inherited code show (plan BR2): the database (Czech writes
/// "Mixxxdb.sqlite", one translation misspells "mixxxdp.sqlite"), the
/// settings and log files and the Linux settings folder ("~/.mixxx", not
/// "~/.mixxx2"). Groups: database, settings, log, folder.
const QRegularExpression kMixxxFiles(
        QStringLiteral("(?<![\\w.-])(?:(mixxxd[bp]\\.sqlite)|(mixxx\\.cfg)|(mixxx\\.log)"
                       "|(~/\\.mixxx)(?![\\w.-]))"),
        QRegularExpression::CaseInsensitiveOption);

/// What showsMixxx() lets through: the hardware and the mapping author named
/// after Mixxx.
const QRegularExpression kAllowedMixxx(
        QStringLiteral("MiniMixxx|BeMixxx"),
        QRegularExpression::CaseInsensitiveOption);

/// `text` with Crate Decks' files and folder where it names Mixxx's.
QString renameMixxxFiles(const QString& text) {
    QString result;
    qsizetype last = 0;
    QRegularExpressionMatchIterator matches = kMixxxFiles.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += QStringView(text).mid(last, match.capturedStart() - last);
        if (match.capturedStart(1) >= 0) {
            result += QLatin1String(kDatabaseFileName);
        } else if (match.capturedStart(2) >= 0) {
            result += QLatin1String(MIXXX_SETTINGS_FILE);
        } else if (match.capturedStart(3) >= 0) {
            result += QLatin1String(kLogFileName);
        } else {
            result += QStringLiteral("~/") + QLatin1String(kSettingsFolderName);
        }
        last = match.capturedEnd();
    }
    if (last == 0) {
        return text;
    }
    result += QStringView(text).mid(last);
    return result;
}

BrandInventory::Treatment treatmentOf(const QString& name) {
    if (name == QLatin1String("text")) {
        return BrandInventory::Treatment::Text;
    }
    if (name == QLatin1String("keep")) {
        return BrandInventory::Treatment::Keep;
    }
    return BrandInventory::Treatment::Rule;
}

} // namespace

bool namesMixxx(const QString& text) {
    return kNamesMixxx.match(text).hasMatch();
}

bool showsMixxx(const QString& text) {
    QString plain = Qt::mightBeRichText(text)
            ? QTextDocumentFragment::fromHtml(text).toPlainText()
            : text;
    plain.remove(kAllowedMixxx);
    return plain.contains(QLatin1String("mixxx"), Qt::CaseInsensitive);
}

QString brandRule(const QString& input) {
    if (!input.contains(QLatin1String("mixxx"), Qt::CaseInsensitive)) {
        return input;
    }
    const QString text = renameMixxxFiles(input);
    QString result;
    qsizetype last = 0;
    QRegularExpressionMatchIterator matches = kNamesMixxx.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += QStringView(text).mid(last, match.capturedStart() - last);
        result += match.capturedView(1) == QLatin1String("MIXXX") ? kCrateDecks.toUpper()
                                                                  : kCrateDecks;
        // Finnish declines foreign names after a colon ("Crate Decks:in");
        // a declension glued to the name (Czech "Mixxxu") is dropped
        if (match.capturedView(3).startsWith(QLatin1Char(':'))) {
            result += match.capturedView(3);
        }
        if (match.capturedStart(4) >= 0) {
            // "Mixxx's" -> "Crate Decks'"
            result += QLatin1Char('\'');
        }
        last = match.capturedEnd();
    }
    if (last == 0) {
        return text;
    }
    result += QStringView(text).mid(last);
    return result;
}

QString brandedTranslation(const QString& source, const QString& translation, bool spanish) {
    if (const auto* pEntry = BrandInventory::instance().string(source)) {
        switch (pEntry->treatment) {
        case BrandInventory::Treatment::Keep:
            return QString();
        case BrandInventory::Treatment::Text:
            return spanish && !pEntry->spanish.isEmpty() ? pEntry->spanish : pEntry->text;
        case BrandInventory::Treatment::Rule:
            break;
        }
    }
    const QString& text = translation.isNull() ? source : translation;
    const QString branded = brandRule(text);
    return branded == text ? QString() : branded;
}

QString brandText(const QString& text) {
    const bool spanish = QLocale().language() == QLocale::Spanish;
    if (const auto* pEntry = BrandInventory::instance().literal(text)) {
        return spanish && !pEntry->spanish.isEmpty() ? pEntry->spanish : pEntry->text;
    }
    const QString branded = brandedTranslation(text, QString(), spanish);
    return branded.isNull() ? text : branded;
}

const BrandInventory& BrandInventory::instance() {
    static const BrandInventory inventory = [] {
        QFile file(kInventoryPath);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "Cannot read the brand inventory" << kInventoryPath;
            return BrandInventory();
        }
        return fromJson(file.readAll());
    }();
    return inventory;
}

BrandInventory BrandInventory::fromJson(const QByteArray& json) {
    BrandInventory inventory;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonArray strings = root.value(QLatin1String("strings")).toArray();
    for (const QJsonValue& value : strings) {
        const QJsonObject object = value.toObject();
        Entry entry;
        entry.treatment = treatmentOf(object.value(QLatin1String("treatment")).toString());
        entry.text = object.value(QLatin1String("text")).toString();
        if (entry.treatment == Treatment::Text && entry.text.isNull()) {
            // A curated empty text: shown as nothing, not as the source
            entry.text = QString::fromUtf8("");
        }
        entry.spanish = object.value(QLatin1String("es")).toString();
        inventory.m_strings.insert(object.value(QLatin1String("source")).toString(), entry);
    }
    const QJsonArray literals = root.value(QLatin1String("literals")).toArray();
    for (const QJsonValue& value : literals) {
        const QJsonObject object = value.toObject();
        if (!object.contains(QLatin1String("shown"))) {
            continue;
        }
        Entry entry;
        entry.treatment = BrandInventory::Treatment::Text;
        entry.text = object.value(QLatin1String("shown")).toString();
        entry.spanish = object.value(QLatin1String("es")).toString();
        inventory.m_literals.insert(object.value(QLatin1String("text")).toString(), entry);
    }
    return inventory;
}

const BrandInventory::Entry* BrandInventory::string(const QString& source) const {
    const auto it = m_strings.constFind(source);
    return it == m_strings.constEnd() ? nullptr : &it.value();
}

const BrandInventory::Entry* BrandInventory::literal(const QString& text) const {
    const auto it = m_literals.constFind(text);
    return it == m_literals.constEnd() ? nullptr : &it.value();
}

QStringList BrandInventory::sources() const {
    return m_strings.keys();
}

bool BrandInventory::isEmpty() const {
    return m_strings.isEmpty();
}

} // namespace crate
