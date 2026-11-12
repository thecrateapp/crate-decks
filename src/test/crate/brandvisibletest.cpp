#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QWidget>
#include <functional>
#include <memory>

#include "control/controlobject.h"
#include "crate/appidentity.h"
#include "crate/brand/brandtext.h"
#include "crate/brand/brandtranslator.h"
#include "crate/crateservers.h"
#include "crate/ui/cratefirstrun.h"
#include "crate/ui/delegates/sidebardelegate.h"
#include "dialog/dlgabout.h"
#include "test/crate/offscreendecks.h"
#include "test/mixxxtest.h"

// Crate Decks shows nothing of Mixxx (plan "Own brand", BR.PR4). This test
// starts the real application offscreen with the CrateDecks skin and reads
// every text the user could see: the main window and its skin, the menus
// and their actions, every Preferences page, the first-start pages, About
// and the quit confirmation. The only "Mixxx" allowed are the credits in
// About (design D2), kept by the brand inventory, and file names.
//
// TEST_GAP: text that widgets paint themselves (waveforms, the skin's
// painted labels) and images are not read; the skin has no text in its XML
// that names Mixxx, and the brand tests check the images (BR.PR5).

namespace {

using crate::test::OffscreenDecks;
using crate::test::waitFor;

const QString kCrateDecks = QStringLiteral("CrateDecks");

/// A text the user can read and where it is.
struct Seen {
    QString where;
    QString text;
};

/// The credits that the inventory keeps, as expressions over plain text
/// ("%1" matches anything): what showsMixxx() does not count in About.
QList<QRegularExpression> keptCredits() {
    QList<QRegularExpression> credits;
    const crate::BrandInventory& inventory = crate::BrandInventory::instance();
    const QStringList sources = inventory.sources();
    for (const QString& source : sources) {
        if (inventory.string(source)->treatment != crate::BrandInventory::Treatment::Keep) {
            continue;
        }
        const QString plain = QTextDocumentFragment::fromHtml(source).toPlainText();
        // escape() turns "%1" into "\%1"
        QString pattern = QRegularExpression::escape(plain);
        static const QRegularExpression kFormatArgument(QStringLiteral("\\\\%\\d"));
        pattern.replace(kFormatArgument, QStringLiteral(".*?"));
        credits.append(QRegularExpression(pattern));
    }
    return credits;
}

class TextCollector {
  public:
    void add(const QString& where, const QString& text) {
        if (!text.trimmed().isEmpty()) {
            m_seen.append({where, text});
        }
    }

    /// Every text of `pRoot` and its children: window titles, tooltips,
    /// status tips, labels, buttons, fields, combo boxes, tabs, group boxes,
    /// text views, item views with their headers, and actions with their
    /// menus.
    /// Leaves `pWidget` out: what Crate Decks must keep.
    void exclude(QWidget* pWidget) {
        m_excluded.append(pWidget);
    }

    void addWidgetTree(QWidget* pRoot, const QString& where) {
        QList<QWidget*> widgets = pRoot->findChildren<QWidget*>();
        widgets.prepend(pRoot);
        for (QWidget* pWidget : std::as_const(widgets)) {
            if (m_excluded.contains(pWidget)) {
                continue;
            }
            const QString name = where + QLatin1Char('/') +
                    (pWidget->objectName().isEmpty()
                                    ? QString::fromLatin1(pWidget->metaObject()->className())
                                    : pWidget->objectName());
            if (pWidget->isWindow()) {
                add(name, pWidget->windowTitle());
            }
            add(name, pWidget->toolTip());
            add(name, pWidget->statusTip());
            add(name, pWidget->whatsThis());
            add(name, pWidget->accessibleName());
            if (auto* pLabel = qobject_cast<QLabel*>(pWidget)) {
                add(name, pLabel->text());
            } else if (auto* pButton = qobject_cast<QAbstractButton*>(pWidget)) {
                add(name, pButton->text());
            } else if (auto* pEdit = qobject_cast<QLineEdit*>(pWidget)) {
                add(name, pEdit->text());
                add(name, pEdit->placeholderText());
            } else if (auto* pCombo = qobject_cast<QComboBox*>(pWidget)) {
                for (int i = 0; i < pCombo->count(); ++i) {
                    add(name, pCombo->itemText(i));
                    add(name, pCombo->itemData(i, Qt::ToolTipRole).toString());
                }
            } else if (auto* pGroup = qobject_cast<QGroupBox*>(pWidget)) {
                add(name, pGroup->title());
            } else if (auto* pTabs = qobject_cast<QTabBar*>(pWidget)) {
                for (int i = 0; i < pTabs->count(); ++i) {
                    add(name, pTabs->tabText(i));
                    add(name, pTabs->tabToolTip(i));
                }
            } else if (auto* pText = qobject_cast<QTextEdit*>(pWidget)) {
                add(name, pText->toPlainText());
            } else if (auto* pView = qobject_cast<QAbstractItemView*>(pWidget)) {
                addItemView(pView, name);
            }
            const QList<QAction*> actions = pWidget->actions();
            for (QAction* pAction : actions) {
                addAction(pAction, name);
            }
        }
    }

    void addAction(QAction* pAction, const QString& where) {
        const QString name = where + QStringLiteral(" > ") + pAction->text();
        add(name, pAction->text());
        add(name, pAction->toolTip());
        add(name, pAction->statusTip());
        add(name, pAction->whatsThis());
        if (QMenu* pMenu = pAction->menu()) {
            // Menus fill themselves as they show, as Mixxx's do
            emit pMenu->aboutToShow();
            add(name, pMenu->title());
            const QList<QAction*> actions = pMenu->actions();
            for (QAction* pChild : actions) {
                addAction(pChild, name);
            }
        }
    }

    /// What showsMixxx() finds, but the kept credits.
    QStringList mixxx() const {
        static const QList<QRegularExpression> credits = keptCredits();
        QStringList found;
        for (const Seen& seen : m_seen) {
            QString text = Qt::mightBeRichText(seen.text)
                    ? QTextDocumentFragment::fromHtml(seen.text).toPlainText()
                    : seen.text;
            for (const QRegularExpression& credit : credits) {
                text.remove(credit);
            }
            if (crate::showsMixxx(text)) {
                found.append(seen.where + QStringLiteral(": ") + seen.text.left(300));
            }
        }
        found.removeDuplicates();
        return found;
    }

    int count() const {
        return m_seen.size();
    }

  private:
    void addItemView(QAbstractItemView* pView, const QString& where) {
        QAbstractItemModel* pModel = pView->model();
        if (!pModel) {
            return;
        }
        for (int column = 0; column < pModel->columnCount(); ++column) {
            add(where, pModel->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
            add(where, pModel->headerData(column, Qt::Horizontal, Qt::ToolTipRole).toString());
        }
        const auto* pSections =
                qobject_cast<crate::delegates::SidebarDelegate*>(
                        pView->itemDelegate());
        int budget = 2000;
        std::function<void(const QModelIndex&, int)> walk = [&](const QModelIndex& parent,
                                                                    int depth) {
            for (int row = 0; row < pModel->rowCount(parent) && budget > 0; ++row, --budget) {
                for (int column = 0; column < pModel->columnCount(parent); ++column) {
                    const QModelIndex index = pModel->index(row, column, parent);
                    add(where, index.data(Qt::DisplayRole).toString());
                    add(where, index.data(Qt::ToolTipRole).toString());
                    if (pSections && column == 0) {
                        add(where + QStringLiteral(" (section)"), pSections->sectionHeader(index));
                    }
                }
                const QModelIndex child = pModel->index(row, 0, parent);
                if (depth < 3 && pModel->hasChildren(child)) {
                    walk(child, depth + 1);
                }
            }
        };
        walk(QModelIndex(), 0);
    }

    QList<Seen> m_seen;
    QList<QWidget*> m_excluded;
};

/// Reads every dialog as it shows, before DialogRejecter answers it: event
/// filters installed later run first.
class ShownDialogs : public QObject {
  public:
    explicit ShownDialogs(TextCollector* pTexts)
            : m_pTexts(pTexts) {
    }
    QStringList titles;

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() == QEvent::Show) {
            if (auto* pDialog = qobject_cast<QDialog*>(pWatched)) {
                titles.append(pDialog->windowTitle());
                // The quit confirmation answers No: rejecting it would not
                if (auto* pBox = qobject_cast<QMessageBox*>(pDialog)) {
                    if (QAbstractButton* pNo = pBox->button(QMessageBox::No)) {
                        QTimer::singleShot(0, pNo, &QAbstractButton::click);
                    }
                }
                m_pTexts->addWidgetTree(pDialog,
                        QStringLiteral("dialog ") + pDialog->windowTitle());
            }
        }
        return QObject::eventFilter(pWatched, pEvent);
    }

  private:
    TextCollector* m_pTexts;
};

class CrateBrandVisibleTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pBrand = crate::installBrandTranslator(application());
    }
    void TearDown() override {
        delete m_pBrand;
    }

  private:
    crate::BrandTranslator* m_pBrand = nullptr;
};

QWidget* topLevelOf(const char* className) {
    const QWidgetList windows = QApplication::topLevelWidgets();
    for (QWidget* pWindow : windows) {
        if (pWindow->inherits(className)) {
            return pWindow;
        }
    }
    return nullptr;
}

TEST_F(CrateBrandVisibleTest, NoMixxxAnywhere) {
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    // The application's name, as main.cpp and appidentity.cpp give it, and
    // not the test binary's: the Crate page shows the audio cache's default
    // directory, which the OS names after the application
    const QString testName = QCoreApplication::applicationName();
    const QString testOrganization = QCoreApplication::organizationName();
    QCoreApplication::setApplicationName(QString::fromUtf8(crate::kApplicationName));
    QCoreApplication::setOrganizationName(QString::fromUtf8(crate::kOrganizationName));
    const auto restoreName = qScopeGuard([&] {
        QCoreApplication::setApplicationName(testName);
        QCoreApplication::setOrganizationName(testOrganization);
    });
    OffscreenDecks decks(profile.path() + QLatin1Char('/'));
    decks.writeProfile(kCrateDecks);
    TextCollector texts;
    ShownDialogs dialogs(&texts);
    decks.start(application(), [&dialogs] { application()->installEventFilter(&dialogs); });
    ASSERT_NE(nullptr, decks.skin());
    decks.mainWindow()->show();
    waitFor([] { return false; }, 300);

    // The main window, its skin and the menu bar with every menu
    texts.addWidgetTree(decks.mainWindow(), QStringLiteral("main window"));
    if (QMenuBar* pMenuBar = decks.mainWindow()->menuBar()) {
        texts.addWidgetTree(pMenuBar, QStringLiteral("menu bar"));
    }

    // Every Preferences page
    auto* pPreferences = qobject_cast<QDialog*>(topLevelOf("DlgPreferences"));
    ASSERT_NE(nullptr, pPreferences);
    pPreferences->show();
    auto* pTree = pPreferences->findChild<QTreeWidget*>(QStringLiteral("contentsTreeWidget"));
    ASSERT_NE(nullptr, pTree);
    int pages = 0;
    for (QTreeWidgetItemIterator it(pTree); *it; ++it) {
        pTree->setCurrentItem(*it);
        QCoreApplication::processEvents();
        texts.addWidgetTree(pPreferences, QStringLiteral("Preferences ") + (*it)->text(0));
        ++pages;
    }
    EXPECT_GT(pages, 15);
    pPreferences->hide();

    // About, but its License tab: Mixxx's copyright notice and the GPL, which
    // Crate Decks must keep (design D2)
    {
        DlgAbout about;
        if (QWidget* pLicense = about.findChild<QWidget*>(QStringLiteral("licenseText"))) {
            texts.exclude(pLicense);
        }
        texts.addWidgetTree(&about, QStringLiteral("About"));
    }

    // The first start, every page, without a sound card
    {
        crate::FirstRunServices services;
        services.outputs = [] { return QList<crate::FirstRunOutput>(); };
        std::unique_ptr<QDialog> pFirstRun(crate::createFirstRunDialog(
                crate::CrateServers::instance(), services, decks.mainWindow()));
        auto* pPages = pFirstRun->findChild<QStackedWidget*>(QStringLiteral("CrateFirstRunPages"));
        ASSERT_NE(nullptr, pPages);
        for (int i = 0; i < pPages->count(); ++i) {
            pPages->setCurrentIndex(i);
            texts.addWidgetTree(pFirstRun.get(), QStringLiteral("first start"));
        }
    }

    // The quit confirmation while a deck plays
    ControlObject* pPlay = ControlObject::getControl(ConfigKey(QStringLiteral("[Channel1]"),
            QStringLiteral("play")));
    ASSERT_NE(nullptr, pPlay);
    pPlay->forceSet(1.0);
    dialogs.titles.clear();
    EXPECT_FALSE(decks.mainWindow()->close()) << "The confirmation answers No";
    pPlay->forceSet(0.0);
    EXPECT_TRUE(dialogs.titles.contains(QStringLiteral("Confirm Exit")))
            << qPrintable(dialogs.titles.join(QStringLiteral(", ")));

    EXPECT_GT(texts.count(), 500);
    const QStringList mixxx = texts.mixxx();
    EXPECT_TRUE(mixxx.isEmpty()) << mixxx.size() << " texts show Mixxx:\n"
                                 << qPrintable(mixxx.join(QLatin1Char('\n')));
}

} // namespace
