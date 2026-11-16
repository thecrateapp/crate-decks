#pragma once

#include <QLabel>
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

class QComboBox;
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

/// The Compatibles view of the Crate section: a thin layer over Compatibles.
/// A bar names the followed track and lets the DJ pick what to follow and
/// the context; the Camelot wheel shows the keys of the results and filters
/// the list by the key clicked (again: every key); the track table shows the
/// model. Without a Crate track it says to load one.
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

    QLabel* hint() const {
        return m_pHint;
    }
    QComboBox* followChoice() const {
        return m_pFollow;
    }
    QComboBox* contextChoice() const {
        return m_pContext;
    }
    widgets::CamelotWheel* wheel() const {
        return m_pWheel;
    }

    /// The text the hint shows in `state`.
    static QString hintFor(Compatibles::State state);

    // LibraryView
    void onShow() override;
    bool hasFocus() const override;
    void setFocus() override;
    void saveCurrentViewState() override;
    bool restoreCurrentViewState() override;

  private:
    void update();
    void onKeyClicked(mixxx::track::io::key::ChromaticKey key);

    const QPointer<Compatibles> m_pCompatibles;
    QWidget* const m_pTable;
    widgets::CamelotWheelModel* const m_pWheelModel;
    QLabel* m_pTitle;
    QLabel* m_pHint;
    QComboBox* m_pFollow;
    QComboBox* m_pContext;
    widgets::CamelotWheel* m_pWheel = nullptr;
};

} // namespace crate::recs
