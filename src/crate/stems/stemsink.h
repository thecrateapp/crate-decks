#pragma once

#include <QString>
#include <span>

#include "util/types.h"

namespace crate {

/// The audio a separation starts from.
struct StemSeparationSource {
    /// The original as Decks decodes it with SoundSourceProxy.
    int originalSampleRate = 0;
    int originalChannelCount = 0;
    SINT originalFrames = 0;
    /// Frames of the stems.
    SINT frames = 0;
    /// Of the stems: 44.1 kHz from the separator; the server's stem files
    /// have theirs.
    int stemSampleRate = 44100;
};

/// Receives the stems of a separation on the separator's worker thread.
class StemSink {
  public:
    virtual ~StemSink() = default;

    /// Before the first stems, once the original is decoded.
    virtual bool begin(const StemSeparationSource& source, QString* pError) {
        Q_UNUSED(source);
        Q_UNUSED(pError);
        return true;
    }

    /// The stems at 44.1 kHz of the frames [firstFrame, firstFrame +
    /// frameCount), in order and without gaps: 8 rows of `frameCount`
    /// samples, drums left and right, then bass, other and vocals. Returning
    /// false fails the separation with `pError`.
    virtual bool write(SINT firstFrame,
            SINT frameCount,
            std::span<const float> stems,
            QString* pError) = 0;
};

} // namespace crate
