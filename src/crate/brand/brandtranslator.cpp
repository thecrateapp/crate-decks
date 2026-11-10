#include "crate/brand/brandtranslator.h"

#include <QCoreApplication>
#include <QEvent>
#include <QLocale>

#include "crate/brand/brandtext.h"
#include "moc_brandtranslator.cpp"
#include "preferences/configobject.h"

namespace crate {

namespace {

/// Whether the C string `text` contains "mixxx" in any case: the quick test
/// before a lookup is branded. Every tr() lookup passes here.
bool mentionsMixxx(const char* text) {
    static constexpr char kName[] = "mixxx";
    for (const char* p = text; *p; ++p) {
        int i = 0;
        while (kName[i] && p[i] && (p[i] | 0x20) == kName[i]) {
            ++i;
        }
        if (!kName[i]) {
            return true;
        }
    }
    return false;
}

} // namespace

BrandTranslator::BrandTranslator(QObject* pParent)
        : QTranslator(pParent),
          m_spanish(false),
          m_moving(false) {
}

BrandTranslator::~BrandTranslator() {
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->removeEventFilter(this);
        QCoreApplication::removeTranslator(this);
    }
}

QString BrandTranslator::translate(const char* context,
        const char* sourceText,
        const char* disambiguation,
        int n) const {
    if (!sourceText || !*sourceText) {
        return QString();
    }
    QString translation;
    if (!m_mixxx.isEmpty()) {
        translation = m_mixxx.translate(context, sourceText, disambiguation, n);
    }
    if (!mentionsMixxx(sourceText) &&
            !translation.contains(QLatin1String("mixxx"), Qt::CaseInsensitive)) {
        return QString();
    }
    return brandedTranslation(QString::fromUtf8(sourceText), translation, m_spanish);
}

bool BrandTranslator::isEmpty() const {
    return false;
}

void BrandTranslator::follow(const QLocale& locale, const QString& translationsPath) {
    m_spanish = locale.language() == QLocale::Spanish;
    // As mixxx::Translations: none for the C locale and English (US or any
    // territory), where the sources are the text
    const bool sources = locale.language() == QLocale::C ||
            (locale.language() == QLocale::English &&
                    (locale.territory() == QLocale::UnitedStates ||
                            locale.territory() == QLocale::AnyTerritory));
    if (sources ||
            !m_mixxx.load(locale,
                    QStringLiteral("mixxx"),
                    QStringLiteral("_"),
                    translationsPath)) {
        // Loading nothing empties it
        (void)m_mixxx.load(QString());
    }
}

bool BrandTranslator::eventFilter(QObject* pWatched, QEvent* pEvent) {
    if (pEvent->type() == QEvent::LanguageChange && !m_moving &&
            pWatched == QCoreApplication::instance()) {
        // Another translator was installed or removed: Mixxx's, from
        // CoreServices. Back to the front, following its locale.
        moveToFront();
    }
    return QTranslator::eventFilter(pWatched, pEvent);
}

void BrandTranslator::moveToFront() {
    m_moving = true;
    // Out of Qt's list first, so that no lookup reads Mixxx's translation
    // while it is replaced
    QCoreApplication::removeTranslator(this);
    follow(QLocale(),
            ConfigObject<ConfigValue>::computeResourcePath() + QStringLiteral("translations/"));
    QCoreApplication::installTranslator(this);
    m_moving = false;
}

BrandTranslator* installBrandTranslator(QCoreApplication* pApp) {
    if (!pApp) {
        return nullptr;
    }
    auto* pTranslator = new BrandTranslator(pApp);
    QCoreApplication::installTranslator(pTranslator);
    pApp->installEventFilter(pTranslator);
    return pTranslator;
}

} // namespace crate
