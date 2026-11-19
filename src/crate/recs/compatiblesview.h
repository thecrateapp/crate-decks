#pragma once

#include <QLabel>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <chrono>
#include <functional>
#include <optional>

#include "crate/cache/audiocache.h"
#include "crate/client/crateresult.h"
#include "crate/recs/recsjson.h"
#include "crate/ui/widgets/camelotwheelmodel.h"
#include "library/libraryview.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class ControlProxy;
class QItemSelectionModel;
class QPushButton;

namespace crate {
class Client;
class Reply;
} // namespace crate

namespace crate::library {
struct CrateTrackRef;
} // namespace crate::library

namespace crate::widgets {
class CamelotWheel;
class LevelRing;
} // namespace crate::widgets

namespace crate::recs {

class CompatiblesModel;

/// Options of crate::recs::Compatibles.
struct CompatiblesOptions {
    /// Quiet time after the followed track changes before the server is
    /// asked: a DJ flicking through decks or rows asks once.
    std::chrono::milliseconds debounce{400};
    /// Results asked for; the server gives at most 100.
    int limit = 50;
    /// The best results downloaded ahead when prewarmEnabled() says so.
    int prewarmCount = 5;
    /// The Client of a server; nullptr if it has none.
    std::function<Client*(const QString& serverId)> clientFor;
    /// The Crate track whose cached file `location` is
    /// (cache::AudioCache::trackOfFile); nullopt for any other file.
    std::function<std::optional<cache::TrackKey>(const QString& location)> trackOfFile;
    mixxx::DbConnectionPoolPtr pDbConnectionPool;
    /// The Crate preference "Prewarm compatibles"; off without one.
    std::function<bool()> prewarmEnabled;
    /// Downloads tracks ahead (CrateLoadCoordinator::download with the
    /// prefetch priority).
    std::function<void(const QList<library::CrateTrackRef>& tracks)> prewarm;
    /// Attempts of a write while the database is busy.
    int writeAttempts = 4;
    std::chrono::milliseconds writeRetryDelay{25};
};

/// The logic of the Compatibles view (plan F5.C-PR1, design §7.5), without
/// widgets: it follows a track, the master deck's or the selected one, and
/// shows in its CompatiblesModel the tracks the server finds compatible with
/// it (`GET /api/tracks/by-entity/{uid}/compatible?context=…&limit=50`).
///
/// Lives on the main thread. The server is asked once the followed track has
/// stayed for `debounce`, and a new track cancels the request in flight. The
/// answer is stored and read back off the main thread (storeCompatibles()).
/// A followed track that is not a Crate track empties the list: the view
/// says to load one. With the preference on, the best `prewarmCount` results
/// are downloaded ahead, so that the next track loads at once.
class Compatibles : public QObject {
    Q_OBJECT
  public:
    using Options = CompatiblesOptions;

    enum class Follow {
        /// The track of the deck on air (PlayerInfo's current playing deck).
        MasterDeck,
        /// The track selected in the library.
        Selection,
    };

    enum class State {
        /// No Crate track to follow.
        NoCrateTrack,
        /// Waiting for the server.
        Loading,
        /// The model shows the results.
        Ready,
        /// The server has no Smart Mix analysis of the followed track.
        NoProfile,
        Failed,
    };

    /// `pModel` must outlive it.
    Compatibles(CompatiblesModel* pModel, Options options, QObject* parent = nullptr);
    ~Compatibles() override;

    Follow follow() const {
        return m_follow;
    }
    void setFollow(Follow follow);
    CompatibleContext context() const {
        return m_context;
    }
    void setContext(CompatibleContext context);

    /// The master deck's track changed (PlayerInfo::currentPlayingTrackChanged).
    void setMasterTrack(const TrackPointer& pTrack);
    /// The deck on air changed (PlayerInfo::currentPlayingDeckChanged): its
    /// group ("[Channel1]"), or empty for none.
    void setMasterDeck(const QString& group);
    const QString& masterDeck() const {
        return m_masterDeck;
    }
    /// The library's selection changed: a track with a file, local or cached.
    void setSelectedTrack(const TrackPointer& pTrack);
    /// The library's selection changed to a Crate track, cached or not;
    /// nullopt for none.
    void setSelectedCrateTrack(const std::optional<cache::TrackKey>& track);

    /// The Crate track followed now; nullopt if there is none.
    const std::optional<cache::TrackKey>& followedTrack() const {
        return m_followed;
    }
    State state() const {
        return m_state;
    }
    /// "Artist - Title" of the followed track, from its deck or library
    /// track; empty if unknown.
    const QString& followedLabel() const {
        return m_followedLabel;
    }
    CompatiblesModel* model() const {
        return m_pModel;
    }

  signals:
    void stateChanged(crate::recs::Compatibles::State state);
    void masterDeckChanged(const QString& group);

  private:
    struct Followed {
        std::optional<cache::TrackKey> track;
        QString label;
        std::optional<double> bpm;
    };

    Followed followedOf(const TrackPointer& pTrack) const;
    void apply();
    void setFollowed(const Followed& followed);
    void send();
    void cancel();
    void onReply(Reply* pReply, quint64 generation, const cache::TrackKey& seed);
    void setState(State state);

    CompatiblesModel* const m_pModel;
    const Options m_options;
    Follow m_follow = Follow::MasterDeck;
    QString m_masterDeck;
    CompatibleContext m_context = CompatibleContext::Auto;
    Followed m_master;
    Followed m_selected;
    std::optional<cache::TrackKey> m_followed;
    QString m_followedLabel;
    std::optional<double> m_followedBpm;
    State m_state = State::NoCrateTrack;
    QTimer m_debounce;
    /// Bumped by every new request and cancel(), so a late answer is
    /// dropped.
    quint64 m_generation = 0;
    QPointer<Reply> m_pReply;
    /// One worker: answers are stored in the order they came.
    QThreadPool m_pool;
};

/// The Compatibles view of the Crate section (GU1 mockup
/// library-compatibles, plan V3.PR2): a thin layer over Compatibles.
///
/// - A 56 px head: the view's name, what it follows ("DECK A · LATTICE ·
///   8A · 124.00 BPM"), the Follow and Context chips, how many tracks and
///   how long ago, and the library's maximise button (the search row, which
///   this view does not use, hides while it shows).
/// - A band on top: the Camelot wheel, scaled to the pane's height between
///   its minimum and the mockup's 470 px; the "Master key" summary (key and
///   track, Results / Harmonic / Cached and a meter per relation); and a side
///   column with the wheel's legend, the cache of the best five and the
///   selected track, which loads into the other deck or opens the Smart Mix
///   assistant for that edge. A click on a key filters the list by it.
/// - Where the band does not fit at its minimum, a one-line strip takes its
///   place (key and relation counts, and a toggle that shows the wheel
///   anyway), and the list takes the pane.
/// - The track table under it, with the mockup's columns.
///
/// The view lays its parts out itself and asks no minimum size of its own,
/// so that it never makes the window grow. Without a Crate track it says to
/// load one.
class CompatiblesView : public QWidget, public LibraryView {
    Q_OBJECT
  public:
    /// `pTable` shows the model: a WTrackTableView in the application. The
    /// view takes it as a child. `pWheelModel` is optional: without it there
    /// is no wheel.
    CompatiblesView(Compatibles* pCompatibles,
            QWidget* pTable,
            widgets::CamelotWheelModel* pWheelModel,
            QWidget* pParent = nullptr);
    ~CompatiblesView() override;

    QLabel* hint() const {
        return m_pHint;
    }
    QWidget* head() const {
        return m_pHead;
    }
    QWidget* band() const {
        return m_pBand;
    }
    QWidget* strip() const {
        return m_pStrip;
    }
    QWidget* table() const {
        return m_pTable;
    }
    QPushButton* followChip(Compatibles::Follow follow) const;
    QPushButton* contextChip(CompatibleContext context) const;
    widgets::CamelotWheel* wheel() const {
        return m_pWheel;
    }
    QPushButton* wheelToggle() const {
        return m_pWheelToggle;
    }
    QPushButton* loadButton() const {
        return m_pLoad;
    }
    QPushButton* planButton() const {
        return m_pPlan;
    }
    QLabel* headMeta() const {
        return m_pHeadMeta;
    }
    QLabel* countLabel() const {
        return m_pCount;
    }
    QLabel* summaryTitle() const {
        return m_pSummaryTitle;
    }
    QLabel* selectedLabel() const {
        return m_pSelectedEyebrow;
    }
    const QList<QLabel*>& badges() const {
        return m_badges;
    }
    QLabel* stripCounts() const {
        return m_pStripCounts;
    }
    QLabel* prewarmLabel() const {
        return m_pPrewarmLabel;
    }
    const QList<widgets::LevelRing*>& prewarmRings() const {
        return m_prewarmRings;
    }
    /// Whether the band gave way to the strip.
    bool isCompact() const {
        return m_compact;
    }
    /// The deck "Load in deck N" loads into, or empty for none.
    QString targetGroup() const;

    /// The text the hint shows in `state`.
    static QString hintFor(Compatibles::State state);
    /// How long ago the results came: "0.4 s ago", "12 s ago", "3 min ago".
    static QString ageText(qint64 ms);
    /// The deck a track loads into beside the master deck `masterIndex`
    /// (-1 for none): its partner (deck 1 with 2, 3 with 4) if it is
    /// stopped, or else the first other deck that is, as the Smart Mix
    /// assistant pairs them; -1 if every other deck plays.
    static int targetDeck(int masterIndex, int deckCount, const std::function<bool(int)>& plays);

    /// How the band fits a pane: `available` is the height left after the
    /// head and the list's minimum, `paddingY` the band's own, `wheelMinimum`
    /// and `wheelMaximum` the wheel's sizes (the maximum already as wide as
    /// the pane allows) and `content` the height its columns need.
    struct BandLayout {
        bool compact = false;
        int wheel = 0;
        int height = 0;
    };
    static BandLayout bandLayout(
            int available, int paddingY, int wheelMinimum, int wheelMaximum, int content);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // LibraryView
    void onShow() override;
    bool hasFocus() const override;
    void setFocus() override;
    void saveCurrentViewState() override;
    bool restoreCurrentViewState() override;

  signals:
    /// "Plan transition": the selected track went to `group`, and the Smart
    /// Mix assistant should show the edge from the master deck to it.
    void planTransitionRequested(const QString& group);

  protected:
    void resizeEvent(QResizeEvent* pEvent) override;
    void showEvent(QShowEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;

  private:
    QWidget* makeHead();
    QWidget* makeBand();
    QWidget* makeSummary(QWidget* pParent);
    QWidget* makeSide(QWidget* pParent);
    QWidget* makeStrip();
    void update();
    void updateHead();
    void updateSummary();
    void updateCache();
    void updateSelected();
    void updateCount();
    /// Shows the selected track's badges that fit the side column.
    void fitBadges();
    void watchSelection();
    void layoutParts();
    /// The table's selected row, or -1.
    int selectedRow() const;
    /// Loads the selected row into `group`, unless the deck has it already.
    void loadSelected(const QString& group);
    void onKeyClicked(mixxx::track::io::key::ChromaticKey key);

    const QPointer<Compatibles> m_pCompatibles;
    QWidget* const m_pTable;
    widgets::CamelotWheelModel* const m_pWheelModel;
    QWidget* m_pHead = nullptr;
    QLabel* m_pHeadMeta = nullptr;
    QLabel* m_pFollowLabel = nullptr;
    QLabel* m_pContextLabel = nullptr;
    QLabel* m_pCount = nullptr;
    QList<QPushButton*> m_followChips;
    QList<QPushButton*> m_contextChips;
    QPushButton* m_pMaximize = nullptr;
    QWidget* m_pBand = nullptr;
    widgets::CamelotWheel* m_pWheel = nullptr;
    QWidget* m_pSummary = nullptr;
    QLabel* m_pDeckBadge = nullptr;
    QLabel* m_pSummaryTitle = nullptr;
    QLabel* m_pSummaryTrack = nullptr;
    QList<QLabel*> m_metrics;
    QList<QLabel*> m_shareLabels;
    QList<QLabel*> m_shareCounts;
    QList<QWidget*> m_shareMeters;
    QWidget* m_pSide = nullptr;
    QLabel* m_pPrewarmLabel = nullptr;
    QList<widgets::LevelRing*> m_prewarmRings;
    QWidget* m_pSelected = nullptr;
    QLabel* m_pSelectedEyebrow = nullptr;
    QLabel* m_pSelectedTitle = nullptr;
    QLabel* m_pSelectedArtist = nullptr;
    QList<QLabel*> m_badges;
    QPushButton* m_pLoad = nullptr;
    QPushButton* m_pPlan = nullptr;
    QWidget* m_pStrip = nullptr;
    QLabel* m_pStripKey = nullptr;
    QLabel* m_pStripCounts = nullptr;
    QPushButton* m_pWheelToggle = nullptr;
    QLabel* m_pHint = nullptr;
    QPointer<QItemSelectionModel> m_pSelection;
    QTimer m_clock;
    ControlProxy* m_pMaximized = nullptr;
    bool m_compact = false;
    /// The side column's room for its content, in pixels; 0 if unknown.
    int m_sideWidth = 0;
};

} // namespace crate::recs
