#include "crate/library/cratetrackpreparer.h"

#include <QSqlQuery>

#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "sources/soundsourceproxy.h"
#include "track/globaltrackcache.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/fileaccess.h"
#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::prepareTrackForLibrary");

} // namespace

TrackPointer libraryTrackOfFile(TrackCollectionManager* pCollection, const QString& path) {
    if (!pCollection) {
        return TrackPointer();
    }
    const TrackRef trackRef = TrackRef::fromFilePath(path);
    if (trackRef.isValid()) {
        return pCollection->getTrackByRef(trackRef);
    }
    // By its location, as TrackDAO::getTrackIdByLocation()
    QSqlQuery query(pCollection->internalCollection()->database());
    query.prepare(QStringLiteral(
            "SELECT library.id FROM library "
            "INNER JOIN track_locations ON library.location = track_locations.id "
            "WHERE track_locations.location = :location"));
    query.bindValue(QStringLiteral(":location"), mixxx::FileInfo(path).location());
    if (!query.exec() || !query.next()) {
        return TrackPointer();
    }
    return pCollection->getTrackById(TrackId(query.value(0)));
}

TrackPointer prepareTrackForLibrary(const QString& path, const SyncTrackMetadataParams& params) {
    const auto fileAccess = mixxx::FileAccess(mixxx::FileInfo(path));
    if (!SoundSourceProxy::isFileSupported(fileAccess.info())) {
        return TrackPointer();
    }
    TrackPointer pTrack;
    {
        // Locks the cache while it lives, as TrackDAO::addTracksAddFile()
        GlobalTrackCacheResolver resolver(fileAccess);
        pTrack = resolver.getTrack();
        switch (resolver.getLookupResult()) {
        case GlobalTrackCacheLookupResult::Hit:
            if (!pTrack || pTrack->getId().isValid()) {
                // In the library already: nothing to read
                return pTrack;
            }
            break;
        case GlobalTrackCacheLookupResult::Miss:
            break;
        default:
            kLogger.warning() << "Another track holds" << path;
            return TrackPointer();
        }
    }
    if (pTrack->getSourceSynchronizedAt().isValid()) {
        // Read already
        return pTrack;
    }
    // What TrackDAO::addTracksAddFile() reads before it adds the row; there
    // it then only takes the tags it does not have
    SoundSourceProxy(pTrack).updateTrackFromSource(
            SoundSourceProxy::UpdateTrackFromSourceMode::Once, params);
    return pTrack;
}

void readLibraryTrackFromFile(const TrackPointer& pTrack, const SyncTrackMetadataParams& params) {
    if (!pTrack || pTrack->getSourceSynchronizedAt().isValid()) {
        return;
    }
    // The call of TrackDAO::getTrackById()
    SoundSourceProxy(pTrack).updateTrackFromSource(
            SoundSourceProxy::UpdateTrackFromSourceMode::Once, params);
}

} // namespace crate::library
