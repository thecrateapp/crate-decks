#include "crate/ui/skin/crateskin.h"

#include <QDir>
#include <QDomDocument>
#include <QEvent>
#include <QFile>
#include <QFontInfo>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QStyleOption>
#include <QWidget>

#include "control/controlobject.h"
#include "crate/crateservers.h"
#include "crate/ui/skin/crateserverstatus.h"
#include "crate/ui/skin/decktrackfields.h"
#include "crate/ui/skin/librarystyle.h"
#include "crate/ui/skin/mixerstyle.h"
#include "crate/ui/skin/preferencescontrol.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::skin");

const QString kVarsXml = QStringLiteral("generated/vars.xml");
const QString kSkinXml = QStringLiteral("skin.xml");
const QString kDeckNumberPrefix = QStringLiteral("DeckNumber");
const QString kMonoPrefix = QStringLiteral("Mono");

const ConfigKey kLayoutNarrowKey(QStringLiteral("[CrateDecks]"), QStringLiteral("layout_narrow"));
const ConfigKey kLayoutShortKey(QStringLiteral("[CrateDecks]"), QStringLiteral("layout_short"));

double variableNumber(const SkinVariables& variables, const QString& name) {
    bool ok = false;
    const double value = variables.value(name).toDouble(&ok);
    return ok ? value : 0;
}

/// Pixel size of a font, also when it was given in points.
qreal pixelSizeOf(const QFont& font) {
    return font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
}

/// Gives the deck numbers of the skin the font features QSS cannot set.
/// QSS resolves its own font properties against the widget's font, so the
/// features survive a restyle.
void applyFonts(QWidget* pSkinRoot) {
    const QList<QWidget*> widgets = pSkinRoot->findChildren<QWidget*>();
    for (QWidget* pWidget : widgets) {
        if (pWidget->objectName().startsWith(kDeckNumberPrefix)) {
            pWidget->ensurePolished();
            pWidget->setFont(deckNumberFont(pWidget->font()));
        }
    }
}

/// Paints a key that is a MonoLabel, the deck header's (GU1 mockups
/// .deck-head), as one: its text in the MonoLabel's type and colour. With
/// Mixxx's key colours on, its default, WKey paints a bar of the key's colour
/// by the text, which the mockups do not have, and it paints both in the
/// style's contents rectangle, which is empty when QSS gives the label no
/// box: the key did not show at all.
class MonoKeyPainter : public QObject {
  public:
    explicit MonoKeyPainter(QLabel* pKey)
            : QObject(pKey),
              m_pKey(pKey) {
        pKey->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pWatched != m_pKey || pEvent->type() != QEvent::Paint) {
            return QObject::eventFilter(pWatched, pEvent);
        }
        // What QLabel paints for plain text
        QPainter painter(m_pKey);
        QStyleOption option;
        option.initFrom(m_pKey);
        QStyle* pStyle = m_pKey->style();
        pStyle->drawPrimitive(QStyle::PE_Widget, &option, &painter, m_pKey);
        pStyle->drawItemText(&painter,
                m_pKey->contentsRect(),
                QStyle::visualAlignment(m_pKey->layoutDirection(), m_pKey->alignment()),
                m_pKey->palette(),
                m_pKey->isEnabled(),
                m_pKey->text(),
                m_pKey->foregroundRole());
        return true;
    }

  private:
    QPointer<QLabel> m_pKey;
};

/// The keys of the skin that are MonoLabels paint as MonoLabels.
void paintMonoKeys(QWidget* pSkinRoot) {
    const QList<QLabel*> labels = pSkinRoot->findChildren<QLabel*>();
    for (QLabel* pLabel : labels) {
        if (pLabel->inherits("WKey") && pLabel->objectName().startsWith(kMonoPrefix)) {
            new MonoKeyPainter(pLabel);
        }
    }
}

/// Sets [CrateDecks],layout_narrow and layout_short from the size of the
/// skin, as the GU1 mockups' media queries do from the window.
class LayoutClassWatcher : public QObject {
  public:
    LayoutClassWatcher(QWidget* pSkinRoot, const SkinBreakpoints& breakpoints)
            : QObject(pSkinRoot),
              m_pSkinRoot(pSkinRoot),
              m_breakpoints(breakpoints) {
        pSkinRoot->installEventFilter(this);
        update();
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pWatched == m_pSkinRoot && pEvent->type() == QEvent::Resize) {
            update();
        }
        return QObject::eventFilter(pWatched, pEvent);
    }

  private:
    void update() {
        const SkinLayoutClass layoutClass =
                skinLayoutClassFor(m_pSkinRoot->size(), m_breakpoints);
        if (m_hasClass && layoutClass == m_class) {
            return;
        }
        m_hasClass = true;
        m_class = layoutClass;
        ControlObject::set(kLayoutNarrowKey, layoutClass.narrow ? 1.0 : 0.0);
        ControlObject::set(kLayoutShortKey, layoutClass.isShort ? 1.0 : 0.0);
    }

    QPointer<QWidget> m_pSkinRoot;
    const SkinBreakpoints m_breakpoints;
    SkinLayoutClass m_class;
    bool m_hasClass = false;
};

} // namespace

SkinVariables readSkinVariables(const QString& varsXmlPath) {
    SkinVariables variables;
    QFile file(varsXmlPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return variables;
    }
    QDomDocument document;
    if (!document.setContent(&file)) {
        kLogger.warning() << "Cannot read the skin variables of" << varsXmlPath;
        return variables;
    }
    for (QDomElement element = document.documentElement().firstChildElement(
                 QStringLiteral("SetVariable"));
            !element.isNull();
            element = element.nextSiblingElement(QStringLiteral("SetVariable"))) {
        variables.insert(element.attribute(QStringLiteral("name")), element.text().trimmed());
    }
    return variables;
}

SkinBreakpoints SkinBreakpoints::fromVariables(const SkinVariables& variables) {
    SkinBreakpoints breakpoints;
    breakpoints.narrowBelow = static_cast<int>(
            variableNumber(variables, QStringLiteral("CrateLayoutNarrowBelow")));
    breakpoints.shortBelow = static_cast<int>(
            variableNumber(variables, QStringLiteral("CrateLayoutShortBelow")));
    return breakpoints;
}

SkinLayoutClass skinLayoutClassFor(QSize size, const SkinBreakpoints& breakpoints) {
    SkinLayoutClass layoutClass;
    if (!breakpoints.isValid() || !size.isValid()) {
        return layoutClass;
    }
    layoutClass.narrow = size.width() < breakpoints.narrowBelow;
    layoutClass.isShort = size.height() < breakpoints.shortBelow;
    return layoutClass;
}

QFont deckNumberFont(const QFont& font) {
    QFont result(font);
    result.setFeature(QFont::Tag("tnum"), 1);
    result.setVariableAxis(QFont::Tag("wght"), static_cast<float>(result.weight()));
    result.setVariableAxis(QFont::Tag("opsz"), static_cast<float>(pixelSizeOf(result)));
    return result;
}

void applySkinDefaults(const UserSettingsPointer& pConfig, const QString& skinPath) {
    const QDir skinDir(skinPath);
    if (!pConfig || !QFile::exists(skinDir.filePath(kVarsXml))) {
        return;
    }
    QFile file(skinDir.filePath(kSkinXml));
    QDomDocument document;
    if (!file.open(QIODevice::ReadOnly) || !document.setContent(&file)) {
        kLogger.warning() << "Cannot read the manifest of" << file.fileName();
        return;
    }
    const QDomElement attributes =
            document.documentElement()
                    .firstChildElement(QStringLiteral("manifest"))
                    .firstChildElement(QStringLiteral("attributes"));
    for (QDomElement attribute = attributes.firstChildElement(QStringLiteral("attribute"));
            !attribute.isNull();
            attribute = attribute.nextSiblingElement(QStringLiteral("attribute"))) {
        if (attribute.attribute(QStringLiteral("persist")) != QLatin1String("true")) {
            continue;
        }
        bool ok = false;
        const double value = attribute.text().trimmed().toDouble(&ok);
        const ConfigKey key = ConfigKey::parseCommaSeparated(
                attribute.attribute(QStringLiteral("config_key")));
        if (!ok || !pConfig->getValueString(key).isEmpty() || !ControlObject::exists(key)) {
            continue;
        }
        pConfig->set(key, ConfigValue(QString::number(value)));
        ControlObject::set(key, value);
    }
}

void prepareLoadedSkin(QWidget* pSkinRoot,
        const QString& skinPath,
        Library* pLibrary,
        const UserSettingsPointer& pConfig) {
    if (!pSkinRoot) {
        return;
    }
    const SkinVariables variables = readSkinVariables(QDir(skinPath).filePath(kVarsXml));
    if (variables.isEmpty()) {
        return;
    }
    applyFonts(pSkinRoot);
    paintMonoKeys(pSkinRoot);
    styleMixer(pSkinRoot, variables);
    styleLibrary(pSkinRoot, variables, skinPath, pLibrary, pConfig);

    const SkinBreakpoints breakpoints = SkinBreakpoints::fromVariables(variables);
    if (breakpoints.isValid() && ControlObject::exists(kLayoutNarrowKey) &&
            ControlObject::exists(kLayoutShortKey)) {
        new LayoutClassWatcher(pSkinRoot, breakpoints);
    }

    auto* pStatus = new ServerStatus(CrateServers::instance(), pSkinRoot);
    bindServerStatus(pStatus, pSkinRoot);

    if (ControlObject::exists(DeckTrackFields::hasArtistKey(1))) {
        new DeckTrackFields(pSkinRoot);
    }

    if (ControlObject::exists(showPreferencesKey())) {
        new PreferencesControl(pSkinRoot);
    }
}

} // namespace crate
