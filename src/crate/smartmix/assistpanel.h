#pragma once

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "library/libraryview.h"

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace crate {

class SmartMixAssist;

namespace widgets {
class LevelRing;
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
///   tempo adjustment, bass swap, gain and planner, and the Prepare, Execute
///   and Cancel buttons with the controls a controller maps.
///
/// The plan's timeline (TransitionPlanView, plan V3.PR1b) is not drawn yet:
/// the mockup's stage under the host stays empty.
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

  private:
    QWidget* makeHead();
    QWidget* makeHost();
    QWidget* makeInspector();
    void showAssist();
    void showTimes();
    void setAutoDj(bool on);

    QPointer<SmartMixAssist> m_pAssist;
    QTimer m_clock;

    QLabel* m_pDecks = nullptr;
    QPushButton* m_pAssistChip = nullptr;
    QPushButton* m_pAutoDjChip = nullptr;
    QLabel* m_pStatusDot = nullptr;
    QLabel* m_pStatus = nullptr;

    QWidget* m_pHost = nullptr;
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
    QPushButton* m_pPrepare = nullptr;
    QPushButton* m_pExecute = nullptr;
    QPushButton* m_pCancel = nullptr;
};

} // namespace crate
