#pragma once

#include <QHash>
#include <QList>
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

class KeyboardEventFilter;
class PlayerInfo;
class TrackCollectionManager;
class TreeItem;
class WLibrary;
class WLibrarySidebar;

namespace crate {
class CrateServers;
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
        /// (Auto DJ) takes Crate's analysis. Optional; not owned.
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
    /// What the cache's view shows.
    CacheStatusModel* cacheStatus() const {
        return m_pCacheStatus;
    }

    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void onLazyChildExpandation(const QModelIndex& index) override;
    void onRightClickChild(const QPoint& globalPos, const QModelIndex& index) override;

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
            std::function<void(std::optional<QList<CrateTrackRef>>)> done);

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
    /// Saved servers, in the order CrateServers keeps them.
    QStringList m_serverIds;
    std::map<QString, std::unique_ptr<Server>> m_servers;
    /// The last tree read of each server, by container id.
    QHash<qint64, StoredContainer> m_containers;
    std::unique_ptr<CrateLoadCoordinator> m_pLoadCoordinator;
    std::unique_ptr<CratePrefetcher> m_pPrefetcher;
    std::unique_ptr<CrateSessionPins> m_pSessionPins;
    std::unique_ptr<StemSync> m_pStemSync;
    std::unique_ptr<StemJobs> m_pStemJobs;
    /// Servers whose capabilities let this account upload stem analyses.
    QSet<QString> m_stemUploads;
    parented_ptr<CacheStatusModel> m_pCacheStatus;
    QThreadPool m_pool;
};

} // namespace crate::library
