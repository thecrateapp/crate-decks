#include "crate/library/cratetrackpreparer.h"

#include "sources/soundsourceproxy.h"
#include "track/globaltrackcache.h"
#include "track/track.h"
#include "util/fileaccess.h"
#include "util/logger.h"

namespace crate::library {

namespace {

const mixxx::Logger kLogger("crate::library::prepareTrackForLibrary");

} // namespace

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

} // namespace crate::library
