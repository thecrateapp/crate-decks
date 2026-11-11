#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTreeView>
#include <cmath>

#include "crate/ui/delegates/camelotdelegate.h"
#include "crate/ui/delegates/celldelegate.h"
#include "crate/ui/delegates/delegateroles.h"
#include "crate/ui/delegates/sidebardelegate.h"
#include "crate/ui/delegates/statedelegates.h"
#include "crate/ui/skin/crateskin.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"

namespace {

using namespace crate::delegates;

/// The values of the CrateDecks skin, as the skin loads them
crate::SkinVariables skinVariables() {
    return crate::readSkinVariables(
            QDir(ConfigObject<ConfigValue>::computeResourcePath())
                    .filePath(QStringLiteral("skins/CrateDecks/generated/vars.xml")));
}

/// How much of `colour` over `background` makes `pixel`: 0 is the
/// background, 1 the colour itself, and a negative value a pixel that is
/// not a shade of that colour (antialiased text and strokes are shades).
double shareOf(QRgb pixel, const QColor& colour, const QColor& background) {
    // The colour as the screen shows it, over the background
    const double alpha = colour.alphaF();
    const double target[3] = {
            colour.redF() * alpha + background.redF() * (1 - alpha),
            colour.greenF() * alpha + background.greenF() * (1 - alpha),
            colour.blueF() * alpha + background.blueF() * (1 - alpha)};
    const double base[3] = {background.redF(), background.greenF(), background.blueF()};
    const double value[3] = {qRed(pixel) / 255.0, qGreen(pixel) / 255.0, qBlue(pixel) / 255.0};
    double dot = 0;
    double length = 0;
    for (int channel = 0; channel < 3; ++channel) {
        dot += (value[channel] - base[channel]) * (target[channel] - base[channel]);
        length += (target[channel] - base[channel]) * (target[channel] - base[channel]);
    }
    if (length <= 0) {
        return -1;
    }
    const double share = dot / length;
    double residual = 0;
    for (int channel = 0; channel < 3; ++channel) {
        const double expected = base[channel] + share * (target[channel] - base[channel]);
        residual = std::max(residual, std::abs(expected - value[channel]));
    }
    return residual <= 4 / 255.0 ? share : -1;
}

/// The strongest shade of `colour` in `rect` of `image`, or -1 if none.
double strongestShade(const QImage& image,
        const QRect& rect,
        const QColor& colour,
        const QColor& background) {
    double strongest = -1;
    const QRect area = rect.intersected(image.rect());
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (pixel == background.rgb()) {
                continue;
            }
            strongest = std::max(strongest, shareOf(pixel, colour, background));
        }
    }
    return strongest;
}

bool isBlank(const QImage& image, const QColor& background) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) != background.rgb()) {
                return false;
            }
        }
    }
    return true;
}

class CrateDelegatesTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_style = DelegateStyle::fromVariables(skinVariables());
        ASSERT_TRUE(m_style.isValid());
        m_background = QColor(skinVariables().value(QStringLiteral("CrateBgApp")));
    }

    /// One cell of `width` x the row height, painted by `delegate`.
    QImage paintCell(QAbstractItemDelegate& delegate,
            const QModelIndex& index,
            int width = 120,
            QStyle::State state = QStyle::State_Enabled) const {
        QImage image(QSize(width, m_style.rowHeight), QImage::Format_RGB32);
        image.fill(m_background);
        QPainter painter(&image);
        QStyleOptionViewItem option;
        option.rect = image.rect();
        option.state = state;
        delegate.paint(&painter, option, index);
        return image;
    }

    QModelIndex item(const QMap<int, QVariant>& data) {
        auto* pItem = new QStandardItem();
        for (auto it = data.cbegin(); it != data.cend(); ++it) {
            pItem->setData(it.value(), it.key());
        }
        m_model.appendRow(pItem);
        return pItem->index();
    }

    /// The text area of a cell: its padding, up to `width`.
    QRect textArea(int width) const {
        return QRect(m_style.cellPadding, 0, width, m_style.rowHeight);
    }

    DelegateStyle m_style;
    QColor m_background;
    QStandardItemModel m_model;
};

TEST_F(CrateDelegatesTest, PaintsCamelotChip) {
    CamelotDelegate delegate(m_style, nullptr);

    // At rest: the Camelot key in mono, textSecondary
    const QModelIndex rest = item({{CamelotRole, QStringLiteral("8A")}});
    const QImage restImage = paintCell(delegate, rest);
    EXPECT_GT(strongestShade(restImage, textArea(30), m_style.textSecondary, m_background), 0.5);
    EXPECT_LT(strongestShade(restImage, restImage.rect(), m_style.accentHi, m_background), 0.5);

    // Mixxx's key column gives the key in the user's notation: shown in
    // Camelot whatever the notation
    EXPECT_EQ(QStringLiteral("8A"),
            delegate.camelotKey(item({{Qt::DisplayRole, QStringLiteral("Am")}})));
    EXPECT_EQ(QStringLiteral("8A"),
            delegate.camelotKey(item({{Qt::DisplayRole, QStringLiteral("1m")}})));
    EXPECT_EQ(QStringLiteral("8B"),
            delegate.camelotKey(item({{Qt::DisplayRole, QStringLiteral("8B")}})));
    EXPECT_EQ(QString(), delegate.camelotKey(item({})));

    // Compatible with the master deck: accentHi text, no outline
    const QModelIndex compatible = item({{CamelotRole, QStringLiteral("9A")},
            {CamelotToneRole, static_cast<int>(CamelotTone::Compatible)}});
    const QImage compatibleImage = paintCell(delegate, compatible);
    EXPECT_GT(strongestShade(compatibleImage, textArea(30), m_style.accentHi, m_background), 0.5);
    // The chip's top edge, a line above the text, centred on the row less
    // its rule
    const int chipTop = (m_style.rowHeight - m_style.rule - m_style.chipHeight) / 2;
    const QRect chipEdge(m_style.cellPadding + m_style.chipHeight / 2, chipTop, 6, 1);
    EXPECT_LT(strongestShade(compatibleImage, chipEdge, m_style.accentHiBorder, m_background),
            0.5);

    // The master deck's key: an accent chip, outline and text
    const QModelIndex master = item({{CamelotRole, QStringLiteral("8A")},
            {CamelotToneRole, static_cast<int>(CamelotTone::Master)}});
    const QImage masterImage = paintCell(delegate, master);
    EXPECT_GT(strongestShade(masterImage, chipEdge, m_style.accentHiBorder, m_background), 0.5);
    const QRect chipInside(m_style.cellPadding + m_style.chipPadding, 0, 20, m_style.rowHeight);
    EXPECT_GT(strongestShade(masterImage, chipInside, m_style.accentHi, m_background), 0.5);
}

TEST_F(CrateDelegatesTest, PaintsProfileQualityTones) {
    ProfileQualityDelegate delegate(m_style, nullptr);

    // A full profile has no mark
    EXPECT_TRUE(isBlank(paintCell(delegate,
                                item({{ProfileQualityRole,
                                        static_cast<int>(ProfileQuality::Full)}})),
            m_background));
    EXPECT_TRUE(isBlank(paintCell(delegate, item({})), m_background));

    // A partial one, an amber dot at the start of the cell
    const QImage partial = paintCell(delegate,
            item({{ProfileQualityRole, static_cast<int>(ProfileQuality::Partial)}}));
    const QRect dot(m_style.cellPadding, 0, m_style.dot, m_style.rowHeight);
    EXPECT_GT(strongestShade(partial, dot, m_style.warning, m_background), 0.95);
    EXPECT_LT(strongestShade(partial,
                      QRect(m_style.cellPadding + m_style.dot + 1, 0, 60, m_style.rowHeight),
                      m_style.warning,
                      m_background),
            0);

    // None: "NO PROFILE" in mono, textSubtle, and nothing amber
    const QImage missing = paintCell(delegate,
            item({{ProfileQualityRole, static_cast<int>(ProfileQuality::Missing)}}));
    EXPECT_GT(strongestShade(missing, textArea(80), m_style.textSubtle, m_background), 0.5);
    EXPECT_LT(strongestShade(missing, missing.rect(), m_style.warning, m_background), 0.5);
}

TEST_F(CrateDelegatesTest, PaintsCacheState) {
    CacheStateDelegate delegate(m_style, nullptr);
    const QRect dot(m_style.cellPadding, 0, m_style.dot, m_style.rowHeight);

    EXPECT_TRUE(isBlank(paintCell(delegate, item({})), m_background));

    // Cached: an accentHi dot
    const QImage ready = paintCell(delegate,
            item({{CacheStateRole, static_cast<int>(CacheState::Ready)}}));
    EXPECT_GT(strongestShade(ready, dot, m_style.accentHi, m_background), 0.95);

    // Downloading half of it: the level ring of r 7 from the top clockwise
    // to the bottom in accentHi, the rest of the ring a 1 px track
    const QImage downloading = paintCell(delegate,
            item({{CacheStateRole, static_cast<int>(CacheState::Downloading)},
                    {CacheProgressRole, 0.5}}));
    const double radius = m_style.ringRadius;
    const QPointF centre(m_style.cellPadding + radius + 2, m_style.rowHeight / 2.0);
    const auto around = [&](double degrees) {
        // Clockwise from the top
        const double angle = degrees * M_PI / 180;
        const QPoint at(static_cast<int>(std::floor(centre.x() + radius * std::sin(angle))),
                static_cast<int>(std::floor(centre.y() - radius * std::cos(angle))));
        return QRect(at - QPoint(1, 1), QSize(3, 3));
    };
    EXPECT_GT(strongestShade(downloading, around(45), m_style.accentHi, m_background), 0.5);
    EXPECT_GT(strongestShade(downloading, around(90), m_style.accentHi, m_background), 0.5);
    EXPECT_GT(strongestShade(downloading, around(135), m_style.accentHi, m_background), 0.5);
    EXPECT_LT(strongestShade(downloading, around(270), m_style.accentHi, m_background), 0.2);
    EXPECT_GT(strongestShade(downloading, around(270), m_style.border, m_background), 0.3);

    // Failed: an amber dot and "FAILED" in the warning text colour
    const QImage failed = paintCell(delegate,
            item({{CacheStateRole, static_cast<int>(CacheState::Failed)}}));
    EXPECT_GT(strongestShade(failed, dot, m_style.warning, m_background), 0.95);
    const QRect label(m_style.cellPadding + m_style.dot + m_style.stateGap,
            0,
            50,
            m_style.rowHeight);
    EXPECT_GT(strongestShade(failed, label, m_style.warningText, m_background), 0.5);
    EXPECT_LT(strongestShade(failed, failed.rect(), m_style.accentHi, m_background), 0.5);
}

TEST_F(CrateDelegatesTest, PaintsSidebarSectionsAndActiveItem) {
    QTreeView view;
    view.resize(240, 300);
    SidebarDelegate delegate(m_style, &view);
    delegate.setDefaultSection(QStringLiteral("Local"));
    const QModelIndex crate = item({{Qt::DisplayRole, QStringLiteral("Library")},
            {SidebarSectionRole, QStringLiteral("Crate · dev.crate.local")},
            {SidebarCountRole, QStringLiteral("136 512")}});
    const QModelIndex tracks = item({{Qt::DisplayRole, QStringLiteral("Tracks")}});
    const QModelIndex autoDj = item({{Qt::DisplayRole, QStringLiteral("Auto DJ")}});

    // A header above the first item of each section
    EXPECT_EQ(QStringLiteral("Crate · dev.crate.local"), delegate.sectionHeader(crate));
    EXPECT_EQ(QStringLiteral("Local"), delegate.sectionHeader(tracks));
    EXPECT_EQ(QString(), delegate.sectionHeader(autoDj));
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 240, m_style.rowHeight);
    EXPECT_EQ(m_style.rowHeight + delegate.sectionHeaderHeight(),
            delegate.sizeHint(option, tracks).height());
    EXPECT_EQ(m_style.rowHeight, delegate.sizeHint(option, autoDj).height());

    // The active item: its header in accentHi, a 2 px accentHi bar on the
    // left of the item and its label in textPrimary
    const int header = delegate.sectionHeaderHeight();
    QImage image(240, header + m_style.rowHeight, QImage::Format_RGB32);
    image.fill(m_background);
    {
        QPainter painter(&image);
        option.rect = image.rect();
        option.state = QStyle::State_Enabled | QStyle::State_Selected;
        delegate.paint(&painter, option, tracks);
    }
    EXPECT_GT(strongestShade(image, QRect(0, 0, 240, header), m_style.accentHi, m_background),
            0.5);
    const QRect bar(m_style.sidebarItemInset,
            header + m_style.sidebarBarInset,
            m_style.activeBar,
            m_style.rowHeight - 2 * m_style.sidebarBarInset);
    EXPECT_GT(strongestShade(image, bar, m_style.accentHi, m_background), 0.95);
    EXPECT_LT(strongestShade(image,
                      bar.translated(m_style.activeBar, 0),
                      m_style.accentHi,
                      m_background),
            0.5);
    const QRect label(m_style.sidebarItemInset + m_style.sidebarItemPadding,
            header,
            100,
            m_style.rowHeight);
    EXPECT_GT(strongestShade(image, label, m_style.textPrimary, m_background), 0.5);

    // At rest, in navIdle and without the bar
    image.fill(m_background);
    {
        QPainter painter(&image);
        option.rect = QRect(0, 0, 240, m_style.rowHeight);
        option.state = QStyle::State_Enabled;
        delegate.paint(&painter, option, autoDj);
    }
    EXPECT_LT(strongestShade(image, bar.translated(0, -header), m_style.accentHi, m_background),
            0);
    EXPECT_GT(strongestShade(image, label.translated(0, -header), m_style.navIdle, m_background),
            0.5);

    // Mixxx's "Tracks (1234)": the count on the right, in mono, textFaint
    const QModelIndex counted = item({{Qt::DisplayRole, QStringLiteral("Crates (1234)")}});
    image.fill(m_background);
    {
        QPainter painter(&image);
        delegate.paint(&painter, option, counted);
    }
    const QRect right(240 - m_style.sidebarItemInset - m_style.sidebarItemPadding - 30,
            0,
            30,
            m_style.rowHeight);
    EXPECT_GT(strongestShade(image, right, m_style.textFaint, m_background), 0.5);
}

// The Cache item writes its use as its count (GU1 mockups sidebar)
TEST(CrateSidebarCountTest, SizesAreCountsToo) {
    const auto size = crate::delegates::splitCount(QStringLiteral("Cache (18.4 GB)"));
    EXPECT_EQ(QStringLiteral("Cache"), size.label);
    EXPECT_EQ(QStringLiteral("18.4 GB"), size.count);
    const auto count = crate::delegates::splitCount(QStringLiteral("Library (136512)"));
    EXPECT_EQ(QStringLiteral("Library"), count.label);
    EXPECT_EQ(QStringLiteral("136 512"), count.count);
    EXPECT_TRUE(crate::delegates::splitCount(QStringLiteral("Mix (1.5)")).count.isEmpty());
}

// The Crate feature's root (plan F3.C-PR2): its title is the section's
// header and its children are painted as top-level items with their icons
TEST_F(CrateDelegatesTest, PaintsSectionRootAsHeaderAndItsChildrenAtTheTop) {
    QTreeView view;
    view.resize(240, 300);
    SidebarDelegate delegate(m_style, &view);
    delegate.setDefaultSection(QStringLiteral("Mixxx"));
    constexpr int kIconNameRole = Qt::UserRole + 7;
    QTemporaryDir icons;
    ASSERT_TRUE(icons.isValid());
    const QString iconPath = icons.filePath(QStringLiteral("library.svg"));
    {
        QFile icon(iconPath);
        ASSERT_TRUE(icon.open(QIODevice::WriteOnly));
        icon.write(R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24">)"
                   R"(<rect width="24" height="24" fill="black"/></svg>)");
    }
    delegate.setIcons({{QStringLiteral("crate-library"), iconPath}}, kIconNameRole);
    delegate.setSectionRoot(QStringLiteral("crateserver"));

    auto* pRoot = new QStandardItem(QStringLiteral("Crate · dev.crate.local"));
    pRoot->setData(QStringLiteral("crateserver"), kIconNameRole);
    auto* pLibrary = new QStandardItem(QStringLiteral("Library (136512)"));
    // An icon that only names the Solar icon, as the Crate feature's
    class NamedEngine : public QIconEngine {
      public:
        void paint(QPainter*, const QRect&, QIcon::Mode, QIcon::State) override {
        }
        QIconEngine* clone() const override {
            return new NamedEngine();
        }
        QString iconName() override {
            return QStringLiteral("crate-library");
        }
    };
    pLibrary->setIcon(QIcon(new NamedEngine()));
    pRoot->appendRow(pLibrary);
    m_model.appendRow(pRoot);
    const QModelIndex tracks = item({{Qt::DisplayRole, QStringLiteral("Tracks")}});
    const QModelIndex root = pRoot->index();
    const QModelIndex library = pLibrary->index();

    EXPECT_TRUE(delegate.isSectionRoot(root));
    EXPECT_FALSE(delegate.isSectionRoot(library));
    EXPECT_EQ(QStringLiteral("Crate · dev.crate.local"), delegate.sectionHeader(root));
    EXPECT_EQ(QString(), delegate.sectionHeader(library));
    // Mixxx's section starts after it
    EXPECT_EQ(QStringLiteral("Mixxx"), delegate.sectionHeader(tracks));
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 240, m_style.rowHeight);
    EXPECT_EQ(delegate.sectionHeaderHeight(), delegate.sizeHint(option, root).height());
    EXPECT_EQ(m_style.rowHeight, delegate.sizeHint(option, library).height());

    // The root: only its header, never as an active item
    QImage image(240, delegate.sectionHeaderHeight(), QImage::Format_RGB32);
    image.fill(m_background);
    {
        QPainter painter(&image);
        option.rect = image.rect();
        option.state = QStyle::State_Enabled | QStyle::State_Selected;
        delegate.paint(&painter, option, root);
    }
    EXPECT_GT(strongestShade(image, image.rect(), m_style.accentHi, m_background), 0.5);
    EXPECT_LT(strongestShade(image, image.rect(), m_style.textPrimary, m_background), 0.5);

    // Its child: unindented, with its icon where a top-level item has it
    image = QImage(240, m_style.rowHeight, QImage::Format_RGB32);
    image.fill(m_background);
    {
        QPainter painter(&image);
        option.rect = image.rect();
        option.state = QStyle::State_Enabled;
        delegate.paint(&painter, option, library);
    }
    const int iconLeft = m_style.sidebarItemInset + m_style.sidebarItemPadding;
    const QRect iconArea(iconLeft,
            (m_style.rowHeight - m_style.sidebarIcon) / 2,
            m_style.sidebarIcon,
            m_style.sidebarIcon);
    EXPECT_GT(strongestShade(image, iconArea, m_style.navIdle, m_background), 0.5);
}

TEST_F(CrateDelegatesTest, PaintsLoadedTitleInTheDeckColour) {
    TrackTextDelegate title(m_style, TrackTextDelegate::Kind::Title, nullptr);
    const QModelIndex loaded = item({{Qt::DisplayRole, QStringLiteral("Lattice")},
            {LoadedDeckRole, QStringLiteral("A")}});
    EXPECT_EQ(m_style.deck[0], title.textColour(loaded));
    const QModelIndex resting = item({{Qt::DisplayRole, QStringLiteral("Glass Orchard")}});
    EXPECT_EQ(m_style.textPrimary, title.textColour(resting));
    // Mixxx's played and missing colours win over the rest colour
    const QModelIndex played = item({{Qt::DisplayRole, QStringLiteral("Low Sun")},
            {Qt::ForegroundRole, m_style.textSubtle}});
    EXPECT_EQ(m_style.textSubtle, title.textColour(played));

    TrackTextDelegate name(m_style, TrackTextDelegate::Kind::Name, nullptr);
    EXPECT_EQ(m_style.textMuted, name.textColour(resting));
    TrackTextDelegate meta(m_style, TrackTextDelegate::Kind::Meta, nullptr);
    EXPECT_EQ(m_style.textSecondary, meta.textColour(resting));
    const QImage image = paintCell(meta, item({{Qt::DisplayRole, QStringLiteral("124.00")}}));
    EXPECT_GT(strongestShade(image, textArea(50), m_style.textSecondary, m_background), 0.5);
}

} // namespace
