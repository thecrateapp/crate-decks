// The Smart Mix plan's timeline view (plan V3.PR1b): it renders the plan
// with the tokens' strokes, follows the outgoing deck's position, keeps
// everything but the playhead in a pixmap and repaints only the playhead's
// strips when it moves. The plan is the assistant's long blend fixture; the
// grids are constant at the real profiles' tempo and downbeat.

#include "crate/ui/widgets/transitionplanview.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFontMetricsF>
#include <QImage>
#include <QPaintEvent>
#include <QTest>
#include <algorithm>
#include <cmath>

#include "crate/analysis/mixprofile.h"
#include "crate/smartmix/transitionplan.h"
#include "crate/ui/skin/crateskin.h"
#include "crate/ui/widgets/transitionplanviewmodel.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"

namespace {

using namespace crate::widgets;
using Side = TransitionPlanViewModel::Side;

constexpr mixxx::audio::SampleRate::value_t kSampleRate = 44100;
// The mockup's plan stage at 1920 x 1080
const QSize kMockupStage(1100, 332);

crate::SkinVariables skinVariables() {
    return crate::readSkinVariables(
            QDir(ConfigObject<ConfigValue>::computeResourcePath())
                    .filePath(QStringLiteral("skins/CrateDecks/generated/vars.xml")));
}

/// How much of `colour` over `background` makes `pixel` (0 the background,
/// 1 the colour), or -1 for a pixel that is not a shade of it
double shareOf(QRgb pixel, const QColor& colour, const QColor& background) {
    const double target[3] = {colour.redF(), colour.greenF(), colour.blueF()};
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
    return residual <= 6 / 255.0 ? share : -1;
}

/// Collects the regions the view paints
class PaintSpy : public QObject {
  public:
    QList<QRegion> regions;

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override {
        if (pEvent->type() == QEvent::Paint) {
            regions.append(static_cast<QPaintEvent*>(pEvent)->region());
        }
        return QObject::eventFilter(pWatched, pEvent);
    }
};

class CrateTransitionPlanViewTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_style = InstrumentStyle::fromVariables(skinVariables());
        ASSERT_TRUE(m_style.isValid());
        m_pModel = std::make_unique<TransitionPlanViewModel>();
        m_pView = std::make_unique<TransitionPlanView>(m_pModel.get(), m_style);
        m_pView->resize(kMockupStage);
        setPlan();
        // Deck A 7.7 s before the entry, as the mockup
        m_pModel->setOutgoingPositionMs(330298.0 - 7700.0);
    }

    void TearDown() override {
        m_pView.reset();
        m_pModel.reset();
    }

    QString dataPath(const QString& name) const {
        return getTestDir().filePath(QStringLiteral("crate/data/") + name);
    }

    PlanDeck deck(const QString& group, const QString& profileName, double durationMs) const {
        QFile file(dataPath(QStringLiteral("mix-profiles/") + profileName));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        const auto profile = crate::analysis::parseMixProfile(file.readAll()).value();
        PlanDeck deck;
        deck.group = group;
        deck.title = group == QStringLiteral("[Channel1]") ? QStringLiteral("Lattice")
                                                           : QStringLiteral("Northbound Static");
        deck.durationMs = durationMs;
        deck.bpm = *profile.bpm;
        deck.pBeats = mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::FramePos(
                        std::round(*profile.downbeatAnchorMs * kSampleRate / 1000.0)),
                mixxx::Bpm(*profile.bpm));
        return deck;
    }

    void setPlan() {
        QFile file(dataPath(QStringLiteral("transition-plans/synthetic-assist-long-blend.json")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const auto plan = crate::TransitionPlan::fromJson(file.readAll());
        ASSERT_TRUE(plan.ok());
        m_pModel->setPlan(plan.value(),
                deck(QStringLiteral("[Channel1]"),
                        QStringLiteral("real-full-whole-bpm.json"),
                        427298.0),
                deck(QStringLiteral("[Channel2]"),
                        QStringLiteral("real-full-fractional-bpm.json"),
                        148052.0));
    }

    QImage render() const {
        QImage image(m_pView->size(), QImage::Format_RGB32);
        image.fill(m_style.background);
        m_pView->render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
        return image;
    }

    double share(const QImage& image, QPointF at, const QColor& colour) const {
        return shareOf(image.pixel(at.toPoint()), colour, m_style.background);
    }

    InstrumentStyle m_style;
    std::unique_ptr<TransitionPlanViewModel> m_pModel;
    std::unique_ptr<TransitionPlanView> m_pView;
};

// At the mockup's size the whole plan shows: lanes and legend. The entry
// cue is filled in accent, the exit is a plan cue with an accentHi rim, the
// playhead's pulse sits on the outgoing ruler, and the incoming ruler is
// silent before the entry.
TEST_F(CrateTransitionPlanViewTest, RendersThePlan) {
    EXPECT_TRUE(m_pView->showsTimeline());
    EXPECT_TRUE(m_pView->showsLanes());
    EXPECT_TRUE(m_pView->showsLegend());
    const QImage image = render();
    const double yA = m_pView->outgoingRulerY();
    const double yB = m_pView->incomingRulerY();
    EXPECT_LT(yA, yB);

    const double entry = m_pView->xOf(m_pModel->windowStartMs());
    EXPECT_EQ(m_style.accent, image.pixelColor(QPointF(entry, yB).toPoint()));
    const double exit = m_pView->xOf(m_pModel->windowEndMs());
    double rim = -1;
    for (int dx = -7; dx <= 7; ++dx) {
        rim = std::max(rim, share(image, QPointF(exit + dx, yA), m_style.accentHi));
    }
    EXPECT_GT(rim, 0.8);
    EXPECT_EQ(m_style.background, image.pixelColor(QPointF(exit, yA).toPoint()));

    const double now = m_pView->xOf(m_pModel->nowMs());
    EXPECT_GT(share(image, QPointF(now + 0.5, yA), m_style.accentHi), 0.8);
    EXPECT_LT(now, entry);

    // The tempo is solid on its lane: its first value is the outgoing
    // tempo, so it runs high, above the lane
    const double tempoY = m_pView->laneYs()[0] - m_style.plan.tempoSwing;
    const double quarter = entry + (exit - entry) / 4;
    double tempo = -1;
    for (int dy = -1; dy <= 1; ++dy) {
        tempo = std::max(tempo, share(image, QPointF(quarter, tempoY + dy), m_style.accentHi));
    }
    EXPECT_GT(tempo, 0.5);

    // Nothing of the incoming deck before its entry but its silent ruler
    const QPointF before((m_pView->xOf(m_pModel->rangeStartMs()) + now) / 2, yB + 5);
    EXPECT_EQ(m_style.background, image.pixelColor(before.toPoint()));
}

// The bass swap is a connection in the "3 4" stroke pattern of a queue
// (dashQueue): along its line, 3 px dashes and 4 px gaps.
TEST_F(CrateTransitionPlanViewTest, BassHandoffUsesQueueDash) {
    ASSERT_TRUE(m_pModel->bassSwap());
    ASSERT_EQ(QVector<qreal>({3, 4}), m_style.dashQueue);
    const QImage image = render();
    const int x = static_cast<int>(std::floor(m_pView->xOf(m_pModel->bassSwap()->atMs)));
    // Between the levels and the beatmatch lanes, where nothing else is
    const int top = static_cast<int>(
            std::ceil(m_pView->laneYs()[1] + m_style.plan.gainSwing + 3));
    const int bottom = static_cast<int>(std::floor(m_pView->laneYs()[2] - 3));
    ASSERT_GT(bottom - top, 20);
    QList<int> dashes;
    QList<int> gaps;
    int run = 0;
    bool lit = share(image, QPointF(x, top), m_style.accentHi) > 0.4;
    for (int y = top + 1; y <= bottom; ++y) {
        const bool pixelLit = share(image, QPointF(x, y), m_style.accentHi) > 0.4;
        ++run;
        if (pixelLit != lit) {
            (lit ? dashes : gaps).append(run);
            run = 0;
            lit = pixelLit;
        }
    }
    // The first and last runs are cut by the window
    ASSERT_GE(dashes.size(), 3);
    ASSERT_GE(gaps.size(), 3);
    for (int index = 1; index < dashes.size() - 1; ++index) {
        EXPECT_NEAR(3, dashes[index], 1) << index;
    }
    for (int index = 1; index < gaps.size() - 1; ++index) {
        EXPECT_NEAR(4, gaps[index], 1) << index;
    }
}

// The playhead follows the outgoing deck: its line and pulse move, and its
// label names the bar.
TEST_F(CrateTransitionPlanViewTest, PlayheadFollowsPosition) {
    const double yA = m_pView->outgoingRulerY();
    const double first = m_pView->xOf(m_pModel->nowMs());
    EXPECT_TRUE(m_pView->playheadRegion().contains(
            QPoint(static_cast<int>(first), static_cast<int>(yA))));
    QImage image = render();
    EXPECT_GT(share(image, QPointF(first + 0.5, yA), m_style.accentHi), 0.8);

    // Into the transition
    m_pModel->setOutgoingPositionMs(m_pModel->windowStartMs() + 20000.0);
    const double second = m_pView->xOf(m_pModel->nowMs());
    EXPECT_GT(second, first + 50);
    EXPECT_TRUE(m_pView->playheadRegion().contains(
            QPoint(static_cast<int>(second), static_cast<int>(yA))));
    EXPECT_FALSE(m_pView->playheadRegion().contains(
            QPoint(static_cast<int>(first), static_cast<int>(yA))));
    image = render();
    EXPECT_GT(share(image, QPointF(second + 0.5, yA), m_style.accentHi), 0.8);
    EXPECT_LT(share(image, QPointF(first + 0.5, yA), m_style.accentHi), 0.5);
    // The line, at the playhead's opacity, between the rulers
    const double between = (m_pView->laneYs()[2] + m_pView->incomingRulerY()) / 2;
    EXPECT_NEAR(m_style.plan.playheadOpacity,
            shareOf(image.pixel(static_cast<int>(std::floor(second)), static_cast<int>(between)),
                    m_style.textPrimary,
                    m_style.background),
            0.1);
    EXPECT_EQ(QStringLiteral("Now · bar %1").arg(*m_pModel->nowBar()), m_pModel->nowLabel());
}

// "Now · bar N" stays clear of the incoming deck's names on its row: close
// to the entry it moves left of "Entry".
TEST_F(CrateTransitionPlanViewTest, NowStaysClearOfTheEntry) {
    render();
    const double entry = m_pView->xOf(m_pModel->windowStartMs());
    const QFont font = m_style.monoFont(m_style.plan.fontSizeCue, m_style.plan.trackingCue);
    const double half = QFontMetricsF(font).horizontalAdvance(QStringLiteral("ENTRY")) / 2;
    // A beat before the entry: the playhead almost on it
    m_pModel->setOutgoingPositionMs(m_pModel->windowStartMs() - 500.0);
    const double now = m_pView->xOf(m_pModel->nowMs());
    ASSERT_LT(entry - now, half);
    const QRectF label = m_pView->nowLabelRect();
    EXPECT_FALSE(label.isEmpty());
    EXPECT_LE(label.right(), entry - half - m_style.plan.legendGap + 1);
    EXPECT_GE(label.left(), 0);
    EXPECT_TRUE(m_pView->playheadRegion().contains(label.toAlignedRect()));
}

// Everything but the playhead is painted once into a pixmap: a moving
// playhead and another expose only copy it; a new plan or another size
// paint it again.
TEST_F(CrateTransitionPlanViewTest, CachesStaticScaleInPixmap) {
    render();
    const qint64 painted = m_pView->staticCacheKey();
    EXPECT_NE(0, painted);
    for (int step = 1; step <= 10; ++step) {
        m_pModel->setOutgoingPositionMs(m_pModel->nowMs() + 1000.0);
        render();
    }
    EXPECT_EQ(painted, m_pView->staticCacheKey());
    setPlan();
    render();
    const qint64 again = m_pView->staticCacheKey();
    EXPECT_NE(painted, again);
    m_pView->resize(kMockupStage.width() - 100, kMockupStage.height());
    render();
    EXPECT_NE(again, m_pView->staticCacheKey());
}

// While the transition runs, a move of the playhead repaints only its old
// and new strips, and a move of less than a pixel within the same bar
// repaints nothing.
TEST_F(CrateTransitionPlanViewTest, RepaintsOnlyDirtyRegionDuringRun) {
    m_pView->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(m_pView.get()));
    // The platform may size a new window its own way first
    m_pView->resize(kMockupStage);
    ASSERT_TRUE(QTest::qWaitFor([this] { return m_pView->size() == kMockupStage; }, 2000));
    PaintSpy spy;
    m_pView->installEventFilter(&spy);
    // Until the platform is done exposing the window
    const auto settle = [&spy] {
        for (int attempt = 0; attempt < 20; ++attempt) {
            spy.regions.clear();
            QTest::qWait(100);
            if (spy.regions.isEmpty()) {
                return;
            }
        }
    };
    settle();

    const QRegion before = m_pView->playheadRegion();
    m_pModel->setOutgoingPositionMs(m_pModel->windowStartMs() + 2000.0);
    const QRegion after = m_pView->playheadRegion();
    ASSERT_TRUE(QTest::qWaitFor([&spy] { return !spy.regions.isEmpty(); }, 2000));
    QCoreApplication::processEvents();
    ASSERT_EQ(1, spy.regions.size());
    const QRegion painted = spy.regions.first();
    const QRegion expected = before.united(after).intersected(m_pView->rect());
    EXPECT_TRUE(expected.subtracted(painted).isEmpty());
    // Strips: a fifth of the view at most
    qint64 area = 0;
    for (const QRect& rect : painted) {
        area += static_cast<qint64>(rect.width()) * rect.height();
    }
    EXPECT_LT(area, static_cast<qint64>(m_pView->width()) * m_pView->height() / 5);

    // Less than a pixel, within the bar: to the middle of the pixel the
    // playhead is in
    const double now = m_pModel->nowMs();
    const double pxPerMs = (m_pView->xOf(now + 1000.0) - m_pView->xOf(now)) / 1000.0;
    const double x = m_pView->xOf(now);
    const double centre = now + (std::floor(x) + 0.5 - x) / pxPerMs;
    ASSERT_EQ(m_pModel->barAt(Side::Outgoing, now), m_pModel->barAt(Side::Outgoing, centre));
    settle();
    m_pModel->setOutgoingPositionMs(centre);
    QTest::qWait(50);
    EXPECT_TRUE(spy.regions.isEmpty());
    m_pView->removeEventFilter(&spy);
}

// The view shows what fits its height: lanes and legend at the mockup's,
// the lanes without the legend below that, the compact timeline (rulers,
// moments, window, bass swap and playhead) below that, nothing below its
// minimum; and nothing without a plan.
TEST_F(CrateTransitionPlanViewTest, FitsWhatItShowsToItsHeight) {
    const int width = kMockupStage.width();
    EXPECT_GE(kMockupStage.height(), m_pView->sizeHint().height());
    m_pView->resize(width, m_pView->sizeHint().height());
    EXPECT_TRUE(m_pView->showsLanes());
    EXPECT_TRUE(m_pView->showsLegend());

    m_pView->resize(width, m_pView->sizeHint().height() - 30);
    EXPECT_TRUE(m_pView->showsLanes());
    EXPECT_FALSE(m_pView->showsLegend());
    // The lanes keep their least gap
    EXPECT_GE(m_pView->laneYs()[1] - m_pView->laneYs()[0], m_style.plan.laneMinGap);

    m_pView->resize(width, m_pView->minimumSizeHint().height());
    EXPECT_TRUE(m_pView->showsTimeline());
    EXPECT_FALSE(m_pView->showsLanes());
    QImage image = render();
    // The bass swap still shows, between the rulers
    const double bass = m_pView->xOf(m_pModel->bassSwap()->atMs);
    double swap = -1;
    for (double y = m_pView->outgoingRulerY() + m_style.plan.arrowRise;
            y < m_pView->incomingRulerY() - m_style.plan.arrowGap;
            y += 1) {
        swap = std::max(swap, share(image, QPointF(bass, y), m_style.accentHi));
    }
    EXPECT_GT(swap, 0.5);

    m_pView->resize(width, m_pView->minimumSizeHint().height() - 1);
    EXPECT_FALSE(m_pView->showsTimeline());
    image = render();
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            ASSERT_EQ(m_style.background.rgb(), image.pixel(x, y)) << x << "," << y;
        }
    }

    m_pView->resize(kMockupStage);
    m_pModel->clear();
    EXPECT_FALSE(m_pView->showsTimeline());
}

} // namespace
