#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QIcon>
#include <QLocale>
#include <QPixmap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <QXmlStreamReader>
#include <memory>
#include <vector>

#include "crate/brand/brandtext.h"
#include "crate/brand/brandtranslator.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"

// Crate Decks shows nothing of Mixxx (plan "Own brand", BR.PR1). These tests
// check the brand layer: the rule, the inventory's curated texts and the
// translator that applies them to every tr() lookup, in English and in the
// languages Mixxx ships.

namespace {

QString translationsPath() {
    return ConfigObject<ConfigValue>::computeResourcePath() + QStringLiteral("translations/");
}

TEST(CrateBrandTextTest, RenamesTheApplication) {
    EXPECT_QSTRING_EQ(QStringLiteral("A deck is currently playing. Exit Crate Decks?"),
            crate::brandText(QStringLiteral("A deck is currently playing. Exit Mixxx?")));
    EXPECT_QSTRING_EQ(QStringLiteral("<b>Reconfigure</b> Crate Decks' sound device settings."),
            crate::brandText(QStringLiteral("<b>Reconfigure</b> Mixxx's sound device settings.")));
    EXPECT_QSTRING_EQ(QStringLiteral("CRATE DECKS"), crate::brandText(QStringLiteral("MIXXX")));
    EXPECT_QSTRING_EQ(QStringLiteral("Sobre el Crate Decks (&A)"),
            crate::brandText(QStringLiteral("Sobre el Mixxx (&A)")));
    EXPECT_QSTRING_EQ(QStringLiteral("&Crate Decks-yhteisö (englanniksi)"),
            crate::brandText(QStringLiteral("&Mixxx-yhteisö (englanniksi)")));
    EXPECT_QSTRING_EQ(QStringLiteral("Evoixmr Crate Decks version"),
            crate::brandText(QStringLiteral("Evoixmr mixxx 2.4 version")));
    // Translations glue the name to other scripts and to prefixes
    EXPECT_QSTRING_EQ(QStringLiteral("启动Crate Decks时自动开始DJ。"),
            crate::brandText(QStringLiteral("启动Mixxx时自动开始DJ。")));
    EXPECT_QSTRING_EQ(QStringLiteral("在CRATE DECKS中"),
            crate::brandText(QStringLiteral("在MIXXX中")));
    EXPECT_QSTRING_EQ(QStringLiteral("Library reCrate Decks"),
            crate::brandText(QStringLiteral("Library reMixxx")));
    EXPECT_QSTRING_EQ(QStringLiteral("uporabniški vmesnik Crate Decks"),
            crate::brandText(QStringLiteral("uporabniški vmesnik MIxxx")));
    // Czech declines it and misspells its capitals
    EXPECT_QSTRING_EQ(QStringLiteral("vyčištěn z databáze Crate Decks."),
            crate::brandText(QStringLiteral("vyčištěn z databáze Mixxxu.")));
    EXPECT_QSTRING_EQ(QStringLiteral("kde by měl Crate Decks hledat"),
            crate::brandText(QStringLiteral("kde by měl MixXX hledat")));
    // Finnish declines it after a colon and builds compounds
    EXPECT_QSTRING_EQ(QStringLiteral("Käynnistää Crate Decks:in kokoruututilassa"),
            crate::brandText(QStringLiteral("Käynnistää Mixxx:in kokoruututilassa")));
    EXPECT_QSTRING_EQ(QStringLiteral("Tietoja Crate Decks"),
            crate::brandText(QStringLiteral("Tietoja mixxxistä")));
    EXPECT_QSTRING_EQ(QStringLiteral("määritelty Crate Decks-asetuksissa."),
            crate::brandText(QStringLiteral("määritelty mixxx-asetuksissa.")));
    EXPECT_QSTRING_EQ(QStringLiteral("Soubor Mixxxdb.sqlite může být poškozen."),
            crate::brandText(QStringLiteral("Soubor Mixxxdb.sqlite může být poškozen.")));
}

TEST(CrateBrandTextTest, KeepsFileNamesUrlsAndLongerWords) {
    for (const QString& text : {
                 QStringLiteral("Your mixxxdb.sqlite file may be corrupt."),
                 QStringLiteral("(e.g. ~/.mixxx)"),
                 QStringLiteral("https://www.mixxx.org/support/"),
                 QStringLiteral("http://www.Mixxx.org"),
                 QStringLiteral("org.mixxx.Mixxx"),
                 QStringLiteral("the MixxxControl names"),
                 QStringLiteral("Yaeltex MiniMixxx"),
                 QStringLiteral("BeMixxx"),
                 QStringLiteral("std::optional<mixxx::RgbColor>"),
                 QStringLiteral("mixxx-devel@lists.sourceforge.net"),
                 QStringLiteral("nothing is stored in mixxx.cfg."),
                 QStringLiteral("skins:Mixxx"),
         }) {
        EXPECT_QSTRING_EQ(text, crate::brandText(text));
        EXPECT_FALSE(crate::namesMixxx(text)) << qPrintable(text);
    }
}

TEST(CrateBrandTextTest, DropsMixxxVersions) {
    EXPECT_QSTRING_EQ(QStringLiteral("2-deck control/4-deck mixer preset for Crate Decks. Assumes"),
            crate::brandText(QStringLiteral(
                    "2-deck control/4-deck mixer preset for Mixxx 2.1. Assumes")));
    EXPECT_QSTRING_EQ(QStringLiteral("for Vestax Typhoon in Crate Decks and script"),
            crate::brandText(QStringLiteral("for Vestax Typhoon in Mixxx 2.0+ and script")));
    EXPECT_QSTRING_EQ(QStringLiteral("conversion of the file us428.midi.xml for Crate Decks"),
            crate::brandText(QStringLiteral(
                    "conversion of the file us428.midi.xml for Mixxx 1.6.2")));
    // A number without a dot is not a version
    EXPECT_QSTRING_EQ(QStringLiteral("Crate Decks 4 decks"),
            crate::brandText(QStringLiteral("Mixxx 4 decks")));
}

TEST(CrateBrandTextTest, ShowsCuratedTextsOfLiterals) {
    // Effect manifests name their author with a literal that Preferences shows
    EXPECT_QSTRING_EQ(QStringLiteral("Built-in"),
            crate::brandText(QStringLiteral("The Mixxx Team")));
    EXPECT_QSTRING_EQ(QStringLiteral("Built-in, CAPS Plugins"),
            crate::brandText(QStringLiteral("The Mixxx Team, CAPS Plugins")));
}

TEST(CrateBrandTextTest, ShowsMixxxSeesWhatTheUserReads) {
    EXPECT_TRUE(crate::showsMixxx(QStringLiteral("Exit Mixxx?")));
    EXPECT_TRUE(crate::showsMixxx(QStringLiteral("MixxxControl(s) not found")));
    EXPECT_TRUE(crate::showsMixxx(QStringLiteral("<b>MIXXX</b>")));
    EXPECT_TRUE(crate::showsMixxx(QStringLiteral("see www.mixxx.org")));
    EXPECT_FALSE(crate::showsMixxx(QStringLiteral(
            "<a href='https://manual.mixxx.org'>User manual</a>")));
    EXPECT_FALSE(crate::showsMixxx(QStringLiteral("Your mixxxdb.sqlite file may be corrupt.")));
    EXPECT_FALSE(crate::showsMixxx(QStringLiteral("Yaeltex MiniMixxx")));
}

TEST(CrateBrandTextTest, InventoryIsBuiltIn) {
    const crate::BrandInventory& inventory = crate::BrandInventory::instance();
    ASSERT_FALSE(inventory.isEmpty());
    const auto* pGuide = inventory.string(QStringLiteral("Mixxx DJ Hardware Guide"));
    ASSERT_NE(nullptr, pGuide);
    EXPECT_EQ(crate::BrandInventory::Treatment::Text, pGuide->treatment);
    EXPECT_QSTRING_EQ(QStringLiteral("DJ Hardware Guide"), pGuide->text);
    EXPECT_QSTRING_EQ(QStringLiteral("Guía de hardware para DJ"), pGuide->spanish);
}

/// Every inventory string, branded in English, shows no Mixxx but the
/// credits it keeps.
TEST(CrateBrandTextTest, EveryInventoryStringIsBrandedInEnglish) {
    const crate::BrandInventory& inventory = crate::BrandInventory::instance();
    const QStringList sources = inventory.sources();
    ASSERT_GT(sources.size(), 90);
    for (const QString& source : sources) {
        const auto* pEntry = inventory.string(source);
        ASSERT_NE(nullptr, pEntry);
        QString shown = crate::brandedTranslation(source, QString(), false);
        if (shown.isNull()) {
            shown = source;
        }
        if (pEntry->treatment == crate::BrandInventory::Treatment::Keep) {
            EXPECT_QSTRING_EQ(source, shown);
        } else {
            EXPECT_FALSE(crate::showsMixxx(shown)) << qPrintable(shown);
        }
    }
}

/// Every translation Mixxx ships that names Mixxx (or translates a source
/// that does) comes back without Mixxx, in all its languages.
TEST(CrateBrandTextTest, EveryShippedTranslationIsBranded) {
    const QDir translations(translationsPath());
    const QStringList files = translations.entryList({QStringLiteral("mixxx_*.ts")}, QDir::Files);
    ASSERT_GT(files.size(), 50);
    const crate::BrandInventory& inventory = crate::BrandInventory::instance();
    int checked = 0;
    for (const QString& file : files) {
        const bool spanish = file.startsWith(QLatin1String("mixxx_es"));
        QFile ts(translations.filePath(file));
        ASSERT_TRUE(ts.open(QIODevice::ReadOnly));
        QXmlStreamReader xml(&ts);
        QString source;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) {
                continue;
            }
            if (xml.name() == QLatin1String("source")) {
                source = xml.readElementText();
            } else if (xml.name() == QLatin1String("translation")) {
                const QString type = xml.attributes().value(QLatin1String("type")).toString();
                const QString translation = xml.readElementText(
                        QXmlStreamReader::IncludeChildElements);
                // Qt's lrelease leaves these out of the .qm files
                if (translation.isEmpty() || type == QLatin1String("unfinished") ||
                        type == QLatin1String("vanished") || type == QLatin1String("obsolete")) {
                    continue;
                }
                if (!crate::showsMixxx(translation) && !crate::showsMixxx(source)) {
                    continue;
                }
                const auto* pEntry = inventory.string(source);
                if (pEntry && pEntry->treatment == crate::BrandInventory::Treatment::Keep) {
                    continue;
                }
                QString shown = crate::brandedTranslation(source, translation, spanish);
                if (shown.isNull()) {
                    shown = translation;
                }
                EXPECT_FALSE(crate::showsMixxx(shown))
                        << qPrintable(file) << ": " << qPrintable(shown);
                ++checked;
            }
        }
    }
    EXPECT_GT(checked, 1000);
}

/// Installs the brand translator for one test and restores the default
/// locale and the translators afterwards.
class CrateBrandTranslatorTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_previousLocale = QLocale();
        m_pBrand = crate::installBrandTranslator(QCoreApplication::instance());
        ASSERT_NE(nullptr, m_pBrand);
    }

    void TearDown() override {
        for (const auto& pTranslator : m_mixxxTranslators) {
            QCoreApplication::removeTranslator(pTranslator.get());
        }
        m_mixxxTranslators.clear();
        delete m_pBrand;
        QLocale::setDefault(m_previousLocale);
    }

    /// As mixxx::Translations does in CoreServices: the default locale, then
    /// Mixxx's translation installed after the brand translator.
    void installMixxxTranslation(const QString& localeName) {
        const QLocale locale(localeName);
        QLocale::setDefault(locale);
        auto pTranslator = std::make_unique<QTranslator>();
        ASSERT_TRUE(pTranslator->load(locale,
                QStringLiteral("mixxx"),
                QStringLiteral("_"),
                translationsPath()));
        QCoreApplication::installTranslator(pTranslator.get());
        m_mixxxTranslators.push_back(std::move(pTranslator));
    }

    crate::BrandTranslator* m_pBrand = nullptr;

  private:
    QLocale m_previousLocale;
    std::vector<std::unique_ptr<QTranslator>> m_mixxxTranslators;
};

TEST_F(CrateBrandTranslatorTest, BrandsTrStrings) {
    EXPECT_QSTRING_EQ(QStringLiteral("A deck is currently playing. Exit Crate Decks?"),
            QCoreApplication::translate(
                    "MixxxMainWindow", "A deck is currently playing. Exit Mixxx?"));
    EXPECT_QSTRING_EQ(QStringLiteral("Quits Crate Decks"),
            QCoreApplication::translate("WMainMenuBar", "Quits Mixxx"));
    EXPECT_QSTRING_EQ(QStringLiteral("Confirm Exit"),
            QCoreApplication::translate("MixxxMainWindow", "Confirm Exit"));
}

TEST_F(CrateBrandTranslatorTest, UsesCuratedTexts) {
    EXPECT_QSTRING_EQ(QStringLiteral("DJ Hardware Guide"),
            QCoreApplication::translate("DlgPrefSound", "Mixxx DJ Hardware Guide"));
    EXPECT_QSTRING_EQ(QStringLiteral("Control(s) not found"),
            QCoreApplication::translate("MidiController", "MixxxControl(s) not found"));
}

TEST_F(CrateBrandTranslatorTest, KeepsTheAboutCredits) {
    EXPECT_QSTRING_EQ(QStringLiteral("Donate to Mixxx"),
            QCoreApplication::translate("DlgAbout", "Donate to Mixxx"));
}

TEST_F(CrateBrandTranslatorTest, WinsOverTranslatorsInstalledLater) {
    installMixxxTranslation(QStringLiteral("es_ES"));
    const QString exit = QCoreApplication::translate(
            "MixxxMainWindow", "A deck is currently playing. Exit Mixxx?");
    EXPECT_TRUE(exit.contains(QLatin1String("Crate Decks"))) << qPrintable(exit);
    EXPECT_FALSE(crate::showsMixxx(exit)) << qPrintable(exit);
    EXPECT_FALSE(exit.startsWith(QLatin1String("A deck"))) << "Not in Spanish";
    EXPECT_QSTRING_EQ(QStringLiteral("Guía de hardware para DJ"),
            QCoreApplication::translate("DlgPrefSound", "Mixxx DJ Hardware Guide"));
}

TEST_F(CrateBrandTranslatorTest, BrandsTranslationsThatNameMixxx) {
    // Catalan names the application where English does not
    installMixxxTranslation(QStringLiteral("ca_ES"));
    const QString about = QCoreApplication::translate("WMainMenuBar", "&About");
    EXPECT_QSTRING_EQ(QStringLiteral("Sobre el Crate Decks (&A)"), about);
}

TEST_F(CrateBrandTranslatorTest, BrandsTheCommandLineHelp) {
    EXPECT_QSTRING_EQ(QStringLiteral("Starts Crate Decks in full-screen mode"),
            QCoreApplication::translate("CmdlineArgs", "Starts Mixxx in full-screen mode"));
}

} // namespace
