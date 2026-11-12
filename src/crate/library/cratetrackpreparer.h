#pragma once

#include <QString>

#include "track/track_decl.h"

class TrackCollectionManager;

namespace crate::library {

/// The library's track of a file, from the track cache or the database,
/// never adding it; nullptr if the library has none. Also for a file that is
/// not there (yet), which TrackCollectionManager::getTrackByRef() does not
/// find: without its file a track has no canonical location. The database
/// then reads nothing from the file. Main thread.
TrackPointer libraryTrackOfFile(TrackCollectionManager* pCollection, const QString& path);

/// Reads the tags and the cover of a file that the library does not have
/// yet, as TrackDAO does when it adds one, into the track object of the
/// global track cache, so that the main thread does not have to.
///
/// Adding a file to the library reads its tags and decodes, scales and
/// hashes its embedded cover: 40-90 ms for the files of the dev stack (G3,
/// 2026-11-11), over the 16 ms the main thread may block (design §9). Run
/// this on a worker first; TrackCollectionManager::getOrAddTrack() on the
/// main thread then finds the prepared track in the cache and only writes
/// its library row.
///
/// Only for a file the library does not have: a track of the library must
/// come from the database. The caller keeps the returned track until
/// getOrAddTrack() has taken it, because the cache cannot save a track that
/// is in no library. Thread-safe. Returns nullptr for a file Mixxx cannot
/// read or that another track holds.
TrackPointer prepareTrackForLibrary(const QString& path, const SyncTrackMetadataParams& params);

/// Reads the tags and the cover of a library track that has never been read
/// from its file, as TrackDAO::getTrackById() does the first time it takes
/// the track from the database with the file there (60-74 ms for the files
/// of the dev stack, G3, 2026-11-12). Such is the track of a Crate file that
/// was queued in Auto DJ before it was downloaded. Run this on a worker with
/// the track held in the track cache from before its file came: the
/// database then gives that track, read, without touching the file on the
/// main thread. Thread-safe; nothing for a track read already.
void readLibraryTrackFromFile(const TrackPointer& pTrack, const SyncTrackMetadataParams& params);

} // namespace crate::library
