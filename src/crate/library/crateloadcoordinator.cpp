#include "crate/library/crateloadcoordinator.h"

#include "control/controlobject.h"
#include "mixer/playermanager.h"
#include "preferences/dialog/dlgprefdeck.h"
#include "track/track.h"
#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::CrateLoadCoordinator");

bool isPlaying(const QString& group) {
    return ControlObject::get(ConfigKey(group, QStringLiteral("play"))) > 0.0;
}

} // namespace

CrateLoadCoordinator::CrateLoadCoordinator(Options options, QObject* parent)
        : QObject(parent),
          m_pCache(options.pCache),
          m_pConfig(std::move(options.pConfig)),
          m_trackOfFile(std::move(options.trackOfFile)),
          m_applyAnalysis(std::move(options.applyAnalysis)) {
}

CrateLoadCoordinator::~CrateLoadCoordinator() {
    const QStringList groups = m_pending.keys();
    for (const QString& group : groups) {
        cancel(group);
    }
}

QString CrateLoadCoordinator::keyOf(const QString& serverId, const QString& entityUid) {
    return serverId + QLatin1Char('\n') + entityUid.toLower();
}

bool CrateLoadCoordinator::isCached(const QString& serverId, const QString& entityUid) const {
    return m_pCache && m_pCache->cachedPath(serverId, entityUid).has_value();
}

TrackPointer CrateLoadCoordinator::cachedTrack(const QString& serverId, const QString& entityUid) {
    if (!m_pCache) {
        return TrackPointer();
    }
    const std::optional<QString> path = m_pCache->cachedPath(serverId, entityUid);
    if (!path) {
        return TrackPointer();
    }
    return trackOf(*path, serverId, entityUid);
}

TrackPointer CrateLoadCoordinator::trackOf(
        const QString& path, const QString& serverId, const QString& entityUid) {
    if (!m_trackOfFile) {
        return TrackPointer();
    }
    TrackPointer pTrack = m_trackOfFile(path);
    if (!pTrack) {
        kLogger.warning() << "The library does not take the cached file" << path;
        return pTrack;
    }
    const QString key = keyOf(serverId, entityUid);
    if (m_applyAnalysis && !m_analysed.contains(key)) {
        m_analysed.insert(key);
        m_applyAnalysis(pTrack, serverId, entityUid);
    }
    return pTrack;
}

void CrateLoadCoordinator::requestLoad(
        const CrateTrackRef& track, const QString& group, bool play) {
    cancel(group);
    if (!m_pCache) {
        return;
    }
    PendingLoad load;
    load.track = track;
    load.play = play;
    cache::CacheTicket* pTicket = m_pCache->ensure(
            track.serverId, track.entityUid, cache::Priority::Deck, track.hints);
    load.pTicket = pTicket;
    m_pending.insert(group, load);
    connect(pTicket, &cache::CacheTicket::ready, this, [this, group, pTicket](const QString& path) {
        onReady(group, pTicket, path);
    });
    connect(pTicket,
            &cache::CacheTicket::failed,
            this,
            [this, group, pTicket](const crate::Error& error) {
                const auto it = m_pending.constFind(group);
                if (it == m_pending.cend() || it->pTicket != pTicket) {
                    return;
                }
                const CrateTrackRef failed = it->track;
                m_pending.remove(group);
                kLogger.warning() << "Cannot load" << failed.entityUid << "into" << group << ":"
                                  << error.message;
                emit loadFailed(failed.serverId, failed.entityUid, error);
            });
}

void CrateLoadCoordinator::onReady(
        const QString& group, cache::CacheTicket* pTicket, const QString& path) {
    const auto it = m_pending.constFind(group);
    if (it == m_pending.cend() || it->pTicket != pTicket) {
        return;
    }
    const PendingLoad load = it.value();
    m_pending.remove(group);
    const TrackPointer pTrack = trackOf(path, load.track.serverId, load.track.entityUid);
    if (!pTrack) {
        return;
    }
    if (group.isEmpty()) {
        // As a double click: the first stopped deck, if there is one
        emit loadTrackIntoNextAvailableDeck(pTrack);
        return;
    }
    if (!mayLoadInto(group, m_pConfig)) {
        // The deck started playing while the track downloaded
        kLogger.info() << "Not loading" << load.track.entityUid << "into the playing" << group;
        return;
    }
    emit loadTrackToPlayer(pTrack, group, load.play);
}

void CrateLoadCoordinator::cancel(const QString& group) {
    const auto it = m_pending.find(group);
    if (it == m_pending.end()) {
        return;
    }
    const QPointer<cache::CacheTicket> pTicket = it->pTicket;
    m_pending.erase(it);
    if (pTicket) {
        pTicket->cancel();
    }
}

bool CrateLoadCoordinator::isLoadPending(const QString& group) const {
    return m_pending.contains(group);
}

void CrateLoadCoordinator::download(const QList<CrateTrackRef>& tracks,
        cache::Priority priority,
        const QString& pinReason) {
    if (!m_pCache) {
        return;
    }
    for (const CrateTrackRef& track : tracks) {
        if (!pinReason.isEmpty()) {
            m_pCache->pin(track.serverId, track.entityUid, pinReason);
        }
        // The ticket deletes itself when the download ends
        cache::CacheTicket* pTicket =
                m_pCache->ensure(track.serverId, track.entityUid, priority, track.hints);
        connect(pTicket,
                &cache::CacheTicket::failed,
                this,
                [track](const crate::Error& error) {
                    kLogger.warning() << "Cannot download" << track.entityUid << ":"
                                      << error.message;
                });
    }
}

bool CrateLoadCoordinator::removeFromCache(const QString& serverId, const QString& entityUid) {
    if (!m_pCache) {
        return false;
    }
    m_analysed.remove(keyOf(serverId, entityUid));
    return m_pCache->remove(serverId, entityUid);
}

bool CrateLoadCoordinator::mayLoadInto(const QString& group, const UserSettingsPointer& pConfig) {
    if (group.isEmpty() || PlayerManager::isPreviewDeckGroup(group) || !isPlaying(group)) {
        return true;
    }
    if (!PlayerManager::isDeckGroup(group)) {
        // A playing sampler: the track menu offers no load into it either
        return false;
    }
    // The rule of WTrackTableView::loadSelectedTrackToGroup()
    if (!pConfig) {
        return false;
    }
    if (pConfig->exists(kConfigKeyLoadWhenDeckPlaying)) {
        switch (static_cast<LoadWhenDeckPlaying>(
                pConfig->getValueString(kConfigKeyLoadWhenDeckPlaying).toInt())) {
        case LoadWhenDeckPlaying::Allow:
        case LoadWhenDeckPlaying::AllowButStopDeck:
            return true;
        case LoadWhenDeckPlaying::Reject:
            return false;
        }
        return false;
    }
    // The older form of the preference
    return pConfig->getValue<bool>(kConfigKeyAllowTrackLoadToPlayingDeck);
}

} // namespace crate::library

#include "moc_crateloadcoordinator.cpp"
