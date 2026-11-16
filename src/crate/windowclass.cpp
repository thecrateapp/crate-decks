// The X11 WM_CLASS of Decks' windows (crate::applyWindowClass()). Apart from
// appidentity.cpp because Xlib's headers define macros (None, Bool, Status)
// that clash with Qt's names.

#include <QByteArray>
#include <QEvent>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QWindow>

#include "crate/appidentity.h"

#if defined(__X11__) && QT_CONFIG(xcb)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace crate {

namespace {

#if defined(__X11__) && QT_CONFIG(xcb)
/// Writes the WM_CLASS of windowClassProperty() on every window as Qt
/// creates its native window, which Qt has just given its own WM_CLASS
/// and has not mapped yet.
class WindowClassFilter : public QObject {
  public:
    WindowClassFilter(Display* pDisplay, QObject* pParent)
            : QObject(pParent),
              m_pDisplay(pDisplay),
              m_name(QByteArray(kDesktopFileName)) {
    }

    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() == QEvent::PlatformSurface && pWatched->isWindowType() &&
                static_cast<QPlatformSurfaceEvent*>(pEvent)->surfaceEventType() ==
                        QPlatformSurfaceEvent::SurfaceCreated) {
            const auto* pWindow = static_cast<QWindow*>(pWatched);
            XClassHint hint;
            hint.res_name = m_name.data();
            hint.res_class = m_name.data();
            XSetClassHint(m_pDisplay, static_cast<Window>(pWindow->winId()), &hint);
            XFlush(m_pDisplay);
        }
        return QObject::eventFilter(pWatched, pEvent);
    }

  private:
    Display* m_pDisplay;
    QByteArray m_name;
};
#endif

} // namespace

QByteArray windowClassProperty() {
    const QByteArray name(kDesktopFileName);
    return name + '\0' + name + '\0';
}

bool applyWindowClass(QGuiApplication* pApplication) {
#if defined(__X11__) && QT_CONFIG(xcb)
    if (!pApplication || QGuiApplication::platformName() != QLatin1String("xcb")) {
        return false;
    }
    auto* pX11 = pApplication->nativeInterface<QNativeInterface::QX11Application>();
    Display* pDisplay = pX11 ? pX11->display() : nullptr;
    if (!pDisplay) {
        return false;
    }
    pApplication->installEventFilter(new WindowClassFilter(pDisplay, pApplication));
    return true;
#else
    Q_UNUSED(pApplication);
    return false;
#endif
}

} // namespace crate
