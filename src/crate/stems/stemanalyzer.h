#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <array>
#include <optional>
#include <span>

#include "crate/stems/stemsink.h"
#include "util/types.h"

namespace crate {

/// A `stem-analysis/1` document (Smart Mix Stems design §5, STEM-D1): per
/// stem, one level byte per 250 ms window, optional beats tracked on the
/// drum stem, the separator, the producer and the server's source revision
/// of the file it describes. Never audio. The server reads and stores it
/// with the same rules (crate.smart_mix.stems.StemAnalysis).
struct StemAnalysis {
    static const QString kSchema;
    /// In the order of the sidecar (STEM-D9): drums, bass, other, vocals.
    static const std::array<QString, 4> kSources;
    static constexpr int kHopMs = 250;
    /// The separator identifier of both producers (STEM-D3).
    static const QString kSeparatorVersion;

    /// "crate-decks/<version>" or "crate-server/<version>".
    QString producer;
    /// "htdemucs" or "htdemucs_ft".
    QString separatorModel = QStringLiteral("htdemucs");
    QString separatorVersion = kSeparatorVersion;
    /// The server's sourceRevision of the track's file: 64 hex digits.
    QString sourceRevision;
    int hopMs = kHopMs;
    /// Per stem, in kSources order: one byte per window, RMS in dBFS
    /// linear from -60 dB (0) to 0 dB (255).
    std::array<QByteArray, 4> levels;
    /// Strictly increasing; empty when there are none.
    QList<qint64> drumBeatsMs;
    qint64 durationMs = 0;

    bool isFromDecks() const {
        return producer.startsWith(QLatin1String("crate-decks/"));
    }
    /// STEM-D4: the stronger separator wins (htdemucs_ft over htdemucs).
    int separatorStrength() const;

    /// The document as JSON, for `PUT /api/decks/tracks/{uid}/stem-analysis`.
    QByteArray toJson() const;
    /// Reads a document with the checks the server makes on an upload
    /// (schema, sizes and ranges), so that what Decks keeps from the server
    /// or sends to it is well-formed. nullopt, with the reason in `pError`,
    /// otherwise.
    static std::optional<StemAnalysis> fromJson(const QByteArray& json, QString* pError);
};

/// `crate-decks/<the application's version>`, in the producer's pattern.
QString decksStemProducer();

/// One level byte: -60 dBFS or less (silence included) is 0 and 0 dBFS or
/// more is 255, linear in dB and rounded half up, floor((dB + 60) * 255 / 60
/// + 0.5), as crate.smart_mix.stems.encode_levels() on the server.
quint8 encodeStemLevel(double rmsDb);

/// Computes the `stem-analysis/1` levels of the stems a separation writes
/// (a StemSink), as the server's producer does
/// (crate.smart_mix.separation.analyze_stems): each stem's level is the RMS
/// of its mono mix, the mean of both channels, over windows of 250 ms from
/// the start of the track; the last window may be short and covers the
/// samples it has. The stems are at 44.1 kHz. Not thread-safe: the
/// separator's worker thread writes it, and finish() is called after.
class StemAnalyzer : public StemSink {
  public:
    explicit StemAnalyzer(int sampleRate = 44100);

    bool begin(const StemSeparationSource& source, QString* pError) override;
    bool write(SINT firstFrame,
            SINT frameCount,
            std::span<const float> stems,
            QString* pError) override;

    /// The analysis of what was written.
    StemAnalysis finish(const QString& sourceRevision,
            const QString& producer = decksStemProducer()) const;

  private:
    void reset();

    const int m_sampleRate;
    const SINT m_window;
    SINT m_frames = 0;
    /// Per stem: the window levels done, and the sum of squares of the
    /// window in progress.
    std::array<QByteArray, 4> m_levels;
    std::array<double, 4> m_squares{};
    SINT m_inWindow = 0;
};

} // namespace crate
