#pragma once

#include <QColor>
#include <QPixmap>
#include <QString>

#include "crate/ui/skin/crateskin.h"

class QLabel;
class QPushButton;
class QWidget;

namespace crate {

/// The sizes that the dialogs of Crate Decks lay out with in C++ (plan
/// V2.PR5): the "dialog" tokens of res/crate-design/tokens.json, which the
/// generator writes into the CrateDecks skin's generated/vars.xml as
/// CrateDlg<Token>. The dialogs read them from there whatever skin is
/// loaded: they belong to the application. QSS (generated/default.qss)
/// has the colours, fonts and borders.
class DialogMetrics {
  public:
    /// The metrics of the installed CrateDecks skin.
    static const DialogMetrics& installed();
    static DialogMetrics fromVariables(const SkinVariables& variables);

    bool isValid() const {
        return !m_variables.isEmpty();
    }
    /// The token `name` ("prefsWidth"), rounded to whole pixels; 0 if absent.
    int px(const char* name) const;
    /// The token `name` as it is, for fractional strokes; 0 if absent.
    double number(const char* name) const;
    /// A colour of the skin's variables ("CrateBorder").
    QColor colour(const char* variable) const;
    /// All the skin's variables, for the delegates' style.
    const SkinVariables& variables() const {
        return m_variables;
    }

  private:
    SkinVariables m_variables;
};

/// The resource directory of this installation, with a trailing slash.
QString resourcePath();

/// The CrateDecks skin's generated directory, where the style, icons and
/// logo of the dialogs are, whatever skin is loaded.
QString designPath();

/// `styleSheet` with its skins: URLs made absolute under `resourcePath`.
/// The skin parser sets the "skins" search path only while it loads a
/// skin; the application's style and the splash screen are set before.
QString absoluteSkinUrls(const QString& styleSheet, const QString& resourcePath);

/// The application's default style: the generated default.qss of the
/// CrateDecks skin, with absolute URLs. Empty if it cannot be read.
QString defaultStyleSheet(const QString& resourcePath);

/// The style of dialogs: the generated dialogs.qss, with absolute URLs.
QString dialogStyleSheet(const QString& resourcePath);

/// Styles Crate Decks: `pMainWindow` gets `styleSheet` (defaultStyleSheet()),
/// as upstream sets res/skins/default.qss on it, and each dialog gets
/// `styleSheet` and `dialogStyleSheet` as its own style when it is
/// polished: Preferences and About have no parent, so the main window's
/// style does not reach them. With CRATE, MixxxMainWindow calls this instead
/// of setting res/skins/default.qss (design 5.2). Skins keep winning inside
/// the skin. Nothing goes on the application: an application style makes
/// every widget go through QSS, Preferences' hundreds of widgets included,
/// which made the start a quarter of a second slower; a dialog is styled
/// when it first shows. Menus, which are popups, get rounded corners, and
/// the spin boxes of dialogs their padding (what QSS cannot do). Blocking
/// dialogs dim `pMainWindow` behind them (watchBlockingDialogs()). Setting
/// the same styles again does nothing, so that a skin reload does not
/// repolish every widget. Empty styles undo it, for tests.
void applyStyles(QWidget* pMainWindow, const QString& styleSheet, const QString& dialogStyleSheet);

/// applyStyles() with the generated styles under `resourcePath`. False if
/// they cannot be read.
bool applyApplicationStyle(QWidget* pMainWindow, const QString& resourcePath);

/// Gives upstream's splash screen (LaunchImage) the Crate Decks brand instead
/// of the upstream logo, for every skin: the logo, the name and a 1 px progress
/// lane (generated/launch.qss). It replaces the skin's own splash style and
/// Upstream's holiday logo.
void styleLaunchImage(QWidget* pLaunchImage, const QString& resourcePath);

/// The Crate logo (generated/crate-logo.svg) at `width` x `height` logical
/// pixels, for `devicePixelRatio`.
QPixmap logoPixmap(int width, int height, qreal devicePixelRatio);

/// A MonoLabel (design 14.1) named `objectName`, which must start with
/// "Mono" for the style to find it; `accent` gives it the accent tone.
QLabel* monoLabel(const QString& text,
        const QString& objectName,
        QWidget* pParent,
        bool accent = false);

/// Shows `text` (plain text) in `pLabel` as a paragraph of the mockups, with
/// their line height (lineHeightBody) at `fontSizeToken` ("fontSizeBody"),
/// which QSS cannot set on a label.
void setParagraph(QLabel* pLabel, const QString& text, const char* fontSizeToken);

/// Gives a title in display type (Bricolage Grotesque) the optical size of
/// its pixel size and its weight on the weight axis, as the browser does
/// with font-optical-sizing: QSS sets the family, size and weight only.
/// A label that is not shown yet gets them when it first shows, once its
/// dialog has its style.
void setDisplayAxes(QLabel* pLabel);

/// The look of an admin button (base.css .crate-button): "primary" (cyan
/// outline), "ghost" or "danger" (danger-soft), small (32 px) by default or
/// `full` (40 px) at the foot of a dialog.
void setButtonVariant(QPushButton* pButton, const char* variant, bool full = false);

} // namespace crate
