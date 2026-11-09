#include "crate/ui/dialogs/preferenceschrome.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "crate/appidentity.h"
#include "crate/ui/delegates/delegatestyle.h"
#include "crate/ui/delegates/sidebardelegate.h"
#include "crate/ui/dialogs/dialogchrome.h"
#include "crate/ui/dialogs/dialogstyle.h"

namespace crate {

namespace {

QString translate(const char* text) {
    return QCoreApplication::translate("crate::Preferences", text);
}

/// A button of the foot that stands for one of Mixxx's: same text, enabled
/// and shown when it is, and a click clicks Mixxx's button, so that
/// DlgPreferences sees the role it expects.
class ButtonMirror : public QObject {
  public:
    ButtonMirror(QAbstractButton* pOriginal, QPushButton* pMirror)
            : QObject(pMirror),
              m_pOriginal(pOriginal),
              m_pMirror(pMirror) {
        pOriginal->installEventFilter(this);
        QObject::connect(pMirror, &QPushButton::clicked, pOriginal, &QAbstractButton::click);
        sync();
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        switch (pEvent->type()) {
        case QEvent::EnabledChange:
        case QEvent::ShowToParent:
        case QEvent::HideToParent:
        case QEvent::Show:
        case QEvent::Hide:
            sync();
            break;
        default:
            break;
        }
        return QObject::eventFilter(pWatched, pEvent);
    }

  private:
    void sync() {
        // The mnemonic without its underline, which the mockups do not draw
        const QString text = m_pOriginal->text();
        m_pMirror->setText(QString(text).remove(QLatin1Char('&')));
        m_pMirror->setShortcut(QKeySequence::mnemonic(text));
        m_pMirror->setEnabled(m_pOriginal->isEnabled());
        m_pMirror->setVisible(!m_pOriginal->isHidden());
    }

    QAbstractButton* m_pOriginal;
    QPushButton* m_pMirror;
};

/// The mockups' page padding on a page of the stack (a QScrollArea around
/// the page). The Crate page starts with its intro right under the title;
/// Mixxx's pages start with a section after the intro's gap.
void padPage(QWidget* pScrollArea, const DialogMetrics& metrics) {
    auto* pArea = qobject_cast<QScrollArea*>(pScrollArea);
    if (!pArea || !pArea->widget() || !pArea->widget()->layout()) {
        return;
    }
    QWidget* pPage = pArea->widget();
    const bool cratePage = pPage->inherits("crate::DlgPrefCrate");
    pPage->layout()->setContentsMargins(metrics.px("prefsPagePaddingX"),
            cratePage ? 0 : metrics.px("prefsIntroGap") - metrics.px("prefsTitleGap"),
            metrics.px("prefsPagePaddingX"),
            metrics.px("prefsPagePaddingY"));
    pArea->setFrameShape(QFrame::NoFrame);
}

} // namespace

void stylePreferences(QDialog* pDialog) {
    const DialogMetrics& metrics = DialogMetrics::installed();
    auto* pTree = pDialog ? pDialog->findChild<QTreeWidget*>(QStringLiteral("contentsTreeWidget"))
                          : nullptr;
    auto* pPages = pDialog ? pDialog->findChild<QStackedWidget*>(QStringLiteral("pagesWidget"))
                           : nullptr;
    auto* pButtons = pDialog ? pDialog->findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"))
                             : nullptr;
    if (!metrics.isValid() || !pTree || !pPages || !pButtons || !pDialog->layout() ||
            !pPages->parentWidget() ||
            !qobject_cast<QVBoxLayout*>(pPages->parentWidget()->layout())) {
        return;
    }
    pDialog->layout()->setContentsMargins(0, 0, 0, 0);
    pDialog->layout()->setSpacing(0);
    auto* pChrome = new DialogChrome(pDialog,
            {QString::fromUtf8(kApplicationName), translate("Preferences")});
    if (auto* pSplitter = pDialog->findChild<QSplitter*>(QStringLiteral("splitter"))) {
        pSplitter->setHandleWidth(0);
    }

    // The navigation: the admin's, as the library sidebar (delegates/)
    delegates::DelegateStyle style = delegates::DelegateStyle::fromVariables(metrics.variables());
    style.rowHeight = metrics.px("prefsNavRowHeight");
    auto* pDelegate = new delegates::SidebarDelegate(style, pTree);
    pDelegate->setLogo(kPreferencesLogoRole,
            designPath() + QStringLiteral("crate-logo.svg"),
            QSize(metrics.px("prefsNavLogoWidth"), metrics.px("prefsNavLogoHeight")));
    pTree->setItemDelegate(pDelegate);
    pTree->setIndentation(0);
    pTree->setRootIsDecorated(false);
    pTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pTree->setFrameShape(QFrame::NoFrame);
    pTree->setMouseTracking(true);
    pTree->viewport()->setAttribute(Qt::WA_Hover);

    // Header over the pages: "PREFERENCES" and the page's title
    QWidget* pRight = pPages->parentWidget();
    auto* pRightLayout = static_cast<QVBoxLayout*>(pRight->layout());
    pRightLayout->setContentsMargins(0, 0, 0, 0);
    pRightLayout->setSpacing(0);
    auto* pHeader = new QFrame(pRight);
    pHeader->setObjectName(QStringLiteral("CratePrefsHeader"));
    auto* pHeaderLayout = new QVBoxLayout(pHeader);
    pHeaderLayout->setContentsMargins(metrics.px("prefsPagePaddingX"),
            metrics.px("prefsPagePaddingY"),
            metrics.px("prefsPagePaddingX"),
            metrics.px("prefsTitleGap"));
    pHeaderLayout->setSpacing(0);
    pHeaderLayout->addWidget(
            monoLabel(translate("Preferences"), QStringLiteral("MonoPrefsEyebrow"), pHeader, true));
    auto* pTitle = new QLabel(pHeader);
    pTitle->setObjectName(QStringLiteral("CratePrefsTitle"));
    pTitle->setTextFormat(Qt::PlainText);
    setDisplayAxes(pTitle);
    pHeaderLayout->addWidget(pTitle);
    pRightLayout->insertWidget(0, pHeader);
    const auto showTitle = [pTitle](QTreeWidgetItem* pCurrent) {
        pTitle->setText(pCurrent ? pCurrent->text(0) : QString());
    };
    QObject::connect(pTree, &QTreeWidget::currentItemChanged, pHeader, showTitle);
    showTitle(pTree->currentItem());

    for (int i = 0; i < pPages->count(); ++i) {
        padPage(pPages->widget(i), metrics);
    }
    // Controllers add their pages later
    QObject::connect(pPages, &QStackedWidget::currentChanged, pPages, [pPages, &metrics](int) {
        padPage(pPages->currentWidget(), metrics);
    });

    // The foot: Mixxx's buttons stay in their box, hidden, and the foot
    // shows them in the mockups' order
    QHBoxLayout* pFoot = pChrome->footLayout();
    const auto mirror = [pButtons, pDialog](QDialogButtonBox::StandardButton which,
                                const char* variant) -> QPushButton* {
        QPushButton* pOriginal = pButtons->button(which);
        if (!pOriginal) {
            return nullptr;
        }
        auto* pMirror = new QPushButton(pDialog);
        pMirror->setObjectName(QStringLiteral("CratePrefsButton"));
        setButtonVariant(pMirror, variant, true);
        new ButtonMirror(pOriginal, pMirror);
        return pMirror;
    };
    for (const auto& [which, variant] :
            {std::pair{QDialogButtonBox::RestoreDefaults, "ghost"},
                    std::pair{QDialogButtonBox::Help, "ghost"}}) {
        if (QPushButton* pMirror = mirror(which, variant)) {
            pFoot->addWidget(pMirror);
        }
    }
    pFoot->addStretch(1);
    // Mixxx's warning about pages with errors, next to the actions
    for (const char* name : {"labelWarningIcon", "labelWarning"}) {
        if (auto* pLabel = pDialog->findChild<QLabel*>(QLatin1String(name))) {
            if (pLabel->parentWidget() && pLabel->parentWidget()->layout()) {
                pLabel->parentWidget()->layout()->removeWidget(pLabel);
            }
            const bool hidden = pLabel->isHidden();
            pFoot->addWidget(pLabel);
            pLabel->setHidden(hidden);
        }
    }
    for (const auto& [which, variant] : {std::pair{QDialogButtonBox::Cancel, "ghost"},
                 std::pair{QDialogButtonBox::Apply, "ghost"},
                 std::pair{QDialogButtonBox::Ok, "primary"}}) {
        if (QPushButton* pMirror = mirror(which, variant)) {
            pFoot->addWidget(pMirror);
            if (which == QDialogButtonBox::Ok) {
                pMirror->setDefault(true);
            }
        }
    }
    pButtons->hide();
}

} // namespace crate
