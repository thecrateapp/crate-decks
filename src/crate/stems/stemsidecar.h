#pragma once

#include <QByteArray>
#include <QString>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "crate/stems/stemanalyzer.h"
#include "crate/stems/stemsink.h"
#include "track/track_decl.h"
#include "util/types.h"

namespace crate {

class Resampler;

/// The lossless stem sidecar of a track (STEM-D9): an 8-channel FLAC beside
/// its file with drums, bass, other and vocals, left and right each, at the
/// original's sample rate. `other` is the integer residual against Decks'
/// own decode of the original, so the four stems add up to it: a deck that
/// plays the sidecar with no stem touched sounds like the original.
///
/// Samples are 24-bit at half scale (kScale), which leaves 6 dB of headroom
/// for the residual: they read back as integer / 2^23 / kScale. A 16-bit
/// original comes back exactly; a decode with more resolution within 2^-24
/// (-144 dBFS).
///
/// It records, as FLAC tags, the sample rate and frames of the decode it was
/// made against and a PCM fingerprint of that decode: kFingerprintExcerpts
/// short excerpts spread over the track, 16-bit. A sidecar applies to a file
/// only while the decode of that file matches all three (isValidFor()), so a
/// changed file, another decoder or a different decoder delay never plays
/// stems out of step with the original. The excerpts are compared with a
/// tolerance of a few 16-bit steps, which the rounding of a decoder never
/// reaches and an offset of one sample in music does.
class StemSidecar {
  public:
    static constexpr int kChannels = 8;
    static constexpr int kBitsPerSample = 24;
    static constexpr double kScale = 0.5;
    static constexpr int kFingerprintExcerpts = 8;
    static constexpr SINT kFingerprintFrames = 512;
    /// In 16-bit steps.
    static constexpr int kFingerprintTolerance = 2;

    struct Info {
        int sampleRate = 0;
        SINT frames = 0;
        /// StemAnalysis::kSeparatorVersion, or the server's separator for
        /// stems that came from it.
        QString separator;
        QString sourceRevision;
        /// Interleaved 16-bit stereo excerpts, little endian.
        QByteArray fingerprint;
        qint64 bytes = 0;
    };

    /// `<folder>/<name without extension>.stems.flac` beside `originalPath`.
    static QString pathFor(const QString& originalPath);

    /// The sidecar's format and tags; nullopt, with the reason in `pError`,
    /// for a file that is not a sidecar.
    static std::optional<Info> readInfo(const QString& sidecarPath, QString* pError = nullptr);

    /// Whether the sidecar beside the track's file (pathFor()) matches the
    /// file's decode: sample rate, frames and fingerprint. Decodes a few
    /// excerpts of the file: call it off the main thread.
    static bool isValidFor(const TrackPointer& pTrack, QString* pReason = nullptr);
    static bool isValidFor(
            const TrackPointer& pTrack, const QString& sidecarPath, QString* pReason = nullptr);

    /// Decodes a whole sidecar into 8 interleaved channels at full scale.
    /// For tests and tools: a track is 1.4 MB per second of audio.
    static bool decode(const QString& sidecarPath,
            std::vector<float>* pSamples,
            SINT* pFrames,
            QString* pError = nullptr);
};

/// Writes a track's sidecar from its stems (a StemSink), on the thread that
/// writes them: the separator's worker, or the one that decodes the
/// server's stems.
///
/// It decodes the original again with SoundSourceProxy, as the separator
/// does (stereo: the first two channels, or mono twice), resamples drums,
/// bass and vocals back to its rate (crate::Resampler), quantises them and
/// writes `other` as the residual. Frames the stems do not cover (the
/// resampler's last fraction of a frame) have only `other`. The file goes
/// to `<sidecar>.part` and is renamed into place by finish(); a writer
/// destroyed before that leaves nothing.
class StemSidecarWriter : public StemSink {
  public:
    struct Options {
        TrackPointer pTrack;
        /// Defaults to StemSidecar::pathFor() of the track's file.
        QString sidecarPath;
        QString separator = StemAnalysis::kSeparatorVersion;
        QString sourceRevision;
    };

    explicit StemSidecarWriter(Options options);
    ~StemSidecarWriter() override;

    bool begin(const StemSeparationSource& source, QString* pError) override;
    /// `other` (rows 4 and 5) is not read: it is the residual.
    bool write(SINT firstFrame,
            SINT frameCount,
            std::span<const float> stems,
            QString* pError) override;
    /// Writes what is left and moves the sidecar into place.
    bool finish(QString* pError);

    const QString& sidecarPath() const {
        return m_options.sidecarPath;
    }
    /// Residual samples that did not fit in 24 bits and were clipped: the
    /// stems then do not add up to the original exactly. Expected 0.
    qint64 clippedSamples() const {
        return m_clipped;
    }

  private:
    class Encoder;
    class Original;

    /// Encodes the stems resampled so far, against as many frames of the
    /// original; with `last`, every frame of the original left.
    bool encodePending(bool last, QString* pError);
    void abandon();

    Options m_options;
    std::unique_ptr<Original> m_pOriginal;
    std::unique_ptr<Resampler> m_pResampler;
    std::unique_ptr<Encoder> m_pEncoder;
    /// Drums, bass and vocals at the original's rate, 6 interleaved channels.
    std::vector<float> m_pending;
    std::vector<float> m_resampleInput;
    std::vector<qint32> m_block;
    SINT m_nextStemFrame = 0;
    SINT m_encodedFrames = 0;
    qint64 m_clipped = 0;
    bool m_finished = false;
};

} // namespace crate
