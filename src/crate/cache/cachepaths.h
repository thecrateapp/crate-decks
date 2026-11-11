#pragma once

#include <QByteArray>
#include <QString>

/// Where the audio cache keeps each track (design D4): a stable path that
/// depends only on the server and the track, so that a file is found again
/// after a restart and a resumed download writes to the same place.
namespace crate::cache {

/// Whether `id` can be part of a cache path: a server id or an entity uid
/// made of letters, digits, '-' and '_', as UUIDs are. Anything else (a dot,
/// a slash) could leave the cache directory and is refused.
bool isSafePathId(const QString& id);

/// The file extension for a track, without the dot: from the server's
/// `format` ("flac", "mp3", "m4a", "alac"…) if it names a known one, else
/// from the stream's Content-Type ("audio/flac"…). Empty if neither does:
/// the decoders pick the format by extension, so an unknown one cannot be
/// played.
QString extensionFor(const QString& format, const QByteArray& contentType);

/// `<server_id>/<uid[0:2]>/<uid>.<extension>`, relative to the cache
/// directory. Both ids must pass isSafePathId().
QString relativeAudioPath(
        const QString& serverId, const QString& entityUid, const QString& extension);

/// The file a download writes to before it is complete: the final path plus
/// ".part".
QString partPathOf(const QString& path);

/// Whether `fileName` is a part file of the cache layout.
bool isPartFile(const QString& fileName);

} // namespace crate::cache
