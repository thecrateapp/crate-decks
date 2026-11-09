#pragma once

#include <QString>
#include <QTranslator>

class QCoreApplication;
class QEvent;
class QLocale;

namespace crate {

/// Brands every tr() lookup that names Mixxx (plan "Own brand", BR.PR1).
///
/// Qt asks the most recently installed translator first. This one answers
/// only when the source string, or Mixxx's translation of it, names Mixxx:
/// with the curated text of the inventory or with the rule of brandText().
/// Otherwise it answers nothing and Qt goes on to Mixxx's translator.
///
/// Mixxx installs its translators later, in CoreServices. Each install sends
/// QEvent::LanguageChange to the application: the brand translator then
/// moves back to the front and loads Mixxx's translation for the new default
/// locale, so that it can brand translated text too.
class BrandTranslator : public QTranslator {
    Q_OBJECT
  public:
    explicit BrandTranslator(QObject* pParent = nullptr);
    ~BrandTranslator() override;

    QString translate(const char* context,
            const char* sourceText,
            const char* disambiguation = nullptr,
            int n = -1) const override;
    /// Never empty: Qt only sends LanguageChange for translators that are not
    bool isEmpty() const override;

    /// Loads Mixxx's translation for `locale` from `translationsPath`; none
    /// for English and the C locale, as mixxx::Translations.
    void follow(const QLocale& locale, const QString& translationsPath);

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override;

  private:
    void moveToFront();

    QTranslator m_mixxx;
    bool m_spanish;
    bool m_moving;
};

/// Installs a BrandTranslator owned by `pApp` and returns it. Call it right
/// after the application is constructed, before the first tr() lookup
/// (--help) and before Mixxx installs its own translations.
BrandTranslator* installBrandTranslator(QCoreApplication* pApp);

} // namespace crate
