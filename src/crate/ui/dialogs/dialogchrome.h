#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

class QDialog;
class QFrame;
class QHBoxLayout;
class QSizeGrip;

namespace crate {

/// The dialog of the GU1 mockups (decks.css .dialog) around an existing
/// QDialog: no window frame, bgPopover with a 1 px border and the panel
/// radius, and optionally the 44 px title row (the Crate logo, a MonoLabel
/// with `titleParts` joined by " · " and a close button) and a full-width
/// foot with a 1 px rule. The title row moves the window and a corner grip
/// resizes it, through the window system (QWindow::startSystemMove), so
/// both work on Wayland.
///
/// It leaves the dialog's own widgets and layout where they are: the frame
/// is a child behind them, and the title row and the foot sit on top of the
/// layout's margins. It belongs to the dialog.
class DialogChrome : public QObject {
    Q_OBJECT
  public:
    explicit DialogChrome(QDialog* pDialog, const QStringList& titleParts = {});

    /// The foot, created the first time: its layout takes the dialog's
    /// actions, at the full width of the dialog.
    QHBoxLayout* footLayout();

    QFrame* titleRow() const {
        return m_pTitle;
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override;

  private:
    void layoutChrome();

    QPointer<QDialog> m_pDialog;
    QFrame* m_pFrame;
    QFrame* m_pTitle = nullptr;
    QFrame* m_pFoot = nullptr;
    QHBoxLayout* m_pFootLayout = nullptr;
    QSizeGrip* m_pGrip;
};

} // namespace crate
