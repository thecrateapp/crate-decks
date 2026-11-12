#pragma once

#include <QByteArray>
#include <QString>
#include <array>
#include <atomic>

#include "track/track_decl.h"

class QColor;

namespace crate {

/// The NI STEM manifest of a file Decks exports: four stems, named, in
/// `colors` (drums, bass, other, vocals), with the compressor and the
/// limiter off, so that other software plays the stems as they are.
QByteArray niStemManifest(const std::array<QColor, 4>& colors);

/// Exports a track and its stem sidecar (STEM-D9) as an NI STEM file, for
/// other software (plan ST5.PR2). It is never the playback format: Decks
/// plays the sidecar.
///
/// With libav, an MP4 of five AAC streams at 256 kbps and the original's
/// rate: the original as the master (the only default stream), then drums,
/// bass, other and vocals. `moov` is written at the end, and the `stem`
/// box with niStemManifest() of stemColors() goes in as the first child of
/// `moov/udta`. The file goes to `<outPath>.part` and is renamed into
/// place.
///
/// Blocks for the whole track: call it on a worker thread. `pCancel`
/// (optional) stops it, leaving nothing.
bool exportNiStem(const TrackPointer& pTrack,
        const QString& sidecarPath,
        const QString& outPath,
        const std::atomic<bool>* pCancel,
        QString* pError);

} // namespace crate
