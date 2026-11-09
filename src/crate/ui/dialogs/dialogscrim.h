#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QString>

class QDialog;
class QWidget;
class QWindow;

namespace crate {

/// Dims the main window behind a blocking dialog (design 14.3, "Velo detrás
/// de los diálogos"): the scrim of the GU1 mockups, the scrim token (bgApp
/// at 72 %), over the whole main window, OpenGL waveforms included. The
/// waveforms are native windows of their own, which no widget of the main
/// window can paint over, so the scrim is a window too:
/// - TopLevel (X11, Windows, macOS): a frameless, translucent window over
///   the main window's frame, which the window manager composites over all
///   of it. It shows before the dialog maps, so the dialog stays above it.
/// - ChildWindow (Wayland): a translucent child window of the main window,
///   that is a subsurface, which the compositor stacks above the
///   subsurfaces created before it: the waveforms'. A Wayland client can
///   neither place a top-level window nor learn where its main window is,
///   so a top-level scrim could neither cover the main window nor follow
///   it. It covers the window's contents, not the title bar that the client
///   draws (there is none in full screen).
/// It follows the main window while it moves and resizes. On Wayland it goes
/// back above a native child of the main window that shows while it is up:
/// a native child that shows gets a new subsurface, on top.
class DialogScrim : public QObject {
  public:
    enum class Kind {
        TopLevel,
        ChildWindow,
    };

    /// The kind that works on `platformName` (QGuiApplication::platformName()).
    static Kind kindForPlatform(const QString& platformName);

    DialogScrim(QWidget* pMainWindow, Kind kind, QObject* pParent = nullptr);
    ~DialogScrim() override;

    /// Shows or removes the scrim. Each time it shows it is a new window, so
    /// that it stacks above the native windows the main window has by then.
    void setActive(bool active);
    bool isActive() const;

    Kind kind() const {
        return m_kind;
    }
    QColor colour() const {
        return m_colour;
    }
    /// The scrim's window while it is active, nullptr otherwise.
    QWindow* window() const;

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override;

  private:
    void place();
    /// A new scrim, above the main window's native children.
    void restack();

    const QPointer<QWidget> m_pMainWindow;
    const Kind m_kind;
    const QColor m_colour;
    QPointer<QWindow> m_pWindow;
    bool m_restackPending = false;
};

/// Whether `pDialog` blocks the main window and takes the scrim: a dialog
/// modal to the application, or to `pMainWindow`'s windows, that shows on
/// screen (a native dialog does not, and it is not Crate's) and is not
/// Preferences. Preferences is not modal from the toolbar, so that the DJ
/// can adjust something mid-set watching the decks; Mixxx also opens it
/// modal after a sound device fails, and it stays without the scrim then.
bool dialogTakesScrim(const QDialog* pDialog, const QWidget* pMainWindow);

/// Shows the scrim over `pMainWindow` while a dialog that takes it
/// (dialogTakesScrim()) is open and the main window is on screen. nullptr
/// stops watching. applyStyles() calls it with Crate's dialog style.
void watchBlockingDialogs(QWidget* pMainWindow);

/// The scrim of watchBlockingDialogs(); nullptr while it watches nothing.
const DialogScrim* blockingDialogScrim();

} // namespace crate
