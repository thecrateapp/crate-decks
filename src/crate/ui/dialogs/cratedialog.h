#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace crate {

/// A dialog of Decks' own (plan V3.PR3) with the grammar of the GU1 first
/// start (first-run): the logo and a MonoLabel in the accent tone that says
/// what it is about, a display title, a paragraph, sections headed by an
/// accent MonoLabel over a 1 px rule, without boxes, and the actions at its
/// foot as Crate buttons. It has the first start's width and the chrome of
/// Decks' dialogs (DialogChrome); opened modal, it dims the main window.
class CrateDialog : public QDialog {
    Q_OBJECT
  public:
    /// What an action of the foot does and how it looks (design 14.1):
    /// Primary is the cyan outline and accepts, Danger the red one and
    /// accepts (only to destroy something), Cancel the ghost one and rejects.
    enum class Action {
        Primary,
        Danger,
        Cancel,
    };

    /// `context` is the head's MonoLabel ("Music Path"), `intro` the
    /// paragraph under the title, as plain text; both may be empty.
    CrateDialog(const QString& context,
            const QString& title,
            const QString& intro,
            QWidget* pParent = nullptr);

    /// A section at the end of the body: a 1 px rule, the accent MonoLabel
    /// `label` and its rows, which go in the layout it returns.
    QVBoxLayout* addSection(const QString& label);

    /// A button at the foot, after the others. A primary action is the
    /// default button (Enter); a destructive one never is.
    QPushButton* addAction(const QString& text, const QString& objectName, Action action);

    /// A MonoLabel at the right of the foot, as the first start's "Later:
    /// Preferences · Crate"; hidden while empty.
    void setLater(const QString& text);

    /// The width of its content: sections and rows never widen the dialog.
    int contentWidth() const;

  private:
    QVBoxLayout* m_pBody;
    QHBoxLayout* m_pFoot;
    QLabel* m_pLater;
    int m_actions = 0;
};

/// A question with one action and Cancel, for confirmations ("Clear the
/// cache?").
struct Confirmation {
    QString context;
    QString title;
    QString text;
    /// The action's text, a verb ("Clear cache"), never "Yes".
    QString action;
    /// Red, and not the default button: the action destroys something.
    bool destructive = false;
    /// "Cancel" when empty.
    QString cancel;
};

/// The dialog of `confirmation`: accepted by its action (crateConfirm),
/// rejected by Cancel (crateCancel).
CrateDialog* createConfirmation(const Confirmation& confirmation, QWidget* pParent = nullptr);

/// A choice from a list, for what QInputDialog::getItem() asked.
struct Choice {
    QString context;
    QString title;
    QString text;
    /// The section's MonoLabel over the list.
    QString label;
    QStringList items;
    QString action;
};

/// The dialog of `choice`: a combo box (crateChoice) in a section, accepted
/// by its action (crateConfirm), rejected by Cancel (crateCancel).
CrateDialog* createChoice(const Choice& choice, QWidget* pParent = nullptr);

/// The combo box of a dialog of createChoice().
QComboBox* choiceBox(const QDialog* pDialog);

} // namespace crate
