#include "crate/ui/skin/librarystyle.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "crate/ui/delegates/camelotdelegate.h"
#include "crate/ui/delegates/celldelegate.h"
#include "crate/ui/delegates/delegatestyle.h"
#include "crate/ui/delegates/sidebardelegate.h"
#include "crate/ui/delegates/statedelegates.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/library_prefs.h"
#include "library/sidebarmodel.h"
#include "library/trackmodel.h"
#include "mixer/playerinfo.h"
#include "track/track.h"
#include "util/logger.h"
#include "widget/wlabel.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wtracktableview.h"
#include "widget/wtracktableviewheader.h"

namespace crate {

namespace {

using namespace crate::delegates;

const mixxx::Logger kLogger("crate::library");

const ConfigKey kRowHeightKey(QStringLiteral("[Library]"), QStringLiteral("RowHeight"));
constexpr int kDecks = 4;
/// Decimals of the BPM in the mockups' table and the decks ("124.00")
constexpr int kBpmDecimals = 2;

QString iconPath(const QString& skinPath, const QString& solarName) {
    return QDir(skinPath).filePath(
            QStringLiteral("generated/icons/%1_idle.svg").arg(solarName));
}

/// The Solar icons of the mockups' sidebar for Mixxx's features, by the
/// icon name each feature gives (LibraryFeature::iconName()). The external
/// libraries share one.
QHash<QString, QString> sidebarIcons(const QString& skinPath) {
    const QList<std::pair<QString, QString>> icons = {
            {QStringLiteral("tracks"), QStringLiteral("music-library")},
            {QStringLiteral("autodj"), QStringLiteral("shuffle")},
            {QStringLiteral("playlist"), QStringLiteral("playlist-2")},
            {QStringLiteral("crates"), QStringLiteral("inbox-archive")},
            {QStringLiteral("computer"), QStringLiteral("folder")},
            {QStringLiteral("recordings"), QStringLiteral("record-circle")},
            {QStringLiteral("history"), QStringLiteral("undo-left")},
            {QStringLiteral("prepare"), QStringLiteral("soundwave")},
            {QStringLiteral("itunes"), QStringLiteral("music-library-2")},
            {QStringLiteral("rhythmbox"), QStringLiteral("music-library-2")},
            {QStringLiteral("banshee"), QStringLiteral("music-library-2")},
            {QStringLiteral("traktor"), QStringLiteral("music-library-2")},
            {QStringLiteral("rekordbox"), QStringLiteral("music-library-2")},
            {QStringLiteral("serato"), QStringLiteral("music-library-2")},
            // The Crate section (crate::library::CrateServerFeature)
            {QStringLiteral("crate-server"), QStringLiteral("server")},
            {QStringLiteral("crate-library"), QStringLiteral("music-library")},
            {QStringLiteral("crate-playlists"), QStringLiteral("playlist-2")},
            {QStringLiteral("crate-crates"), QStringLiteral("inbox-archive")},
            {QStringLiteral("crate-genres"), QStringLiteral("tag")},
            {QStringLiteral("crate-moods"), QStringLiteral("pulse")},
            {QStringLiteral("crate-recent"), QStringLiteral("clock-circle")},
            {QStringLiteral("crate-likes"), QStringLiteral("heart")},
            {QStringLiteral("crate-cache"), QStringLiteral("archive-down")},
    };
    QHash<QString, QString> paths;
    for (const auto& [iconName, solarName] : icons) {
        paths.insert(iconName, iconPath(skinPath, solarName));
    }
    return paths;
}

/// The letters of the decks that have each track loaded, from PlayerInfo.
class LoadedDecks : public QObject {
  public:
    explicit LoadedDecks(QObject* pParent)
            : QObject(pParent) {
        PlayerInfo& playerInfo = PlayerInfo::instance();
        for (int deck = 1; deck <= kDecks; ++deck) {
            const QString group = QStringLiteral("[Channel%1]").arg(deck);
            setTrack(group, playerInfo.getTrackInfo(group));
        }
        connect(&playerInfo,
                &PlayerInfo::trackChanged,
                this,
                [this](const QString& group, TrackPointer pNewTrack, TrackPointer) {
                    setTrack(group, pNewTrack);
                });
    }

    QString deckOf(TrackId trackId) const {
        return trackId.isValid() ? m_letters.value(trackId) : QString();
    }

    /// Widgets to repaint when a deck loads or ejects a track.
    void addWidget(QWidget* pWidget) {
        m_widgets.append(pWidget);
    }

  private:
    void setTrack(const QString& group, const TrackPointer& pTrack) {
        if (!group.startsWith(QLatin1String("[Channel")) || group.size() != 10) {
            return;
        }
        const int deck = group.at(8).digitValue();
        if (deck < 1 || deck > kDecks) {
            return;
        }
        const QString letter(QChar(u'A' + deck - 1));
        for (auto it = m_letters.begin(); it != m_letters.end();) {
            it = it.value() == letter ? m_letters.erase(it) : std::next(it);
        }
        if (pTrack && pTrack->getId().isValid()) {
            m_letters.insert(pTrack->getId(), letter);
        }
        for (const QPointer<QWidget>& pWidget : std::as_const(m_widgets)) {
            if (pWidget) {
                pWidget->update();
            }
        }
    }

    QHash<TrackId, QString> m_letters;
    QList<QPointer<QWidget>> m_widgets;
};

/// How the CrateDecks skin shows a column of Mixxx's track models, by the
/// column's field name.
enum class ColumnKind {
    Keep,
    Title,
    Name,
    Meta,
    Bpm,
    Key,
    /// Crate's columns (plan F3.C-PR2), which only Crate's track models have
    Profile,
    Cache,
};

// The icon name of crate::library::CrateServerFeature, whose root is a
// section header
const QString kCrateSectionIcon = QStringLiteral("crateserver");

// The field names of Crate's columns in crate::library::CrateServerTrackModel
const QString kCrateEnergyField = QStringLiteral("crate_energy");
const QString kCrateProfileField = QStringLiteral("crate_profile");
const QString kCrateCacheField = QStringLiteral("crate_cache");

ColumnKind columnKind(const QString& field) {
    if (field == LIBRARYTABLE_TITLE) {
        return ColumnKind::Title;
    }
    if (field == LIBRARYTABLE_BPM) {
        return ColumnKind::Bpm;
    }
    if (field == LIBRARYTABLE_KEY) {
        return ColumnKind::Key;
    }
    if (field == kCrateProfileField) {
        return ColumnKind::Profile;
    }
    if (field == kCrateCacheField) {
        return ColumnKind::Cache;
    }
    if (field == LIBRARYTABLE_DURATION || field == LIBRARYTABLE_YEAR ||
            field == LIBRARYTABLE_TRACKNUMBER || field == LIBRARYTABLE_BITRATE ||
            field == LIBRARYTABLE_SAMPLERATE || field == LIBRARYTABLE_REPLAYGAIN ||
            field == LIBRARYTABLE_CHANNELS || field == LIBRARYTABLE_DATETIMEADDED ||
            field == LIBRARYTABLE_LAST_PLAYED_AT || field == PLAYLISTTRACKSTABLE_POSITION ||
            field == PLAYLISTTRACKSTABLE_DATETIMEADDED || field == kCrateEnergyField) {
        return ColumnKind::Meta;
    }
    if (field == LIBRARYTABLE_ARTIST || field == LIBRARYTABLE_ALBUM ||
            field == LIBRARYTABLE_ALBUMARTIST || field == LIBRARYTABLE_GENRE ||
            field == LIBRARYTABLE_COMPOSER || field == LIBRARYTABLE_GROUPING ||
            field == LIBRARYTABLE_FILETYPE) {
        return ColumnKind::Name;
    }
    return ColumnKind::Keep;
}

/// Whether `pDelegate` is one of Mixxx's text delegates, which the skin's
/// own replace. Its other delegates (rating, colour, cover art, preview,
/// location, comments, played) keep their editors and stay.
bool isMixxxTextDelegate(const QAbstractItemDelegate* pDelegate) {
    if (!pDelegate) {
        return true;
    }
    const QLatin1String className(pDelegate->metaObject()->className());
    return className == QLatin1String("DefaultDelegate") ||
            className == QLatin1String("BPMDelegate") ||
            className == QLatin1String("KeyDelegate");
}

/// The columns of the mockups' track list, in their order: the playlist
/// position where there is one, title, artist, BPM, key, duration, energy,
/// genre, profile and cache. Energy, profile and cache are Crate's (plan
/// F3.C-PR2): Mixxx's own track models do not have them. The like column
/// waits for F4.
const QStringList& defaultColumns() {
    static const QStringList columns = {
            PLAYLISTTRACKSTABLE_POSITION,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_DURATION,
            kCrateEnergyField,
            LIBRARYTABLE_GENRE,
            kCrateProfileField,
            kCrateCacheField,
    };
    return columns;
}

/// The widths of the mockups' columns (decks.js trackTable).
struct ColumnWidths {
    int bpm = 0;
    int key = 0;
    int time = 0;
    int energy = 0;
    int profile = 0;
    int cache = 0;
    double artist = 0;
    double genre = 0;

    static ColumnWidths fromVariables(const SkinVariables& variables) {
        const auto number = [&variables](const QString& name) {
            return variables.value(name).toDouble();
        };
        ColumnWidths widths;
        widths.bpm = static_cast<int>(number(QStringLiteral("CrateLibColumnBpm")));
        widths.key = static_cast<int>(number(QStringLiteral("CrateLibColumnKey")));
        widths.time = static_cast<int>(number(QStringLiteral("CrateLibColumnTime")));
        widths.energy = static_cast<int>(number(QStringLiteral("CrateLibColumnEnergy")));
        widths.profile = static_cast<int>(number(QStringLiteral("CrateLibColumnProfile")));
        widths.cache = static_cast<int>(number(QStringLiteral("CrateLibColumnCache")));
        widths.artist = number(QStringLiteral("CrateLibColumnArtist"));
        widths.genre = number(QStringLiteral("CrateLibColumnGenre"));
        return widths;
    }
};

/// The 34 px column on the left of a track table (decks.js trackTable's
/// first column): the letter of the deck that has the row's track loaded,
/// in the deck's colour, and the selected row's 2 px accentHi bar. It is the
/// table's vertical header, which Mixxx hides, so the columns and their
/// headers move right with it.
class DeckMarkHeader : public QHeaderView {
  public:
    using DeckOfRow = std::function<QString(int)>;
    using HoveredRow = std::function<int()>;

    DeckMarkHeader(const DelegateStyle& style,
            DeckOfRow deckOfRow,
            HoveredRow hoveredRow,
            QWidget* pParent)
            : QHeaderView(Qt::Vertical, pParent),
              m_style(style),
              m_deckOfRow(std::move(deckOfRow)),
              m_hoveredRow(std::move(hoveredRow)) {
        setObjectName(QStringLiteral("CrateDeckMarks"));
        setSectionsClickable(true);
        setHighlightSections(false);
        setFixedWidth(m_style.deckMarkWidth);
    }

    QSize sizeHint() const override {
        return QSize(m_style.deckMarkWidth, QHeaderView::sizeHint().height());
    }

  protected:
    void paintSection(QPainter* pPainter, const QRect& rect, int row) const override {
        pPainter->save();
        pPainter->fillRect(rect, m_style.background);
        const bool selected = selectionModel() &&
                selectionModel()->isRowSelected(row, QModelIndex());
        const QRect content = rect.adjusted(0, 0, 0, -m_style.rule);
        if (selected) {
            pPainter->fillRect(content, m_style.rowSelected);
            pPainter->fillRect(
                    QRect(content.left(), content.top(), m_style.activeBar, content.height()),
                    m_style.accentHi);
        } else if (m_hoveredRow() == row) {
            pPainter->fillRect(content, m_style.rowHover);
        }
        pPainter->fillRect(
                QRect(rect.left(), rect.bottom() - m_style.rule + 1, rect.width(), m_style.rule),
                m_style.borderSubtle);
        const QString letter = m_deckOfRow(row);
        if (!letter.isEmpty()) {
            paintText(pPainter,
                    content.adjusted(m_style.cellPadding, 0, 0, 0),
                    letter,
                    m_style.monoFont(m_style.fontSizeMark),
                    m_style.deckColour(letter),
                    Qt::AlignLeft | Qt::AlignVCenter);
        }
        pPainter->restore();
    }

  private:
    const DelegateStyle m_style;
    const DeckOfRow m_deckOfRow;
    const HoveredRow m_hoveredRow;
};

/// The search row's MonoLabel (decks.js library(): "248 tracks · Crate
/// library"): how many tracks the library shows and where they come from,
/// the sidebar's current item without its count.
class LibraryMeta : public QObject {
  public:
    LibraryMeta(WLabel* pLabel, WLibrary* pLibrary, WLibrarySidebar* pSidebar)
            : QObject(pLabel),
              m_pLabel(pLabel),
              m_pLibrary(pLibrary),
              m_pSidebar(pSidebar) {
        connect(pLibrary, &QStackedWidget::currentChanged, this, &LibraryMeta::refresh);
        if (pSidebar && pSidebar->selectionModel()) {
            connect(pSidebar->selectionModel(),
                    &QItemSelectionModel::currentChanged,
                    this,
                    &LibraryMeta::refresh);
        }
        refresh();
    }

    /// After a view loads or switches its model; the label follows once
    /// the switch is done.
    void refresh() {
        QTimer::singleShot(0, this, [this] { watchModel(); });
    }

  private:
    QAbstractItemModel* currentModel() const {
        QWidget* pPage = m_pLibrary ? m_pLibrary->currentWidget() : nullptr;
        if (!pPage) {
            return nullptr;
        }
        auto* pTable = qobject_cast<WTrackTableView*>(pPage);
        if (!pTable) {
            pTable = pPage->findChild<WTrackTableView*>();
        }
        return pTable ? pTable->model() : nullptr;
    }

    void watchModel() {
        QAbstractItemModel* pModel = currentModel();
        if (pModel != m_pModel) {
            for (const QMetaObject::Connection& connection : std::as_const(m_connections)) {
                disconnect(connection);
            }
            m_connections.clear();
            m_pModel = pModel;
            if (pModel) {
                const auto update = [this] { updateText(); };
                m_connections = {
                        connect(pModel, &QAbstractItemModel::modelReset, this, update),
                        connect(pModel, &QAbstractItemModel::rowsInserted, this, update),
                        connect(pModel, &QAbstractItemModel::rowsRemoved, this, update),
                        connect(pModel, &QAbstractItemModel::layoutChanged, this, update),
                };
            }
        }
        updateText();
    }

    void updateText() {
        if (!m_pLabel) {
            return;
        }
        if (!m_pModel) {
            // WLabel keeps its own copy of the text, which it sets back
            // whenever it resizes
            m_pLabel->setText(QString());
            return;
        }
        const int count = m_pModel->rowCount();
        QString text = count == 1
                ? QCoreApplication::translate("crate::LibraryStyle", "1 track")
                : QCoreApplication::translate("crate::LibraryStyle", "%1 tracks")
                          .arg(groupThousands(QString::number(count)));
        if (m_pSidebar) {
            const QModelIndex current = m_pSidebar->currentIndex();
            QString source = splitCount(current.data(Qt::DisplayRole).toString()).label;
            QModelIndex top = current;
            while (top.parent().isValid()) {
                top = top.parent();
            }
            if (!source.isEmpty() && top != current &&
                    top.data(SidebarModel::IconNameRole).toString() == kCrateSectionIcon) {
                // The mockups' "Crate library": where the tracks come from
                source = QCoreApplication::translate("crate::LibraryStyle", "Crate %1")
                                 .arg(source.toLower());
            }
            if (!source.isEmpty()) {
                text += QStringLiteral(" · ") + source;
            }
        }
        m_pLabel->setText(text);
    }

    QPointer<WLabel> m_pLabel;
    QPointer<WLibrary> m_pLibrary;
    QPointer<WLibrarySidebar> m_pSidebar;
    QPointer<QAbstractItemModel> m_pModel;
    QList<QMetaObject::Connection> m_connections;
};

/// The skin's look of one track table: rows painted under the cells, the
/// deck letters, MonoLabel column headers, the cells' delegates and the
/// default columns.
class TrackTableStyle : public QObject {
  public:
    TrackTableStyle(WTrackTableView* pView,
            const DelegateStyle& style,
            const ColumnWidths& widths,
            const QString& skinPath,
            LoadedDecks* pLoadedDecks,
            LibraryMeta* pMeta)
            : QObject(pView),
              m_pView(pView),
              m_style(style),
              m_widths(widths),
              m_lockIconPath(iconPath(skinPath, QStringLiteral("lock"))),
              m_pLoadedDecks(pLoadedDecks),
              m_pMeta(pMeta) {
        // The deck letters take the place of the vertical header, which Mixxx
        // hides and gives the row height; the rows are not resized by hand
        QHeaderView* pOld = pView->verticalHeader();
        auto* pMarks = new DeckMarkHeader(
                m_style,
                [this](int row) { return deckOfRow(row); },
                [this] { return m_hoveredRow; },
                pView);
        pMarks->setMinimumSectionSize(pOld->minimumSectionSize());
        pMarks->setDefaultSectionSize(pOld->defaultSectionSize());
        pMarks->setSectionResizeMode(QHeaderView::Fixed);
        pView->setVerticalHeader(pMarks);
        pMarks->show();
        m_pMarks = pMarks;

        pView->installEventFilter(this);
        pView->viewport()->installEventFilter(this);
        pView->viewport()->setMouseTracking(true);
        m_pLoadedDecks->addWidget(pView->viewport());
        m_pLoadedDecks->addWidget(pMarks->viewport());
        if (pView->isVisible()) {
            styleModel();
        }
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (!m_pView) {
            return false;
        }
        if (pWatched == m_pView) {
            if (pEvent->type() == QEvent::Show) {
                styleModel();
                // A view shows after it loads a model: the meta follows
                if (m_pMeta) {
                    m_pMeta->refresh();
                }
            }
            return false;
        }
        if (pWatched == m_pView->viewport()) {
            switch (pEvent->type()) {
            case QEvent::Paint:
                paintRows(static_cast<QPaintEvent*>(pEvent)->rect());
                break;
            case QEvent::MouseMove:
                setHoveredRow(m_pView->rowAt(static_cast<QMouseEvent*>(pEvent)->pos().y()));
                break;
            case QEvent::Leave:
                setHoveredRow(-1);
                break;
            case QEvent::Resize:
                if (m_fitColumns) {
                    fitColumns();
                }
                break;
            default:
                break;
            }
            return false;
        }
        if (m_pHeader && pWatched == m_pHeader->viewport() && pEvent->type() == QEvent::Paint) {
            paintHeader();
            return true;
        }
        return false;
    }

  private:
    QString deckOfRow(int row) const {
        auto* pTrackModel = dynamic_cast<TrackModel*>(m_pView->model());
        if (!pTrackModel || !m_pLoadedDecks) {
            return QString();
        }
        return m_pLoadedDecks->deckOf(pTrackModel->getTrackId(m_pView->model()->index(row, 0)));
    }

    QRect rowRect(QWidget* pViewport, int row) const {
        return QRect(0,
                m_pView->rowViewportPosition(row),
                pViewport->width(),
                m_pView->rowHeight(row));
    }

    void setHoveredRow(int row) {
        if (row == m_hoveredRow) {
            return;
        }
        for (const int changed : {m_hoveredRow, row}) {
            if (changed >= 0) {
                m_pView->viewport()->update(rowRect(m_pView->viewport(), changed));
                if (m_pMarks) {
                    m_pMarks->viewport()->update(rowRect(m_pMarks->viewport(), changed));
                }
            }
        }
        m_hoveredRow = row;
    }

    /// Rule, hover and selection under the cells (decks.css .tracks td and
    /// tr[data-hover], tr[data-selected]).
    void paintRows(const QRect& area) {
        QAbstractItemModel* pModel = m_pView->model();
        if (!pModel || pModel->rowCount() == 0) {
            return;
        }
        int first = m_pView->rowAt(area.top());
        int last = m_pView->rowAt(area.bottom());
        if (first < 0) {
            first = 0;
        }
        if (last < 0) {
            last = pModel->rowCount() - 1;
        }
        QPainter painter(m_pView->viewport());
        const QItemSelectionModel* pSelection = m_pView->selectionModel();
        const int width = m_pView->viewport()->width();
        for (int row = first; row <= last; ++row) {
            if (m_pView->isRowHidden(row)) {
                continue;
            }
            const int top = m_pView->rowViewportPosition(row);
            const int height = m_pView->rowHeight(row);
            if (top > area.bottom()) {
                break;
            }
            const QRect content(0, top, width, height - m_style.rule);
            if (pSelection && pSelection->isRowSelected(row, QModelIndex())) {
                painter.fillRect(content, m_style.rowSelected);
            } else if (row == m_hoveredRow) {
                painter.fillRect(content, m_style.rowHover);
            }
            painter.fillRect(QRect(0, top + height - m_style.rule, width, m_style.rule),
                    m_style.borderSubtle);
        }
    }

    /// The column headers as MonoLabels (decks.css .tracks th): textSubtle,
    /// the sorted column in textPrimary with its arrow, aligned like the
    /// column's cells, over a 1 px rule.
    void paintHeader() {
        QHeaderView* pHeader = m_pHeader;
        QAbstractItemModel* pModel = pHeader->model();
        QPainter painter(pHeader->viewport());
        const QRect area = pHeader->viewport()->rect();
        painter.fillRect(area, m_style.background);
        if (pModel) {
            const QFont font = m_style.monoLabelFont(m_style.fontSizeHeader);
            for (int visual = 0; visual < pHeader->count(); ++visual) {
                const int logical = pHeader->logicalIndex(visual);
                if (pHeader->isSectionHidden(logical)) {
                    continue;
                }
                const int left = pHeader->sectionViewportPosition(logical);
                const int width = pHeader->sectionSize(logical);
                if (left + width < area.left() || left > area.right()) {
                    continue;
                }
                QString title = m_titles.value(logical);
                if (title.isEmpty()) {
                    title = pModel->headerData(logical, Qt::Horizontal).toString();
                }
                const bool sorted = pHeader->isSortIndicatorShown() &&
                        pHeader->sortIndicatorSection() == logical;
                if (sorted) {
                    title += pHeader->sortIndicatorOrder() == Qt::AscendingOrder
                            ? QStringLiteral(" ↑")
                            : QStringLiteral(" ↓");
                }
                // A left-aligned title may run into its right padding, as a
                // browser lets it ("Cache" in its 58 px column)
                const Qt::Alignment alignment = m_alignments.value(logical, Qt::AlignLeft);
                const QRect cell(left + m_style.cellPadding,
                        area.top(),
                        width - (alignment & Qt::AlignRight ? 2 : 1) * m_style.cellPadding,
                        area.height() - m_style.rule);
                paintText(&painter,
                        cell,
                        title,
                        font,
                        sorted ? m_style.textPrimary : m_style.textSubtle,
                        alignment | Qt::AlignVCenter);
            }
        }
        painter.fillRect(
                QRect(area.left(), area.bottom() - m_style.rule + 1, area.width(), m_style.rule),
                m_style.border);
    }

    /// After a model load: the cells' delegates, the header's painter and,
    /// without a saved layout, the default columns.
    void styleModel() {
        QAbstractItemModel* pModel = m_pView->model();
        QHeaderView* pHeader = m_pView->horizontalHeader();
        if (!pModel || (pModel == m_pStyledModel && pHeader == m_pHeader)) {
            return;
        }
        m_pStyledModel = pModel;
        m_alignments.clear();
        m_titles.clear();
        QHash<QString, int> columnOfField;
        for (int column = 0; column < pModel->columnCount(); ++column) {
            const QString field =
                    pModel->headerData(column, Qt::Horizontal, TrackModel::kHeaderNameRole)
                            .toString();
            columnOfField.insert(field, column);
            if (field == LIBRARYTABLE_DURATION) {
                // The mockups' name for it, shorter in its 60 px
                m_titles.insert(column, QCoreApplication::translate("crate::LibraryStyle", "Time"));
            }
            const ColumnKind kind = columnKind(field);
            if (kind == ColumnKind::Bpm || field == LIBRARYTABLE_DURATION ||
                    field == kCrateEnergyField ||
                    field == LIBRARYTABLE_BITRATE || field == LIBRARYTABLE_TRACKNUMBER ||
                    field == LIBRARYTABLE_REPLAYGAIN) {
                // As Mixxx aligns their cells
                m_alignments.insert(column, Qt::AlignRight);
            }
            if (kind == ColumnKind::Keep ||
                    !isMixxxTextDelegate(m_pView->itemDelegateForColumn(column))) {
                continue;
            }
            QAbstractItemDelegate* pOld = m_pView->itemDelegateForColumn(column);
            m_pView->setItemDelegateForColumn(column, createDelegate(kind));
            delete pOld;
        }

        if (pHeader != m_pHeader) {
            m_pHeader = pHeader;
            m_columnOfField = columnOfField;
            m_fitColumns = false;
            pHeader->viewport()->installEventFilter(this);
            pHeader->setFixedHeight(m_style.headerHeight);
            // The title is the flexible column of the mockups: it takes what
            // the others leave
            const int title = columnOfField.value(LIBRARYTABLE_TITLE, -1);
            if (title >= 0) {
                pHeader->setSectionResizeMode(title, QHeaderView::Stretch);
            }
            auto* pTrackHeader = qobject_cast<WTrackTableViewHeader*>(pHeader);
            if (pTrackHeader && !pTrackHeader->hasPersistedHeaderState()) {
                applyDefaultColumns(pTrackHeader, columnOfField);
            }
            // The user's own widths stop the fitting; the title's do not,
            // the header stretches it
            connect(pHeader,
                    &QHeaderView::sectionResized,
                    this,
                    [this, title](int section) {
                        if (!m_fitting && section != title) {
                            m_fitColumns = false;
                        }
                    });
        }
        QItemSelectionModel* pSelection = m_pView->selectionModel();
        if (pSelection && pSelection != m_pSelection) {
            // The deck letters' column shows the selection bar
            m_pSelection = pSelection;
            connect(pSelection,
                    &QItemSelectionModel::selectionChanged,
                    m_pMarks,
                    [pMarks = m_pMarks] {
                        if (pMarks) {
                            pMarks->viewport()->update();
                        }
                    });
        }
    }

    QAbstractItemDelegate* createDelegate(ColumnKind kind) {
        TrackTextDelegate* pDelegate = nullptr;
        switch (kind) {
        case ColumnKind::Key:
            return new CamelotDelegate(m_style, m_pView);
        case ColumnKind::Bpm:
            return new BpmDelegate(m_style, m_lockIconPath, m_pView);
        case ColumnKind::Title:
            pDelegate = new TrackTextDelegate(m_style, TrackTextDelegate::Kind::Title, m_pView);
            pDelegate->setLoadedDeck([this](const QModelIndex& index) {
                return deckOfRow(index.row());
            });
            return pDelegate;
        case ColumnKind::Meta:
            return new TrackTextDelegate(m_style, TrackTextDelegate::Kind::Meta, m_pView);
        case ColumnKind::Profile:
            return new ProfileQualityDelegate(m_style, m_pView);
        case ColumnKind::Cache:
            return new CacheStateDelegate(m_style, m_pView);
        case ColumnKind::Name:
        case ColumnKind::Keep:
            break;
        }
        return new TrackTextDelegate(m_style, TrackTextDelegate::Kind::Name, m_pView);
    }

    /// The widths of the mockups' columns, while the table keeps its
    /// default layout: fixed for the numbers, a share of the table for
    /// artist and genre; the title stretches.
    void fitColumns() {
        QHeaderView* pHeader = m_pHeader;
        const int width = m_pView->viewport()->width();
        if (!pHeader || width <= 0) {
            return;
        }
        m_fitting = true;
        const auto fix = [&](const QString& field, int size) {
            const int column = m_columnOfField.value(field, -1);
            if (column >= 0 && !pHeader->isSectionHidden(column)) {
                pHeader->resizeSection(column, size);
            }
        };
        fix(PLAYLISTTRACKSTABLE_POSITION, m_widths.time);
        fix(LIBRARYTABLE_BPM, m_widths.bpm);
        fix(LIBRARYTABLE_KEY, m_widths.key);
        fix(LIBRARYTABLE_DURATION, m_widths.time);
        fix(kCrateEnergyField, m_widths.energy);
        fix(kCrateProfileField, m_widths.profile);
        fix(kCrateCacheField, m_widths.cache);
        // The mockups' shares are of the whole table, deck letters included
        const int table = width + m_style.deckMarkWidth;
        fix(LIBRARYTABLE_ARTIST, static_cast<int>(std::lround(table * m_widths.artist)));
        fix(LIBRARYTABLE_GENRE, static_cast<int>(std::lround(table * m_widths.genre)));
        m_fitting = false;
    }

    /// The mockups' columns, order and widths, for a table that has no
    /// layout of its own yet. Mixxx saves it with the table's layout from
    /// then on, and the user's changes win.
    void applyDefaultColumns(WTrackTableViewHeader* pHeader,
            const QHash<QString, int>& columnOfField) {
        auto* pTrackModel = dynamic_cast<TrackModel*>(m_pView->model());
        if (!pTrackModel) {
            return;
        }
        QList<int> shown;
        for (const QString& field : defaultColumns()) {
            const int column = columnOfField.value(field, -1);
            if (column >= 0 && !pTrackModel->isColumnInternal(column)) {
                shown.append(column);
            }
        }
        if (!shown.contains(columnOfField.value(LIBRARYTABLE_TITLE, -1))) {
            // Not a track list like Mixxx's: leave it as it is
            return;
        }
        for (int column = 0; column < pHeader->count(); ++column) {
            pHeader->setSectionHidden(column, !shown.contains(column));
        }
        for (int visual = 0; visual < shown.size(); ++visual) {
            pHeader->moveSection(pHeader->visualIndex(shown[visual]), visual);
        }

        m_fitColumns = true;
        fitColumns();

        // The header's menu ticks the columns it showed when it was built
        const QList<QCheckBox*> boxes = pHeader->findChildren<QCheckBox*>();
        for (QCheckBox* pBox : boxes) {
            for (int column = 0; column < pHeader->count(); ++column) {
                if (pHeader->model()->headerData(column, Qt::Horizontal).toString() ==
                        pBox->text()) {
                    const QSignalBlocker blocker(pBox);
                    pBox->setChecked(!pHeader->isSectionHidden(column));
                    break;
                }
            }
        }
    }

    QPointer<WTrackTableView> m_pView;
    const DelegateStyle m_style;
    const ColumnWidths m_widths;
    const QString m_lockIconPath;
    QPointer<LoadedDecks> m_pLoadedDecks;
    QPointer<LibraryMeta> m_pMeta;
    QPointer<QHeaderView> m_pMarks;
    QPointer<QHeaderView> m_pHeader;
    QPointer<QAbstractItemModel> m_pStyledModel;
    QPointer<QItemSelectionModel> m_pSelection;
    QHash<int, Qt::Alignment> m_alignments;
    QHash<int, QString> m_titles;
    QHash<QString, int> m_columnOfField;
    int m_hoveredRow = -1;
    bool m_fitColumns = false;
    bool m_fitting = false;
};

} // namespace

void applyLibraryDefaults(const UserSettingsPointer& pConfig) {
    if (pConfig &&
            pConfig->getValueString(mixxx::library::prefs::kBpmColumnPrecisionConfigKey)
                    .isEmpty()) {
        pConfig->setValue(mixxx::library::prefs::kBpmColumnPrecisionConfigKey, kBpmDecimals);
    }
}

void styleLibrary(QWidget* pSkinRoot,
        const SkinVariables& variables,
        const QString& skinPath,
        Library* pLibrary,
        const UserSettingsPointer& pConfig) {
    const DelegateStyle style = DelegateStyle::fromVariables(variables);
    if (!pSkinRoot || !style.isValid()) {
        return;
    }
    DelegateStyle::setCurrent(style);

    const QList<WLibrarySidebar*> sidebars = pSkinRoot->findChildren<WLibrarySidebar*>();
    for (WLibrarySidebar* pSidebar : sidebars) {
        auto* pDelegate = new SidebarDelegate(style, pSidebar);
        // The section of the local library, which Mixxx's features fill
        // (plan "Own brand", BR.PR6)
        pDelegate->setDefaultSection(
                QCoreApplication::translate("crate::LibraryStyle", "Local"));
        pDelegate->setIcons(sidebarIcons(skinPath), SidebarModel::IconNameRole);
        // The Crate feature's root is its section's header
        pDelegate->setSectionRoot(kCrateSectionIcon);
        pSidebar->setItemDelegate(pDelegate);
        // The delegate indents child items itself and draws no branches,
        // as the admin's navigation, and the rows are as wide as the sidebar.
        // The admin hides its navigation's scroll bar (.hide-rail-scrollbar);
        // the wheel and the keys still scroll it
        pSidebar->setIndentation(0);
        pSidebar->setRootIsDecorated(false);
        pSidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        pSidebar->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        pSidebar->setMouseTracking(true);
        pSidebar->viewport()->setAttribute(Qt::WA_Hover);
    }

    // The button bars of Auto DJ and Analyze: the library's gutter, their
    // buttons centred in the row, as the search row above them
    const auto barNumber = [&variables](const char* name) {
        return static_cast<int>(
                std::lround(variables.value(QLatin1String(name)).toDouble()));
    };
    const QList<QWidget*> bars =
            pSkinRoot->findChildren<QWidget*>(QStringLiteral("LibraryFeatureControls"));
    for (QWidget* pBar : bars) {
        if (auto* pLayout = pBar->layout()) {
            const int gutter = barNumber("CrateLibBarGutter");
            const int padding = barNumber("CrateLibBarPadding");
            pLayout->setContentsMargins(gutter, padding, gutter, padding);
            pLayout->setSpacing(barNumber("CrateLibBarGap"));
        }
    }

    const QList<WTrackTableView*> tables = pSkinRoot->findChildren<WTrackTableView*>();
    if (tables.isEmpty()) {
        return;
    }
    // The cabin density of design 14.3, unless the user set a row height
    if (pLibrary && pConfig && pConfig->getValueString(kRowHeightKey).isEmpty()) {
        pLibrary->setRowHeight(style.rowHeight);
    }

    auto* pLoadedDecks = new LoadedDecks(pSkinRoot);
    LibraryMeta* pMeta = nullptr;
    auto* pLabel = pSkinRoot->findChild<WLabel*>(QStringLiteral("MonoLibraryMeta"));
    auto* pLibraryWidget = pSkinRoot->findChild<WLibrary*>();
    if (pLabel && pLibraryWidget) {
        pMeta = new LibraryMeta(pLabel,
                pLibraryWidget,
                sidebars.isEmpty() ? nullptr : sidebars.first());
    }
    const ColumnWidths widths = ColumnWidths::fromVariables(variables);
    for (WTrackTableView* pTable : tables) {
        new TrackTableStyle(pTable, style, widths, skinPath, pLoadedDecks, pMeta);
    }
    kLogger.debug() << "Styled" << tables.size() << "track tables and" << sidebars.size()
                    << "sidebars";
}

} // namespace crate
