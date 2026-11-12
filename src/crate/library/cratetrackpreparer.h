#pragma once

#include <QString>

#include "track/track_decl.h"

namespace crate::library {

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

} // namespace crate::library
