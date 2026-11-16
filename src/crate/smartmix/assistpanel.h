#pragma once

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <memory>

#include "library/libraryview.h"
#include "track/track_decl.h"

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace crate {

class SmartMixAssist;

namespace widgets {
class LevelRing;
class TransitionPlanView;
class TransitionPlanViewModel;
} // namespace widgets

/// The Smart Mix assistant (plan F6.C-PR4) in the library pane, the "Smart
/// Mix" item of the Crate section, which the toolbar's Smart Mix button shows
/// and hides: the plan SmartMixAssist holds for "master deck → the other
/// deck", after the GU1 mockup smart-mix-assistant.
///
/// - The view head: the Smart Mix eyebrow, the decks, the Assist and Auto DJ
///   chips (the mode that drives the decks: the Auto DJ chip turns Auto DJ
///   on, the Assist chip off) and the plan's status.
/// - The edge as a StackMap host: both decks' key and tempo, the titles,
///   the artists, the plan's length and the time to its entry.
/// - The inspector: the mode, a sentence of the plan, its state, the
///   confidence, the time to the entry and the length, then mode, entry,
///   tempo adjustment, bass swap, gain and planner and the controls a
///   controller maps. These details scroll when the library pane is short;
///   the Prepare, Execute and Cancel buttons stay at the column's foot,
///   with nothing under them.
///
/// - The plan's timeline (TransitionPlanView, plan V3.PR1b) under the host,
///   while there is a plan: the decks' bars, the plan's moments, window,
///   bass swap, tempo, levels and legend, and the outgoing deck's playhead,
///   which moves a few times a second. In a pane too short for both the
///   host and the compact timeline, the timeline takes the host's place:
///   its rulers name both tracks, and the inspector keeps the length and
///   the time to the entry.
///
/// A thin layer over SmartMixAssist, which it does not own and may not have
/// yet: without one it says that Smart Mix needs a Crate server.
class AssistPanel : public QWidget, public LibraryView {
    Q_OBJECT
  public:
    explicit AssistPanel(QWidget* pParent = nullptr);
    ~AssistPanel() override;

    void setAssist(SmartMixAssist* pAssist);
    SmartMixAssist* assist() const {
        return m_pAssist;
    }

    // LibraryView
    void onShow() override;
    bool hasFocus() const override;

    /// False: the panel takes the library pane's height, whatever its
    /// labels wrap to.
    bool hasHeightForWidth() const override;

    // What it shows, for tests
    QString statusText() const;
    QString titleText() const;
    QString modeText() const;
    QString stateText() const;
    /// The inspector's row `key` ("Mode", "Entry", "Tempo", "Bass swap",
    /// "Gain", "Planner"); empty if hidden.
    QString metadata(const QString& key) const;
    QString readingText(const QString& key) const;
    QPushButton* prepareButton() const {
        return m_pPrepare;
    }
    QPushButton* executeButton() const {
        return m_pExecute;
    }
    QPushButton* cancelButton() const {
        return m_pCancel;
    }
    QPushButton* assistChip() const {
        return m_pAssistChip;
    }
    QPushButton* autoDjChip() const {
        return m_pAutoDjChip;
    }
    /// The inspector's details, which scroll above its actions.
    QScrollArea* details() const {
        return m_pDetails;
    }
    /// Prepare, Execute and Cancel, pinned at the inspector's foot.
    QWidget* actions() const {
        return m_pActions;
    }
    /// The edge's titles and readings, above the timeline.
    QWidget* host() const {
        return m_pHost;
    }
    /// The plan's timeline, and what it shows.
    widgets::TransitionPlanView* planView() const {
        return m_pPlanView;
    }
    const widgets::TransitionPlanViewModel* planModel() const {
        return m_pPlanModel.get();
    }

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override;
    void showEvent(QShowEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;

  private:
    QWidget* makeHead();
    QWidget* makeHost();
    QWidget* makeInspector();
    void showAssist();
    void showTimes();
    void showPlan();
    /// Follows the decks' tracks: their grids and cues place the timeline
    void watchTracks(const TrackPointer& pOutgoing, const TrackPointer& pIncoming);
    void movePlayhead();
    /// The host and the timeline in the plan's column
    void fitPlan();
    void setAutoDj(bool on);

    QPointer<SmartMixAssist> m_pAssist;
    QTimer m_clock;
    QTimer m_playhead;

    QLabel* m_pDecks = nullptr;
    QPushButton* m_pAssistChip = nullptr;
    QPushButton* m_pAutoDjChip = nullptr;
    QLabel* m_pStatusDot = nullptr;
    QLabel* m_pStatus = nullptr;

    QWidget* m_pPlanColumn = nullptr;
    QWidget* m_pHost = nullptr;
    bool m_hasEdge = false;
    const std::unique_ptr<widgets::TransitionPlanViewModel> m_pPlanModel;
    widgets::TransitionPlanView* m_pPlanView = nullptr;
    /// The rates the timeline was laid out with
    double m_outgoingRate = 1.0;
    double m_incomingRate = 1.0;
    /// The tracks it was laid out from, whose grids and cues lay it out
    /// again once they settle
    TrackWeakPointer m_pOutgoingTrack;
    TrackWeakPointer m_pIncomingTrack;
    QList<QMetaObject::Connection> m_trackConnections;
    QTimer m_relayout;
    QLabel* m_pHostMeta = nullptr;
    QLabel* m_pTitle = nullptr;
    QLabel* m_pArtists = nullptr;
    QLabel* m_pLength = nullptr;
    QLabel* m_pLengthUnit = nullptr;
    QLabel* m_pToEntry = nullptr;

    QLabel* m_pMode = nullptr;
    QLabel* m_pDescription = nullptr;
    QWidget* m_pStateRow = nullptr;
    QLabel* m_pState = nullptr;
    QLabel* m_pStateCaption = nullptr;
    QWidget* m_pReadings = nullptr;
    widgets::LevelRing* m_pConfidenceRing = nullptr;
    QHash<QString, QLabel*> m_readings;
    QWidget* m_pMeta = nullptr;
    QHash<QString, QLabel*> m_metadata;
    QHash<QString, QWidget*> m_metadataRows;
    QScrollArea* m_pDetails = nullptr;
    QWidget* m_pActions = nullptr;
    QPushButton* m_pPrepare = nullptr;
    QPushButton* m_pExecute = nullptr;
    QPushButton* m_pCancel = nullptr;
};

} // namespace crate
