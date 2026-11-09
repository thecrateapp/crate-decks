#include "library/crateserver/crateserverfeature.h"

#include <QFutureWatcher>
#include <QIconEngine>
#include <QJsonDocument>
#include <QMenu>
#include <QPainter>
#include <QScopeGuard>
#include <QSet>
#include <QtConcurrent>

#include "crate/client/cratecapabilities.h"
#include "crate/client/crateclient.h"
#include "crate/crateservers.h"
#include "crate/library/crateeventsubscriber.h"
#include "crate/library/cratelibraryjson.h"
#include "crate/library/cratelibrarysync.h"
#include "library/crateserver/crateservertrackmodel.h"
#include "library/library.h"
#include "library/library_prefs.h"
#include "library/librarytablemodel.h"
#include "library/sidebarmodel.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"
#include "widget/wlibrarysidebar.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::CrateServerFeature");

const QString kIconName = QStringLiteral("crateserver");

/// The sections of a server, in the sidebar's order (GU1 mockups).
const QList<Section> kSections = {
        Section::Playlists,
        Section::Crates,
        Section::Genres,
        Section::Moods,
        Section::Recent,
        Section::Likes,
};

/// An icon that names the Solar icon the CrateDecks sidebar paints
/// (SidebarDelegate reads QIcon::name()) and paints Mixxx's own in other
/// skins: Mixxx's SidebarModel forwards a child item's QIcon but not a
/// custom role.
class NamedIconEngine : public QIconEngine {
  public:
    NamedIconEngine(QString name, QString fallbackPath)
            : m_name(std::move(name)),
              m_fallbackPath(std::move(fallbackPath)),
              m_fallback(m_fallbackPath) {
    }

    void paint(QPainter* pPainter, const QRect& rect, QIcon::Mode mode, QIcon::State state)
            override {
        m_fallback.paint(pPainter, rect, Qt::AlignCenter, mode, state);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return m_fallback.pixmap(size, mode, state);
    }
    QIconEngine* clone() const override {
        return new NamedIconEngine(m_name, m_fallbackPath);
    }
    QString key() const override {
        return QStringLiteral("CrateNamedIconEngine");
    }
    QString iconName() override {
        return m_name;
    }

  private:
    const QString m_name;
    const QString m_fallbackPath;
    const QIcon m_fallback;
};

QIcon namedIcon(const QString& name, const QString& mixxxIcon) {
    return QIcon(new NamedIconEngine(name,
            QStringLiteral(":/images/library/ic_library_%1.svg").arg(mixxxIcon)));
}

QIcon sectionIcon(Section section) {
    switch (section) {
    case Section::Playlists:
        return namedIcon(QStringLiteral("crate-playlists"), QStringLiteral("playlist"));
    case Section::Crates:
        return namedIcon(QStringLiteral("crate-crates"), QStringLiteral("crates"));
    case Section::Genres:
        return namedIcon(QStringLiteral("crate-genres"), QStringLiteral("crates"));
    case Section::Moods:
        return namedIcon(QStringLiteral("crate-moods"), QStringLiteral("crates"));
    case Section::Recent:
        return namedIcon(QStringLiteral("crate-recent"), QStringLiteral("history"));
    case Section::Likes:
        return namedIcon(QStringLiteral("crate-likes"), QStringLiteral("tracks"));
    }
    return QIcon();
}

QString sectionName(Section section) {
    switch (section) {
    case Section::Playlists:
        return CrateServerFeature::tr("Playlists");
    case Section::Crates:
        return CrateServerFeature::tr("Crates");
    case Section::Genres:
        return CrateServerFeature::tr("Genres");
    case Section::Moods:
        return CrateServerFeature::tr("Moods");
    case Section::Recent:
        return CrateServerFeature::tr("Recent");
    case Section::Likes:
        return CrateServerFeature::tr("Likes");
    }
    return QString();
}

/// "Name (12)", which the CrateDecks sidebar splits into a label and a
/// count on the right, as Mixxx's own features write their counts.
QString withCount(const QString& label, std::optional<qint64> count) {
    if (!count || *count <= 0) {
        return label;
    }
    return QStringLiteral("%1 (%2)").arg(label).arg(*count);
}

/// What a container's sidebar item counts: its synced tracks, else the
/// server's count when it has one.
std::optional<qint64> countOf(const StoredContainer& container) {
    if (const auto synced = container.syncedTrackCount()) {
        return synced;
    }
    return container.trackCount;
}

// Item data: what each sidebar item stands for
QString libraryData(const QString& serverId) {
    return QStringLiteral("library\n") + serverId;
}
QString serverData(const QString& serverId) {
    return QStringLiteral("server\n") + serverId;
}
QString sectionData(const QString& serverId, Section section) {
    return QStringLiteral("section\n%1\n%2").arg(serverId).arg(static_cast<int>(section));
}
QString containerData(const QString& serverId, qint64 containerId) {
    return QStringLiteral("container\n%1\n%2").arg(serverId).arg(containerId);
}

/// Likes and Recent are one container each: their item is the container.
bool isSingleContainer(Section section, const QList<StoredContainer>& containers) {
    return (section == Section::Likes || section == Section::Recent) && containers.size() == 1;
}

QStringList childData(const TreeItem* pItem) {
    QStringList data;
    for (const TreeItem* pChild : pItem->children()) {
        data.append(pChild->getData().toString());
    }
    return data;
}

} // namespace

struct CrateServerFeature::Server {
    QString id;
    std::unique_ptr<LibrarySync> pSync;
    std::unique_ptr<EventSubscriber> pEvents;
    enum class Readiness {
        Unknown,
        Checking,
        Ready,
        /// The server does not serve the library (no library_cursor).
        Unsupported,
    };
    Readiness readiness = Readiness::Unknown;
    QList<std::function<void()>> waiting;
    /// The containers asked for in this run, so a clean one is revalidated
    /// once (syncIfNeeded()).
    QSet<qint64> revalidated;
};

CrateServerFeature::CrateServerFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : CrateServerFeature(pLibrary,
                  pConfig,
                  Dependencies{pLibrary->trackCollectionManager(),
                          pLibrary->dbConnectionPool(),
                          nullptr}) {
}

CrateServerFeature::CrateServerFeature(Library* pLibrary,
        UserSettingsPointer pConfig,
        const Dependencies& dependencies)
        : LibraryFeature(pLibrary, pConfig, kIconName),
          m_dependencies(dependencies),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pContainerModel(make_parented<CrateServerTrackModel>(
                  CrateServerTrackModel::Source::Container,
                  dependencies.pTrackCollectionManager->internalCollection()->database(),
                  dependencies.pDbConnectionPool,
                  this)),
          m_pLibraryModel(make_parented<CrateServerTrackModel>(
                  CrateServerTrackModel::Source::Library,
                  dependencies.pTrackCollectionManager->internalCollection()->database(),
                  dependencies.pDbConnectionPool,
                  this)) {
    m_pool.setMaxThreadCount(1);
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
    if (m_dependencies.pServers) {
        watchServers(m_dependencies.pServers);
    }
}

CrateServerFeature::~CrateServerFeature() {
    m_servers.clear();
    m_pool.waitForDone();
}

CrateServers* CrateServerFeature::servers() {
    if (!m_pServers && !m_dependencies.pServers && CrateServers::instance()) {
        watchServers(CrateServers::instance());
    }
    return m_pServers;
}

void CrateServerFeature::watchServers(CrateServers* pServers) {
    m_pServers = pServers;
    connect(pServers, &CrateServers::serversChanged, this, &CrateServerFeature::updateServers);
    // CoreServices destroys the servers before the library: their Clients go
    // first, and the syncs with them
    connect(pServers, &QObject::destroyed, this, [this] {
        // Half destroyed: nothing of it is used any more
        m_pServers = nullptr;
        m_servers.clear();
        m_pool.waitForDone();
        updateServers();
    });
    updateServers();
}

void CrateServerFeature::updateServers() {
    QStringList ids;
    if (m_pServers) {
        for (const Session* pSession : m_pServers->sessions()) {
            const QString& id = pSession->profile().id;
            if (m_pServers->isSaved(id)) {
                ids.append(id);
            }
        }
    }
    if (ids == m_serverIds && m_pSidebarModel->getRootItem()->hasChildren() == !ids.isEmpty()) {
        return;
    }
    m_serverIds = ids;

    // The servers that went, and the ones that came
    for (auto it = m_servers.begin(); it != m_servers.end();) {
        it = ids.contains(it->first) ? std::next(it) : m_servers.erase(it);
    }
    for (const QString& id : std::as_const(ids)) {
        if (m_servers.find(id) != m_servers.end()) {
            continue;
        }
        auto pServer = std::make_unique<Server>();
        pServer->id = id;
        pServer->pSync = std::make_unique<LibrarySync>(
                id, m_pServers->client(id), m_dependencies.pDbConnectionPool);
        LibrarySync* pSync = pServer->pSync.get();
        connect(pSync, &LibrarySync::containersUpdated, this, [this, id] {
            loadSidebar(id);
        });
        connect(pSync, &LibrarySync::containerUpdated, this, [this, id](qint64 containerId) {
            if (m_pContainerModel->containerId() == containerId) {
                m_pContainerModel->select();
            }
            if (m_pLibraryModel->serverId() == id) {
                m_pLibraryModel->select();
            }
            loadSidebar(id);
        });
        connect(pSync,
                &LibrarySync::containersDirty,
                this,
                [this, id](const QList<qint64>& containerIds) {
                    // The one on screen is fetched now; the rest when shown
                    if (containerIds.contains(m_pContainerModel->containerId())) {
                        syncIfNeeded(id, m_pContainerModel->containerId());
                    }
                });
        connect(pSync, &LibrarySync::syncFailed, this, [this, id](qint64 containerId) {
            // Tried again the next time it is shown
            if (Server* pFailed = server(id)) {
                pFailed->revalidated.remove(containerId);
            }
        });
        m_servers.emplace(id, std::move(pServer));
    }

    // The tree: one server's sections right under the feature, or one item
    // per server
    std::unique_ptr<TreeItem> pRoot = TreeItem::newRoot(this);
    if (ids.size() == 1) {
        appendDefaultSections(pRoot.get(), ids.first());
    } else {
        for (const QString& id : std::as_const(ids)) {
            TreeItem* pServerItem = pRoot->appendChild(titleOf(id), serverData(id));
            pServerItem->setIcon(namedIcon(
                    QStringLiteral("crate-server"), QStringLiteral("tracks")));
            appendDefaultSections(pServerItem, id);
        }
    }
    m_pSidebarModel->setRootItem(std::move(pRoot));
    // The title changed with the servers
    emit featureIsLoading(this, false);
    expandOwnItems();
    for (const QString& id : std::as_const(ids)) {
        loadSidebar(id);
    }
}

CrateServerFeature::Server* CrateServerFeature::server(const QString& serverId) const {
    const auto it = m_servers.find(serverId);
    return it == m_servers.end() ? nullptr : it->second.get();
}

QString CrateServerFeature::titleOf(const QString& serverId) const {
    const Session* pSession = m_pServers ? m_pServers->session(serverId) : nullptr;
    if (!pSession) {
        return serverId;
    }
    const QUrl& url = pSession->profile().baseUrl;
    return url.port() > 0 ? QStringLiteral("%1:%2").arg(url.host()).arg(url.port()) : url.host();
}

QVariant CrateServerFeature::title() {
    servers();
    if (m_serverIds.size() == 1) {
        return QStringLiteral("Crate · %1").arg(titleOf(m_serverIds.first()));
    }
    return QStringLiteral("Crate");
}

TreeItemModel* CrateServerFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void CrateServerFeature::appendDefaultSections(TreeItem* pParent, const QString& serverId) const {
    // Before the tree is read: the sections every server has
    TreeItem* pLibrary = pParent->appendChild(tr("Library"), libraryData(serverId));
    pLibrary->setIcon(namedIcon(QStringLiteral("crate-library"), QStringLiteral("tracks")));
    for (const Section section : {Section::Playlists, Section::Crates}) {
        TreeItem* pSection = pParent->appendChild(sectionName(section),
                sectionData(serverId, section));
        pSection->setIcon(sectionIcon(section));
    }
}

TreeItem* CrateServerFeature::serverItem(const QString& serverId) const {
    TreeItem* pRoot = m_pSidebarModel->getRootItem();
    if (m_serverIds.size() == 1) {
        return m_serverIds.first() == serverId ? pRoot : nullptr;
    }
    for (TreeItem* pChild : pRoot->children()) {
        if (pChild->getData().toString() == serverData(serverId)) {
            return pChild;
        }
    }
    return nullptr;
}

void CrateServerFeature::loadSidebar(const QString& serverId) {
    auto* pWatcher = new QFutureWatcher<SidebarData>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher, serverId] {
        pWatcher->deleteLater();
        applySidebar(serverId, pWatcher->result());
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pPool = m_dependencies.pDbConnectionPool, serverId]() {
                const mixxx::DbConnectionPooler pooler(pPool);
                const QSqlDatabase database = mixxx::DbConnectionPooled(pPool);
                SidebarData data;
                for (int attempt = 0; attempt < 4 && !data.ok; ++attempt) {
                    if (attempt > 0) {
                        QThread::msleep(25);
                    }
                    QSqlQuery count(database);
                    count.prepare(QStringLiteral(
                            "SELECT COUNT(*) FROM crateserver_tracks WHERE server_id = :server"));
                    count.bindValue(QStringLiteral(":server"), serverId);
                    if (!count.exec() || !count.next()) {
                        continue;
                    }
                    data.libraryTracks = count.value(0).toLongLong();
                    count.finish();
                    data.containers = loadContainers(database, serverId);
                    data.ok = true;
                }
                return data;
            }));
}

void CrateServerFeature::applySidebar(const QString& serverId, const SidebarData& data) {
    TreeItem* pParent = serverItem(serverId);
    if (!data.ok || !pParent) {
        return;
    }
    const QPointer<CrateServerFeature> self(this);
    const auto updated = qScopeGuard([self, serverId] {
        if (self) {
            emit self->sidebarUpdated(serverId);
        }
    });
    for (auto it = m_containers.begin(); it != m_containers.end();) {
        it = it->serverId == serverId ? m_containers.erase(it) : std::next(it);
    }
    QHash<int, QList<StoredContainer>> containersOf;
    for (const StoredContainer& container : data.containers) {
        m_containers.insert(container.id, container);
        if (const auto section = sectionOf(container.kind)) {
            containersOf[static_cast<int>(*section)].append(container);
        }
    }

    // What the server's items should be
    struct Item {
        QString label;
        QString data;
        QIcon icon;
        QList<std::pair<QString, QString>> children; // label, data
    };
    QList<Item> items;
    items.append({withCount(tr("Library"), data.libraryTracks),
            libraryData(serverId),
            namedIcon(QStringLiteral("crate-library"), QStringLiteral("tracks")),
            {}});
    for (const Section section : kSections) {
        const QList<StoredContainer> containers = containersOf.value(static_cast<int>(section));
        const bool always = section == Section::Playlists || section == Section::Crates;
        if (containers.isEmpty() && !always) {
            // Not served by this server (yet): no empty placeholder
            continue;
        }
        // As the mockups: counts on Playlists, Crates and Likes only
        if (isSingleContainer(section, containers)) {
            items.append({section == Section::Likes
                            ? withCount(sectionName(section), countOf(containers.first()))
                            : sectionName(section),
                    containerData(serverId, containers.first().id),
                    sectionIcon(section),
                    {}});
            continue;
        }
        const bool counted = section == Section::Playlists || section == Section::Crates;
        Item item{counted ? withCount(sectionName(section), containers.size())
                          : sectionName(section),
                sectionData(serverId, section),
                sectionIcon(section),
                {}};
        for (const StoredContainer& container : containers) {
            item.children.append({withCount(container.name, countOf(container)),
                    containerData(serverId, container.id)});
        }
        items.append(item);
    }

    // Replace what changed: the selection and the expanded items stay
    QStringList wanted;
    for (const Item& item : std::as_const(items)) {
        wanted.append(item.data);
    }
    const QModelIndex parentIndex = indexOf(pParent);
    if (childData(pParent) != wanted) {
        if (pParent->hasChildren()) {
            m_pSidebarModel->removeRows(0, pParent->childRows(), parentIndex);
        }
        std::vector<std::unique_ptr<TreeItem>> children;
        for (const Item& item : std::as_const(items)) {
            auto pItem = std::make_unique<TreeItem>(item.label, item.data);
            pItem->setIcon(item.icon);
            for (const auto& [label, childData] : item.children) {
                pItem->appendChild(label, childData);
            }
            children.push_back(std::move(pItem));
        }
        m_pSidebarModel->insertTreeItemRows(std::move(children), 0, parentIndex);
        expandOwnItems();
        return;
    }
    for (int row = 0; row < items.size(); ++row) {
        TreeItem* pItem = pParent->child(row);
        const Item& item = items[row];
        if (pItem->getLabel() != item.label) {
            pItem->setLabel(item.label);
            m_pSidebarModel->triggerRepaint(indexOf(pItem));
        }
        QStringList wantedChildren;
        for (const auto& child : item.children) {
            wantedChildren.append(child.second);
        }
        if (childData(pItem) != wantedChildren) {
            const QModelIndex itemIndex = indexOf(pItem);
            if (pItem->hasChildren()) {
                m_pSidebarModel->removeRows(0, pItem->childRows(), itemIndex);
            }
            std::vector<std::unique_ptr<TreeItem>> children;
            for (const auto& [label, childData] : item.children) {
                children.push_back(std::make_unique<TreeItem>(label, childData));
            }
            m_pSidebarModel->insertTreeItemRows(std::move(children), 0, itemIndex);
            continue;
        }
        for (int childRow = 0; childRow < item.children.size(); ++childRow) {
            TreeItem* pChild = pItem->child(childRow);
            if (pChild->getLabel() != item.children[childRow].first) {
                pChild->setLabel(item.children[childRow].first);
                m_pSidebarModel->triggerRepaint(indexOf(pChild));
            }
        }
    }
}

QModelIndex CrateServerFeature::indexOf(const TreeItem* pItem) const {
    if (!pItem || !pItem->hasParent()) {
        return QModelIndex();
    }
    return m_pSidebarModel->index(pItem->parentRow(), 0, indexOf(pItem->parent()));
}

CrateServerFeature::Node CrateServerFeature::nodeOf(const QModelIndex& index) const {
    Node node;
    // The sidebar's own indexes and the feature model's both point at the
    // TreeItem
    const auto* pItem = static_cast<const TreeItem*>(index.internalPointer());
    if (!index.isValid() || !pItem) {
        return node;
    }
    const QStringList parts = pItem->getData().toString().split(QLatin1Char('\n'));
    if (parts.size() < 2) {
        return node;
    }
    node.serverId = parts[1];
    if (parts[0] == QLatin1String("server")) {
        node.kind = Node::Kind::Server;
    } else if (parts[0] == QLatin1String("library")) {
        node.kind = Node::Kind::Library;
    } else if (parts[0] == QLatin1String("section") && parts.size() == 3) {
        node.kind = Node::Kind::Section;
        node.section = parts[2].toInt();
    } else if (parts[0] == QLatin1String("container") && parts.size() == 3) {
        node.kind = Node::Kind::Container;
        node.containerId = parts[2].toLongLong();
    }
    return node;
}

void CrateServerFeature::whenReady(const QString& serverId, std::function<void()> ready) {
    Server* pServer = server(serverId);
    if (!pServer) {
        return;
    }
    switch (pServer->readiness) {
    case Server::Readiness::Ready:
        if (ready) {
            ready();
        }
        return;
    case Server::Readiness::Unsupported:
        return;
    case Server::Readiness::Checking:
        if (ready) {
            pServer->waiting.append(std::move(ready));
        }
        return;
    case Server::Readiness::Unknown:
        break;
    }
    pServer->readiness = Server::Readiness::Checking;
    if (ready) {
        pServer->waiting.append(std::move(ready));
    }
    Client* pClient = m_pServers ? m_pServers->client(serverId) : nullptr;
    if (!pClient) {
        onCapabilities(serverId, false, QByteArray());
        return;
    }
    RequestOptions anonymous;
    anonymous.authenticated = false;
    Reply* pReply = pClient->get(QStringLiteral("/api/capabilities"), anonymous);
    connect(pReply, &Reply::finished, this, [this, serverId](const JsonResult& result) {
        onCapabilities(serverId,
                result.ok(),
                result.ok() ? result.value().toJson(QJsonDocument::Compact) : QByteArray());
    });
}

void CrateServerFeature::onCapabilities(
        const QString& serverId, bool ok, const QByteArray& json) {
    Server* pServer = server(serverId);
    if (!pServer || !m_pServers) {
        return;
    }
    QByteArray capabilitiesJson = json;
    if (ok) {
        m_pServers->rememberCapabilities(serverId, json);
    } else {
        // Offline: what the server said last time
        capabilitiesJson = m_pServers->lastCapabilities(serverId);
    }
    const Result<Capabilities> capabilities =
            Capabilities::fromJson(QJsonDocument::fromJson(capabilitiesJson));
    if (!capabilities.ok() || !capabilities.value().has(DecksFeature::LibraryCursor)) {
        kLogger.info() << "The Crate server" << serverId
                       << "does not serve the library to Decks (library_cursor)";
        pServer->readiness = Server::Readiness::Unsupported;
        pServer->waiting.clear();
        return;
    }
    pServer->readiness = Server::Readiness::Ready;
    if (!pServer->pEvents && m_pServers->client(serverId)) {
        pServer->pEvents = std::make_unique<EventSubscriber>(
                m_pServers->client(serverId), pServer->pSync.get());
        pServer->pEvents->start();
    }
    // Once per run: the tree, then whatever was waiting
    pServer->pSync->syncContainers();
    const QList<std::function<void()>> waiting = std::exchange(pServer->waiting, {});
    for (const auto& ready : waiting) {
        ready();
    }
}

void CrateServerFeature::activate() {
    servers();
    if (m_serverIds.isEmpty()) {
        // No Crate server: Mixxx's own tracks
        if (m_pLibrary && m_pLibrary->trackTableModel()) {
            emit showTrackModel(m_pLibrary->trackTableModel());
        }
        return;
    }
    const QString& serverId = m_serverIds.first();
    TreeItem* pParent = serverItem(serverId);
    if (pParent && pParent->hasChildren()) {
        // Not scrolled to: the section header above it stays in view
        emit featureSelect(this, indexOf(pParent->child(0)), false);
    }
    showLibrary(serverId);
}

void CrateServerFeature::activateChild(const QModelIndex& index) {
    const Node node = nodeOf(index);
    switch (node.kind) {
    case Node::Kind::Library:
        showLibrary(node.serverId);
        return;
    case Node::Kind::Container:
        showContainer(node.serverId, node.containerId);
        return;
    case Node::Kind::Server:
    case Node::Kind::Section:
        whenReady(node.serverId, nullptr);
        return;
    case Node::Kind::None:
        return;
    }
}

void CrateServerFeature::onLazyChildExpandation(const QModelIndex& index) {
    const Node node = nodeOf(index);
    if (!node.serverId.isEmpty()) {
        whenReady(node.serverId, nullptr);
    }
}

void CrateServerFeature::showLibrary(const QString& serverId) {
    m_pLibraryModel->setBpmPrecision(m_pConfig->getValue(
            mixxx::library::prefs::kBpmColumnPrecisionConfigKey,
            BaseTrackTableModel::kBpmColumnPrecisionDefault));
    m_pLibraryModel->showLibrary(serverId);
    emit showTrackModel(m_pLibraryModel);
    emit enableCoverArtDisplay(false);
    whenReady(serverId, nullptr);
}

void CrateServerFeature::showContainer(const QString& serverId, qint64 containerId) {
    m_pContainerModel->setBpmPrecision(m_pConfig->getValue(
            mixxx::library::prefs::kBpmColumnPrecisionConfigKey,
            BaseTrackTableModel::kBpmColumnPrecisionDefault));
    // The stored tracks at once; the server's if they changed
    m_pContainerModel->showContainer(containerId);
    emit showTrackModel(m_pContainerModel);
    emit enableCoverArtDisplay(false);
    whenReady(serverId, [this, serverId, containerId] {
        syncIfNeeded(serverId, containerId);
    });
}

void CrateServerFeature::syncIfNeeded(const QString& serverId, qint64 containerId) {
    Server* pServer = server(serverId);
    if (!pServer || pServer->readiness != Server::Readiness::Ready) {
        return;
    }
    // The tree read last; a container the tree has not shown yet is synced.
    // A clean one is still asked once per run: its tree ETag only follows
    // membership and order, and a mood's never changes, while its tracks'
    // metadata, profiles and availability change only its pages' ETags (S3
    // contract). Each page goes with If-None-Match, so an unchanged
    // container costs a 304 per page and no parsing.
    const auto it = m_containers.constFind(containerId);
    const bool firstThisRun = !pServer->revalidated.contains(containerId);
    if (it == m_containers.constEnd() || it->needsSync() || firstThisRun) {
        pServer->revalidated.insert(containerId);
        pServer->pSync->syncContainerTracks(containerId);
    }
}

void CrateServerFeature::onRightClickChild(const QPoint& globalPos, const QModelIndex& index) {
    // TEST_GAP: a context menu needs a running event loop with a menu open
    const Node node = nodeOf(index);
    Server* pServer = server(node.serverId);
    if (!pServer) {
        return;
    }
    QMenu menu;
    QAction* pRefresh = menu.addAction(tr("Refresh"));
    if (menu.exec(globalPos) != pRefresh) {
        return;
    }
    if (node.kind == Node::Kind::Container) {
        const auto it = m_containers.constFind(node.containerId);
        if (it != m_containers.constEnd()) {
            pServer->pSync->markDirty(
                    {Invalidation::Scope::Container, it->kind, it->remoteId});
        }
        whenReady(node.serverId, [pSync = pServer->pSync.get(), containerId = node.containerId] {
            pSync->syncContainerTracks(containerId);
        });
        return;
    }
    whenReady(node.serverId, [pSync = pServer->pSync.get()] {
        pSync->syncContainers();
    });
}

void CrateServerFeature::bindSidebarWidget(WLibrarySidebar* pSidebarWidget) {
    m_pSidebarWidget = pSidebarWidget;
    servers();
    if (auto* pModel = qobject_cast<SidebarModel*>(pSidebarWidget->model())) {
        // A reset of the sidebar folds every item
        connect(pModel, &QAbstractItemModel::modelReset, this, [this] {
            expandOwnItems();
        });
    }
    expandOwnItems();
}

void CrateServerFeature::expandOwnItems() {
    // The section is always open, as in the mockups: its root is a header
    auto* pModel = m_pSidebarWidget ? qobject_cast<SidebarModel*>(m_pSidebarWidget->model())
                                    : nullptr;
    if (!pModel) {
        return;
    }
    const QModelIndex root = pModel->getFeatureRootIndex(this);
    if (!root.isValid()) {
        return;
    }
    m_pSidebarWidget->expand(root);
    if (m_serverIds.size() > 1) {
        for (int row = 0; row < pModel->rowCount(root); ++row) {
            m_pSidebarWidget->expand(pModel->index(row, 0, root));
        }
    }
}

} // namespace crate::library

#include "moc_crateserverfeature.cpp"
