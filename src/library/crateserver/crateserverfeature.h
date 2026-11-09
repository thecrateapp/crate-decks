#pragma once

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <functional>
#include <map>
#include <memory>

#include "crate/library/cratelibrarystore.h"
#include "library/libraryfeature.h"
#include "util/db/dbconnectionpool.h"
#include "util/parented_ptr.h"

class TrackCollectionManager;
class TreeItem;
class WLibrarySidebar;

namespace crate {
class CrateServers;
} // namespace crate

namespace crate::library {

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
class CrateServerFeature : public LibraryFeature {
    Q_OBJECT
  public:
    struct Dependencies {
        TrackCollectionManager* pTrackCollectionManager = nullptr;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// Not owned. Without one, CrateServers::instance() once there is
        /// one: CoreServices creates it after the library.
        CrateServers* pServers = nullptr;
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

    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void onLazyChildExpandation(const QModelIndex& index) override;
    void onRightClickChild(const QPoint& globalPos, const QModelIndex& index) override;

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
        qint64 libraryTracks = 0;
    };

    CrateServers* servers();
    void watchServers(CrateServers* pServers);
    /// Rebuilds the servers and the whole tree when the saved servers change.
    void updateServers();
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

    const Dependencies m_dependencies;
    QPointer<CrateServers> m_pServers;
    parented_ptr<TreeItemModel> m_pSidebarModel;
    parented_ptr<CrateServerTrackModel> m_pContainerModel;
    parented_ptr<CrateServerTrackModel> m_pLibraryModel;
    QPointer<WLibrarySidebar> m_pSidebarWidget;
    /// Saved servers, in the order CrateServers keeps them.
    QStringList m_serverIds;
    std::map<QString, std::unique_ptr<Server>> m_servers;
    /// The last tree read of each server, by container id.
    QHash<qint64, StoredContainer> m_containers;
    QThreadPool m_pool;
};

} // namespace crate::library
