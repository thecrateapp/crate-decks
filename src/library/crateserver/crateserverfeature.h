#pragma once

#include <QHash>
#include <QList>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "crate/library/cratelibrarystore.h"
#include "crate/library/crateloadcoordinator.h"
#include "crate/ui/cachestatusmodel.h"
#include "library/libraryfeature.h"
#include "util/db/dbconnectionpool.h"
#include "util/parented_ptr.h"

class ControlProxy;
class ControlPushButton;
class KeyboardEventFilter;
class QNetworkAccessManager;
class PlayerInfo;
class TrackCollectionManager;
class TreeItem;
class WLibrary;
class WLibrarySidebar;

namespace crate {
namespace recs {
class AutoDjRecommendationQueue;
class Compatibles;
class CompatiblesModel;
class CompatiblesView;
class CratePathSource;
class CrateRadioSource;
struct PathRequest;
struct RadioSeed;
} // namespace recs
namespace widgets {
class CamelotWheelModel;
} // namespace widgets
namespace writeback {
class CratePlaylistEdit;
} // namespace writeback
class AssistPanel;
class AutoDJSmartMixAdapter;
class CrateServers;
class LocalStemSeparator;
class PlanClient;
class ModelStore;
class SetHistory;
class SmartMixAssist;
class StemJobs;
class StemSync;
} // namespace crate

namespace crate::library {

class CrateLoadCoordinator;
class CratePrefetcher;
class CrateSessionPins;
class CrateServerTrackModel;

/// The Crate section of the library sidebar (plan F3.C-PR2, design §6 and
/// D4): the library of every Crate server this installation is signed in
/// to, as Decks mirrors it in the crateserver_ tables.
///
/// With one server the feature's title is "Crate · <host>" and its children
/// are Library, Playlists, Crates, Genres, Moods, Recent and Likes; with
/// more, each server is a child with those sections. Genres, Moods and
/// Recent only appear when the server serves such containers. Library is
/// every track Decks has synced from the server.
///
/// The sidebar is painted from the local tables at once. The first time a
/// server's node is expanded or activated in a run, its capabilities are
/// checked and, if they announce `library_cursor`, its tree is fetched and
/// its cache invalidations are listened to. Activating a container shows
/// its stored tracks at once and fetches them if they need a sync.
///
/// Last comes Cache, with the audio cache's use as its count: it shows the
/// cache's view (CacheStatusView) in the library pane. The Crate
/// preferences drive the loads: Auto DJ's prefetch and the import policy of
/// Crate's analysis (CratePreferences).
class CrateServerFeature : public LibraryFeature {
    Q_OBJECT
  public:
    struct Dependencies {
        TrackCollectionManager* pTrackCollectionManager = nullptr;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// Not owned. Without one, CrateServers::instance() once there is
        /// one: CoreServices creates it after the library.
        CrateServers* pServers = nullptr;
        /// The players' tracks, so that a Crate track loaded some other way
        /// (Auto DJ) takes Crate's analysis, and the decks Smart Mix plays.
        /// Optional, never PlayerInfo::instance() in its place: without one
        /// there is no Smart Mix (Auto DJ's mode and the assistant). Not
        /// owned.
        PlayerInfo* pPlayerInfo = nullptr;
    };

    /// The library's track collection and database.
    CrateServerFeature(Library* pLibrary, UserSettingsPointer pConfig);
    CrateServerFeature(Library* pLibrary,
            UserSettingsPointer pConfig,
            const Dependencies& dependencies);
    ~CrateServerFeature() override;

    QVariant title() override;
    TreeItemModel* sidebarModel() const override;
    bool hasTrackTable() override {
        return true;
    }
    void bindSidebarWidget(WLibrarySidebar* pSidebarWidget) override;
    void bindLibraryWidget(WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) override;

    /// The name of the cache's view in the library pane.
    static const QString kCacheViewName;
    /// The name of the Smart Mix assistant's view (AssistPanel).
    static const QString kSmartMixViewName;
    /// The name of the Compatibles view (plan F5.C-PR1).
    static const QString kCompatiblesViewName;
    /// `[CrateDecks],show_smart_mix`: the CrateDecks skin's toolbar button,
    /// on while the assistant shows. Declared by the skin's manifest.
    static const ConfigKey& showSmartMixKey();
    /// `[CrateDecks],like_on_air`: likes the Crate track on air, or stops
    /// liking it (plan F7.C-PR2), for keyboard and controller mappings.
    static const ConfigKey& likeOnAirKey();
    /// Likes the Crate track on air, or stops liking it. False if no Crate
    /// track is on air.
    bool toggleLikeOnAir();
    /// The Smart Mix assist mode; nullptr until there is a server, and
    /// without the decks' PlayerInfo.
    SmartMixAssist* smartMixAssist() const {
        return m_pAssist.get();
    }
    /// What the cache's view shows.
    CacheStatusModel* cacheStatus() const {
        return m_pCacheStatus;
    }
    /// The Compatibles view's logic, which follows the master deck or the
    /// selection.
    recs::Compatibles* compatibles() const {
        return m_pCompatibles.get();
    }
    /// Crate's Radio as a source of Auto DJ (plan F5.C-PR2); nullptr until
    /// there is a server.
    recs::CrateRadioSource* radio() const {
        return m_pRadio.get();
    }
    /// The tracks of the radio, which its sidebar item shows.
    CrateServerTrackModel* radioModel() const {
        return m_pRadioModel.get();
    }

    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void onLazyChildExpandation(const QModelIndex& index) override;
    void onRightClickChild(const QPoint& globalPos, const QModelIndex& index) override;
    /// Crate tracks dropped onto a Crate playlist join it (plan F7.C-PR3).
    bool dropAcceptChild(const QModelIndex& index,
            const QList<QUrl>& urls,
            QObject* pSource) override;
    bool dragMoveAcceptChild(const QModelIndex& index, const QList<QUrl>& urls) override;

    /// Edits the server's playlists (plan F7.C-PR3).
    writeback::CratePlaylistEdit* playlistEdit() const {
        return m_pPlaylistEdit.get();
    }
    /// Creates a playlist on the server with the Crate tracks of the Decks
    /// playlist `playlistId`, under its name.
    void exportPlaylistToCrate(const QString& serverId, int playlistId);

    /// Loads Crate tracks into decks; nullptr until there is a server.
    CrateLoadCoordinator* loadCoordinator() const {
        return m_pLoadCoordinator.get();
    }
    CratePrefetcher* prefetcher() const {
        return m_pPrefetcher.get();
    }
    /// The stems of Smart Mix playlists and tracks (plan ST5.PR2); nullptr
    /// until there is a server.
    StemJobs* stemJobs() const {
        return m_pStemJobs.get();
    }
    /// Keeps the Crate tracks in a player or in the Auto DJ queue cached;
    /// nullptr until there is a server.
    CrateSessionPins* sessionPins() const {
        return m_pSessionPins.get();
    }

    /// What an offline set of a container downloads.
    struct OfflineSet {
        /// The tracks that are not cached yet.
        QList<CrateTrackRef> tracks;
        /// Their size, of the tracks whose size is known.
        qint64 knownBytes = 0;
        int unknownSizes = 0;
    };
    /// Reads the container's tracks off the main thread, then `ready`.
    void readOfflineSet(const QString& serverId,
            qint64 containerId,
            std::function<void(const OfflineSet&)> ready);
    /// Downloads and pins the tracks of an offline set of the container.
    void downloadOfflineSet(qint64 containerId, const OfflineSet& set);
    /// Reads a container's tracks, in order, off the main thread.
    void readContainerTracks(const QString& serverId,
            qint64 containerId,
            std::function<void(const QList<CrateTrackRef>&)> ready);
    /// The tracks of the server's playlist `playlistId`, syncing them first
    /// if Decks does not have them: nullopt if the tree has no such
    /// playlist yet, or the sync fails. For StemJobs.
    void readSmartMixPlaylist(const QString& serverId,
            qint64 playlistId,
            const std::function<void(std::optional<QList<CrateTrackRef>>)>& done);

  signals:
    /// The sidebar shows what the tables hold of the server now.
    void sidebarUpdated(const QString& serverId);

  private:
    struct Server;
    /// What a sidebar item stands for.
    struct Node {
        enum class Kind {
            None,
            Server,
            Library,
            Section,
            Container,
            SmartMix,
            Compatibles,
            Radio,
            Cache,
        };
        Kind kind = Kind::None;
        QString serverId;
        int section = -1;
        qint64 containerId = 0;
    };
    /// What the sidebar shows of a server, read off the main thread.
    struct SidebarData {
        bool ok = false;
        QList<StoredContainer> containers;
        /// The whole library's count, as the server's tree gave it last.
        std::optional<qint64> libraryTrackCount;
        /// The tracks synced from the server so far.
        qint64 syncedTracks = 0;
    };

    CrateServers* servers();
    void watchServers(CrateServers* pServers);
    /// Rebuilds the servers and the whole tree when the saved servers change.
    void updateServers();
    /// The coordinator and the prefetcher, once there is a server.
    void startLoading();
    void stopLoading();
    void onPrefetchAutoDjChanged(bool enabled);
    /// Starts the stems of a server that serves them, once it is ready:
    /// its capabilities for this account say `stems` (and `stem_upload`).
    void startStems(const QString& serverId);
    /// Asks before downloading the tracks of a Smart Mix playlist.
    /// Asks before downloading the separation model, with its size.
    void confirmModelDownload(qint64 bytes, std::function<void(bool)> answer);
    void confirmSmartMixDownload(const QString& serverId,
            qint64 playlistId,
            int tracks,
            qint64 bytes,
            std::function<void(bool)> answer);
    /// The Cache item's label, with the cache's use.
    QString cacheLabel() const;
    void updateCacheItem();
    /// The titles of cached tracks, for the cache's view.
    void lookUpTitles(const QList<cache::TrackKey>& tracks,
            std::function<void(const QHash<QString, CacheTrackTitle>&)> ready);
    QString offlineSetName(const QString& serverId, const QString& reason) const;
    /// Applies the server's mix profile of a track to it, once fetched.
    void applyAnalysis(const TrackPointer& pTrack,
            const QString& serverId,
            const QString& entityUid);
    /// Applies Crate's profile of the track, as the preferences say: at load
    /// and when the user's correction of it is cleared.
    void applyProfile(const TrackPointer& pTrack,
            const QString& serverId,
            const QString& entityUid);
    Server* server(const QString& serverId) const;
    QString titleOf(const QString& serverId) const;

    /// The item under which a server's sections are.
    TreeItem* serverItem(const QString& serverId) const;
    void appendDefaultSections(TreeItem* pParent, const QString& serverId) const;
    void loadSidebar(const QString& serverId);
    void applySidebar(const QString& serverId, const SidebarData& data);
    QModelIndex indexOf(const TreeItem* pItem) const;

    Node nodeOf(const QModelIndex& index) const;
    /// Checks the server's capabilities once per run, then calls `ready`.
    void whenReady(const QString& serverId, std::function<void()> ready);
    void onCapabilities(const QString& serverId, bool ok, const QByteArray& json);
    void showLibrary(const QString& serverId);
    void showContainer(const QString& serverId, qint64 containerId);
    void syncIfNeeded(const QString& serverId, qint64 containerId);
    void expandOwnItems();
    /// The search box's `text` on `pModel`; true when the DJ search takes it.
    bool onSearch(CrateServerTrackModel* pModel, const QString& text);
    void onSearchResults(const QString& serverId, qint64 containerId, const QString& query);
    /// Drops the search: the user went to another node.
    void endSearch();
    /// Shows the assistant; from the toolbar, it remembers the sidebar item
    /// to go back to.
    void showSmartMix(bool fromToolbar);
    void onShowSmartMix(double value);
    void onLibraryViewChanged();
    TreeItem* smartMixItem() const;
    void showCompatibles();
    /// Every track model of the section.
    QList<CrateServerTrackModel*> trackModels() const;
    /// The Crate entries of the track menu: Radio's and Music Paths'.
    QList<QAction*> trackActions(
            const QList<CrateTrackRef>& tracks, const QStringList& labels, QObject* pParent);
    void startRadio(const QString& serverId, const recs::RadioSeed& seed);
    /// Asks how many tracks and where to, then for the path from `from` to
    /// `to` (plan F5.C-PR3).
    void askForPath(const CrateTrackRef& from,
            const QString& fromLabel,
            const CrateTrackRef& to,
            const QString& toLabel);
    /// The container of the server's playlist `playlistId`; 0 if the tree
    /// read last has none.
    qint64 playlistContainer(const QString& serverId, qint64 playlistId) const;
    /// The server's playlist of a container; 0 for another kind.
    qint64 playlistIdOf(qint64 containerId) const;
    /// Lets the container's track table edit the playlist, if it is one.
    void editPlaylistIn(const QString& serverId, qint64 containerId);
    /// The Crate tracks of these files join the server's playlist.
    void addToPlaylist(const QString& serverId, qint64 playlistId, const QStringList& locations);
    void showWarning(const QString& message);
    /// "Save correction to Crate" and "Clear correction on Crate" for a
    /// Crate track, when they apply: the entries of upstream's track menus
    /// (crate::ui::setTrackMenuProvider()).
    QList<QAction*> correctionActions(const TrackPointer& pTrack, QObject* pParent);
    /// The master deck's Crate track, if it has one.
    std::optional<cache::TrackKey> crateTrackOnAir() const;
    void showRadio();
    void updateRadioRows();
    /// Follows the selection of the library's track table while it shows a
    /// Crate list, uncached tracks too, for the Compatibles view.
    void watchSelection();

    const Dependencies m_dependencies;
    QPointer<CrateServers> m_pServers;
    parented_ptr<TreeItemModel> m_pSidebarModel;
    parented_ptr<CrateServerTrackModel> m_pContainerModel;
    parented_ptr<CrateServerTrackModel> m_pLibraryModel;
    /// The results of the DJ search.
    parented_ptr<CrateServerTrackModel> m_pSearchModel;
    /// The server of the container on screen.
    QString m_containerServerId;
    /// The model the search started from, shown again when it is cleared.
    CrateServerTrackModel* m_pSearchedModel = nullptr;
    QString m_searchServerId;
    bool m_searching = false;
    bool m_resultsShown = false;
    QPointer<WLibrarySidebar> m_pSidebarWidget;
    QPointer<WLibrary> m_pLibraryWidget;
    QPointer<AssistPanel> m_pAssistPanel;
    /// [CrateDecks],show_smart_mix, while the skin has it.
    ControlProxy* m_pShowSmartMix = nullptr;
    /// The sidebar item the toolbar's Smart Mix button goes back to.
    QPersistentModelIndex m_beforeSmartMix;
    /// Saved servers, in the order CrateServers keeps them.
    QStringList m_serverIds;
    std::map<QString, std::unique_ptr<Server>> m_servers;
    /// The last tree read of each server, by container id.
    QHash<qint64, StoredContainer> m_containers;
    std::unique_ptr<CrateLoadCoordinator> m_pLoadCoordinator;
    std::unique_ptr<CratePrefetcher> m_pPrefetcher;
    /// The Smart Mix plans of each server, and Auto DJ's Crate Smart Mix,
    /// which asks them (plan F6.C-PR3).
    std::map<QString, std::unique_ptr<PlanClient>> m_planClients;
    /// The transitions Auto DJ and the assistant played, for smart-mix-v3
    std::unique_ptr<SetHistory> m_pSetHistory;
    std::unique_ptr<AutoDJSmartMixAdapter> m_pSmartMix;
    std::unique_ptr<SmartMixAssist> m_pAssist;
    std::unique_ptr<CrateSessionPins> m_pSessionPins;
    /// Local separation (STEM-D7), with ONNX Runtime only.
    std::unique_ptr<QNetworkAccessManager> m_pModelNetwork;
    std::unique_ptr<ModelStore> m_pModels;
    std::unique_ptr<LocalStemSeparator> m_pLocalSeparator;
    std::unique_ptr<StemSync> m_pStemSync;
    std::unique_ptr<StemJobs> m_pStemJobs;
    /// Servers whose capabilities let this account upload stem analyses.
    QSet<QString> m_stemUploads;
    /// The Compatibles view (plan F5.C-PR1).
    parented_ptr<recs::CompatiblesModel> m_pCompatiblesModel;
    /// Radio (plan F5.C-PR2) and Music Paths (F5.C-PR3) fill Auto DJ's queue
    parented_ptr<CrateServerTrackModel> m_pRadioModel;
    parented_ptr<CacheStatusModel> m_pCacheStatus;
    std::unique_ptr<recs::Compatibles> m_pCompatibles;
    std::unique_ptr<widgets::CamelotWheelModel> m_pWheelModel;
    std::unique_ptr<recs::AutoDjRecommendationQueue> m_pRecommendations;
    std::unique_ptr<recs::CrateRadioSource> m_pRadio;
    std::unique_ptr<recs::CratePathSource> m_pPaths;
    QPointer<recs::CompatiblesView> m_pCompatiblesView;
    QMetaObject::Connection m_selectionConnection;
    std::unique_ptr<writeback::CratePlaylistEdit> m_pPlaylistEdit;
    /// likeOnAirKey(), unless another feature made it first.
    std::unique_ptr<ControlPushButton> m_pLikeOnAir;
    QThreadPool m_pool;
};

} // namespace crate::library
