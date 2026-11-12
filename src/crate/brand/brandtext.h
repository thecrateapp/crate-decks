#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

class QByteArray;

namespace crate {

// Crate Decks shows nothing of Mixxx (design D2, plan "Own brand"). These
// functions brand the text that Mixxx's code shows, so that Mixxx's sources
// stay as they are: BrandTranslator applies them to every tr() lookup, and
// the few places that show data (controller mappings, effect authors) call
// brandText() directly. Compiled always, like the application identity.

/// Whether `text` names Mixxx as a word the user reads, in any language
/// (Chinese "启动Mixxx时" too). File names, paths, URLs, e-mails, identifiers
/// and longer words do not count: mixxxdb.sqlite, ~/.mixxx,
/// https://mixxx.org, MixxxControl, MiniMixxx, mixxx-devel@.
bool namesMixxx(const QString& text);

/// Whether the user would read "Mixxx" in `text` (rich text is read as its
/// plain text), in any form, Mixxx's file names included, but the hardware
/// and the mapping author named after it (MiniMixxx, BeMixxx). For the tests
/// of what the interface shows.
bool showsMixxx(const QString& text);

/// `text` with Crate Decks where it names Mixxx: the curated text of the
/// inventory for a known literal or source string, else the generic rule:
/// "Mixxx" -> "Crate Decks", "Mixxx's" -> "Crate Decks'", "MIXXX" -> "CRATE
/// DECKS", and a version after the name ("Mixxx 2.1") is dropped. Mixxx's
/// file names become Crate Decks' (plan BR2): mixxx.cfg -> crate-decks.cfg,
/// mixxx.log -> crate-decks.log, mixxxdb.sqlite -> crate-decks.sqlite and
/// ~/.mixxx -> ~/.cratedecks.
QString brandText(const QString& text);

/// The generic rule of brandText() alone. It returns `text` itself, without
/// copying it, when it does not contain "mixxx" in any case, so the log
/// handler can afford it for every message.
QString brandRule(const QString& text);

/// What Crate Decks shows for the source string `source` of a tr() lookup
/// whose translation is `translation` (null without one): the curated text
/// (its Spanish when `spanish`), or the translation (or the source) through
/// the rule. Null when it shows the same as Mixxx: nothing names Mixxx, or the
/// inventory keeps it (the credits in About).
QString brandedTranslation(const QString& source, const QString& translation, bool spanish);

/// The decisions of res/crate-brand/mixxx-strings.json, written by
/// tools/crate-brand/inventory.py.
class BrandInventory {
  public:
    enum class Treatment {
        /// The generic rule
        Rule,
        /// A curated text
        Text,
        /// Shown as Mixxx wrote it
        Keep,
    };
    struct Entry {
        Treatment treatment = Treatment::Rule;
        QString text;
        QString spanish;
    };

    /// The inventory built into the application
    /// (:/crate-brand/mixxx-strings.json)
    static const BrandInventory& instance();
    static BrandInventory fromJson(const QByteArray& json);

    /// The decision for the translatable source string `source`, or nullptr
    const Entry* string(const QString& source) const;
    /// The curated text for the hard-coded literal `text`, or nullptr
    const Entry* literal(const QString& text) const;
    QStringList sources() const;
    bool isEmpty() const;

  private:
    QHash<QString, Entry> m_strings;
    QHash<QString, Entry> m_literals;
};

} // namespace crate
