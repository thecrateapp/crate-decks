#include "crate/ui/dialogs/dialogscrim.h"

#include <QCoreApplication>
#include <QDialog>
#include <QEvent>
#include <QGuiApplication>
#include <QList>
#include <QPainter>
#include <QRasterWindow>
#include <QSurfaceFormat>
#include <QWidget>
#include <QWindow>

#include "crate/ui/dialogs/dialogstyle.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::DialogScrim");

/// The scrim itself: a translucent window filled with one colour.
class ScrimWindow : public QRasterWindow {
  public:
    explicit ScrimWindow(const QColor& colour)
            : m_colour(colour) {
        setObjectName(QStringLiteral("CrateDialogScrim"));
        QSurfaceFormat format = requestedFormat();
        format.setAlphaBufferSize(8);
        setFormat(format);
    }

  protected:
    void paintEvent(QPaintEvent* pEvent) override {
        Q_UNUSED(pEvent);
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(QRect(QPoint(0, 0), size()), m_colour);
    }

  private:
    const QColor m_colour;
};

/// Keeps the dialogs that take the scrim while they show, and the scrim
/// active while there is one and the main window is on screen. As an
/// application event filter it sees each dialog's Show before its window
/// maps, so the scrim shows first and the dialog above it.
class BlockingDialogs : public QObject {
  public:
    explicit BlockingDialogs(QWidget* pMainWindow)
            : QObject(QCoreApplication::instance()),
              m_pMainWindow(pMainWindow),
              m_scrim(pMainWindow,
                      DialogScrim::kindForPlatform(QGuiApplication::platformName())) {
        QCoreApplication::instance()->installEventFilter(this);
    }

    QWidget* mainWindow() const {
        return m_pMainWindow;
    }
    const DialogScrim& scrim() const {
        return m_scrim;
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        const QEvent::Type type = pEvent->type();
        if (type != QEvent::Show && type != QEvent::Hide && type != QEvent::WindowStateChange) {
            return QObject::eventFilter(pWatched, pEvent);
        }
        if (pWatched == m_pMainWindow) {
            update(true);
        } else if (auto* pDialog = qobject_cast<QDialog*>(pWatched)) {
            if (type == QEvent::Show && !m_dialogs.contains(pDialog) &&
                    dialogTakesScrim(pDialog, m_pMainWindow)) {
                m_dialogs.append(pDialog);
                // A dialog deleted while it shows may not hide first
                connect(pDialog, &QObject::destroyed, this, [this](QObject* pObject) {
                    m_dialogs.removeIf([pObject](const QPointer<QDialog>& pDialog) {
                        return !pDialog || pDialog.data() == pObject;
                    });
                    update(false);
                });
                update(false);
            } else if (type == QEvent::Hide && m_dialogs.removeAll(pDialog) > 0) {
                disconnect(pDialog, &QObject::destroyed, this, nullptr);
                update(false);
            }
        }
        return QObject::eventFilter(pWatched, pEvent);
    }

  private:
    /// `raiseDialogs` when the main window came back under open dialogs:
    /// they go back above the new scrim.
    void update(bool raiseDialogs) {
        m_dialogs.removeAll(nullptr);
        const bool active = !m_dialogs.isEmpty() && m_pMainWindow &&
                m_pMainWindow->isVisible() && !m_pMainWindow->isMinimized();
        if (active == m_scrim.isActive()) {
            return;
        }
        m_scrim.setActive(active);
        if (active && raiseDialogs) {
            for (const QPointer<QDialog>& pDialog : std::as_const(m_dialogs)) {
                if (pDialog->isVisible()) {
                    pDialog->raise();
                }
            }
        }
    }

    const QPointer<QWidget> m_pMainWindow;
    DialogScrim m_scrim;
    QList<QPointer<QDialog>> m_dialogs;
};

/// It belongs to the application, which deletes it on exit.
QPointer<BlockingDialogs> s_pBlockingDialogs;

} // namespace

DialogScrim::Kind DialogScrim::kindForPlatform(const QString& platformName) {
    return platformName.startsWith(QLatin1String("wayland")) ? Kind::ChildWindow : Kind::TopLevel;
}

DialogScrim::DialogScrim(QWidget* pMainWindow, Kind kind, QObject* pParent)
        : QObject(pParent),
          m_pMainWindow(pMainWindow),
          m_kind(kind),
          m_colour(DialogMetrics::installed().colour("CrateScrim")) {
    if (!m_colour.isValid()) {
        kLogger.warning() << "No CrateScrim colour in generated/vars.xml: no scrim";
    }
    if (pMainWindow) {
        pMainWindow->installEventFilter(this);
    }
}

DialogScrim::~DialogScrim() {
    setActive(false);
}

void DialogScrim::setActive(bool active) {
    if (active == isActive()) {
        return;
    }
    if (!active) {
        // A child window also belongs to the main window's, which may have
        // deleted it already
        delete m_pWindow.data();
        return;
    }
    if (!m_pMainWindow || !m_colour.isValid()) {
        return;
    }
    QWindow* pMainHandle = m_pMainWindow->windowHandle();
    if (m_kind == Kind::ChildWindow && !pMainHandle) {
        return;
    }
    auto* pWindow = new ScrimWindow(m_colour);
    // Clicks go to the main window, which the modal dialog blocks
    pWindow->setFlag(Qt::WindowTransparentForInput);
    pWindow->setFlag(Qt::WindowDoesNotAcceptFocus);
    if (m_kind == Kind::ChildWindow) {
        pWindow->setParent(pMainHandle);
    } else {
        pWindow->setFlags(pWindow->flags() | Qt::Tool | Qt::FramelessWindowHint |
                Qt::NoDropShadowWindowHint);
        pWindow->setTransientParent(pMainHandle);
    }
    m_pWindow = pWindow;
    place();
    pWindow->show();
}

bool DialogScrim::isActive() const {
    return m_pWindow;
}

QWindow* DialogScrim::window() const {
    return m_pWindow;
}

bool DialogScrim::eventFilter(QObject* pWatched, QEvent* pEvent) {
    if (pWatched == m_pMainWindow &&
            (pEvent->type() == QEvent::Move || pEvent->type() == QEvent::Resize)) {
        place();
    }
    return QObject::eventFilter(pWatched, pEvent);
}

void DialogScrim::place() {
    if (!m_pWindow || !m_pMainWindow) {
        return;
    }
    if (m_kind == Kind::ChildWindow) {
        // Where the main window's contents are; it moves with them
        m_pWindow->setGeometry(QRect(QPoint(0, 0), m_pMainWindow->size()));
    } else {
        m_pWindow->setGeometry(m_pMainWindow->frameGeometry());
    }
}

bool dialogTakesScrim(const QDialog* pDialog, const QWidget* pMainWindow) {
    if (!pDialog || !pMainWindow || !pDialog->isWindow() ||
            pDialog->testAttribute(Qt::WA_DontShowOnScreen) ||
            pDialog->inherits("DlgPreferences")) {
        return false;
    }
    switch (pDialog->windowModality()) {
    case Qt::ApplicationModal:
        return true;
    case Qt::WindowModal:
        // It blocks the windows it belongs to
        for (const QWidget* pParent = pDialog->parentWidget(); pParent;
                pParent = pParent->parentWidget()) {
            if (pParent == pMainWindow) {
                return true;
            }
        }
        return false;
    case Qt::NonModal:
        return false;
    }
    return false;
}

void watchBlockingDialogs(QWidget* pMainWindow) {
    if (s_pBlockingDialogs && s_pBlockingDialogs->mainWindow() == pMainWindow) {
        return;
    }
    delete s_pBlockingDialogs.data();
    s_pBlockingDialogs = pMainWindow && QCoreApplication::instance()
            ? new BlockingDialogs(pMainWindow)
            : nullptr;
}

const DialogScrim* blockingDialogScrim() {
    return s_pBlockingDialogs ? &s_pBlockingDialogs->scrim() : nullptr;
}

} // namespace crate
