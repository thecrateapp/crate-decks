#pragma once

#include <QFont>
#include <QHash>
#include <QSize>
#include <QString>

#include "preferences/usersettings.h"

class QWidget;

namespace crate {

/// The skin of Crate Decks (res/skins/CrateDecks), which is the default
/// skin (design 5.2, plan V2.PR1).
inline constexpr char kSkinName[] = "CrateDecks";

/// The <SetVariable> values of a skin's generated/vars.xml, which
/// tools/crate-design/generate.py writes from res/crate-design/tokens.json.
/// C++ takes the values QSS cannot use from here, so that no size or
/// tracking is written by hand in C++ either. Empty if there is no such file.
using SkinVariables = QHash<QString, QString>;
SkinVariables readSkinVariables(const QString& varsXmlPath);

/// The window sizes below which the GU1 mockups switch to their 1366 x 768
/// layout (their max-width and max-height media queries).
struct SkinBreakpoints {
    int narrowBelow = 0;
    int shortBelow = 0;

    bool isValid() const {
        return narrowBelow > 0 && shortBelow > 0;
    }
    static SkinBreakpoints fromVariables(const SkinVariables& variables);
};

/// Which of those layouts a skin of `size` uses. The skin reads them from
/// [CrateDecks],layout_narrow and [CrateDecks],layout_short.
struct SkinLayoutClass {
    bool narrow = false;
    bool isShort = false;

    bool operator==(const SkinLayoutClass& other) const = default;
};
SkinLayoutClass skinLayoutClassFor(QSize size, const SkinBreakpoints& breakpoints);

/// `font` as the large numbers of the deck (design 14.3): tabular figures
/// (tnum), the optical size of its pixel size and its weight on the weight
/// axis. QSS has no font-feature-settings nor font-variation-settings; it
/// sets the family, size, weight and tracking.
QFont deckNumberFont(const QFont& font);

/// Gives the controls that exist before a skin loads the defaults of the
/// skin's manifest, before SkinLoader loads it. LegacySkinParser writes a
/// persistent attribute's value to an empty setting but leaves a control that
/// already exists as it is, and SkinControls creates [Skin],show_effectrack,
/// show_samplers and show_microphones at 1 before any skin: on a first start
/// the racks were open whatever the skin said (plan V2.PR3). For every
/// persist="true" attribute whose setting is empty and whose control exists,
/// this sets both, so the skin's default applies once and the user's choice
/// persists from then on. Does nothing for a skin without
/// generated/vars.xml, that is, every skin but CrateDecks.
void applySkinDefaults(const UserSettingsPointer& pConfig, const QString& skinPath);

/// Finishes a skin that SkinLoader has just loaded, with what its QSS cannot
/// do: the font features of the widgets whose object name starts with
/// "DeckNumber", the painters of the mixer's faders and meters
/// (mixerstyle.h), the layout classes from the size of the skin and the
/// Crate server status of the toolbar. Does nothing for a skin without
/// generated/vars.xml, that is, every skin but CrateDecks.
void prepareLoadedSkin(QWidget* pSkinRoot, const QString& skinPath);

} // namespace crate
