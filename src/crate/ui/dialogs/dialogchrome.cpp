#include "crate/ui/dialogs/dialogchrome.h"

#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPushButton>
#include <QSizeGrip>
#include <QWindow>

#include "crate/ui/dialogs/dialogstyle.h"
#include "moc_dialogchrome.cpp"

namespace crate {

namespace {

/// A corner that resizes the window and paints nothing.
class Grip : public QSizeGrip {
  public:
    using QSizeGrip::QSizeGrip;

  protected:
    void paintEvent(QPaintEvent* /*pEvent*/) override {
    }
};

} // namespace

DialogChrome::DialogChrome(QDialog* pDialog, const QStringList& titleParts)
        : QObject(pDialog),
          m_pDialog(pDialog),
          m_pFrame(new QFrame(pDialog)),
          m_pGrip(new Grip(pDialog)) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    pDialog->setWindowFlag(Qt::FramelessWindowHint, true);
    pDialog->setAttribute(Qt::WA_TranslucentBackground);
    pDialog->setProperty("crateChrome", true);

    m_pFrame->setObjectName(QStringLiteral("CrateDialogFrame"));
    m_pFrame->lower();
    m_pGrip->setObjectName(QStringLiteral("CrateDialogGrip"));

    if (!titleParts.isEmpty()) {
        m_pTitle = new QFrame(pDialog);
        m_pTitle->setObjectName(QStringLiteral("CrateDialogTitle"));
        auto* pLayout = new QHBoxLayout(m_pTitle);
        pLayout->setContentsMargins(metrics.px("dialogPaddingX"),
                0,
                metrics.px("dialogPaddingX"),
                0);
        pLayout->setSpacing(metrics.px("dialogTitleGap"));
        auto* pLogo = new QLabel(m_pTitle);
        pLogo->setObjectName(QStringLiteral("CrateDialogLogo"));
        pLogo->setPixmap(logoPixmap(metrics.px("dialogLogoWidth"),
                metrics.px("dialogLogoHeight"),
                pDialog->devicePixelRatioF()));
        pLayout->addWidget(pLogo);
        // MonoLabel's meta parts: " · " between them, as decks.js monoLabel()
        pLayout->addWidget(monoLabel(titleParts.join(QStringLiteral(" · ")),
                QStringLiteral("MonoDialogTitle"),
                m_pTitle));
        pLayout->addStretch(1);
        auto* pClose = new QPushButton(m_pTitle);
        pClose->setObjectName(QStringLiteral("CrateDialogClose"));
        pClose->setFocusPolicy(Qt::NoFocus);
        pClose->setAutoDefault(false);
        pClose->setToolTip(tr("Close"));
        pLayout->addWidget(pClose);
        connect(pClose, &QPushButton::clicked, pDialog, &QDialog::close);
        m_pTitle->installEventFilter(this);
    }

    if (QLayout* pLayout = pDialog->layout()) {
        QMargins margins = pLayout->contentsMargins();
        margins.setTop(m_pTitle ? metrics.px("dialogTitleHeight") : margins.top());
        pLayout->setContentsMargins(margins);
    }
    pDialog->installEventFilter(this);
    layoutChrome();
}

QHBoxLayout* DialogChrome::footLayout() {
    if (m_pFootLayout) {
        return m_pFootLayout;
    }
    const DialogMetrics& metrics = DialogMetrics::installed();
    m_pFoot = new QFrame(m_pDialog);
    m_pFoot->setObjectName(QStringLiteral("CrateDialogFoot"));
    m_pFootLayout = new QHBoxLayout(m_pFoot);
    // The 1 px rule on top is inside the foot
    const int rule = metrics.px("strokeHairline");
    m_pFootLayout->setContentsMargins(metrics.px("dialogPaddingX"),
            metrics.px("dialogFootPaddingY") + rule,
            metrics.px("dialogPaddingX"),
            metrics.px("dialogFootPaddingY"));
    m_pFootLayout->setSpacing(metrics.px("dialogButtonGap"));
    if (QLayout* pLayout = m_pDialog->layout()) {
        QMargins margins = pLayout->contentsMargins();
        // A 40 px action and its padding: the foot's height
        margins.setBottom(
                metrics.px("dialogFootPaddingY") * 2 + rule + metrics.px("heightControl"));
        pLayout->setContentsMargins(margins);
    }
    m_pFoot->show();
    m_pGrip->raise();
    layoutChrome();
    return m_pFootLayout;
}

bool DialogChrome::eventFilter(QObject* pWatched, QEvent* pEvent) {
    if (pWatched == m_pDialog) {
        if (pEvent->type() == QEvent::Resize || pEvent->type() == QEvent::Show) {
            layoutChrome();
        }
    } else if (pWatched == m_pTitle && pEvent->type() == QEvent::MouseButtonPress) {
        const auto* pMouse = static_cast<QMouseEvent*>(pEvent);
        if (pMouse->button() == Qt::LeftButton && m_pDialog && m_pDialog->windowHandle()) {
            m_pDialog->windowHandle()->startSystemMove();
            return true;
        }
    }
    return QObject::eventFilter(pWatched, pEvent);
}

void DialogChrome::layoutChrome() {
    if (!m_pDialog) {
        return;
    }
    const QRect rect = m_pDialog->rect();
    m_pFrame->setGeometry(rect);
    if (m_pTitle) {
        m_pTitle->setGeometry(0,
                0,
                rect.width(),
                DialogMetrics::installed().px("dialogTitleHeight"));
        m_pTitle->raise();
    }
    if (m_pFoot) {
        const int height = m_pDialog->layout() ? m_pDialog->layout()->contentsMargins().bottom()
                                               : m_pFoot->sizeHint().height();
        m_pFoot->setGeometry(0, rect.height() - height, rect.width(), height);
        m_pFoot->raise();
    }
    const QSize grip = m_pGrip->sizeHint();
    m_pGrip->setGeometry(rect.width() - grip.width(),
            rect.height() - grip.height(),
            grip.width(),
            grip.height());
    m_pGrip->raise();
}

} // namespace crate
