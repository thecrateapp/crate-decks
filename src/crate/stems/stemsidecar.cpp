#include "crate/stems/stemsidecar.h"

#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio/types.h"
#include "crate/stems/resampler.h"
#include "sources/audiosourcestereoproxy.h"
#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "util/logger.h"
#include "util/samplebuffer.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::StemSidecar");

/// Frames encoded at once.
constexpr SINT kBlockFrames = 4096;
/// The stems that are not the residual, as rows of a StemSink write: drums,
/// bass and vocals, left and right.
constexpr std::array<int, 6> kStemRows = {0, 1, 2, 3, 6, 7};
/// Padding for the tags, written once the fingerprint is known.
constexpr unsigned kPaddingBytes = 64 * 1024;
constexpr qint32 kMax24 = (1 << 23) - 1;
constexpr qint32 kMin24 = -(1 << 23);
constexpr double kFullScale = 8388608.0 * StemSidecar::kScale;

const char* const kTagSources = "CRATE_STEMS";
const char* const kTagScale = "CRATE_SCALE";
const char* const kTagFrames = "CRATE_FRAMES";
const char* const kTagSampleRate = "CRATE_SAMPLE_RATE";
const char* const kTagSeparator = "CRATE_SEPARATOR";
const char* const kTagSourceRevision = "CRATE_SOURCE_REVISION";
const char* const kTagFingerprint = "CRATE_FINGERPRINT";

bool fail(QString* pError, const QString& message) {
    if (pError) {
        *pError = message;
    }
    return false;
}

qint32 quantise24(float sample) {
    return static_cast<qint32>(std::clamp<long>(
            std::lround(static_cast<double>(sample) * kFullScale), kMin24, kMax24));
}

qint16 quantise16(float sample) {
    return static_cast<qint16>(
            std::clamp<long>(std::lround(static_cast<double>(sample) * 32767.0), -32768, 32767));
}

/// The excerpts of the fingerprint: their first frames, all of the same
/// length (excerptLength()).
std::vector<SINT> excerptStarts(SINT frames) {
    if (frames <= StemSidecar::kFingerprintFrames) {
        return {0};
    }
    std::vector<SINT> starts;
    const SINT span = frames - StemSidecar::kFingerprintFrames;
    for (SINT k = 0; k < StemSidecar::kFingerprintExcerpts; ++k) {
        starts.push_back((k + 1) * span / (StemSidecar::kFingerprintExcerpts + 1));
    }
    return starts;
}

SINT excerptLength(SINT frames) {
    return std::min(frames, StemSidecar::kFingerprintFrames);
}

/// The original, decoded as the separator and the decks decode it: stereo.
mixxx::AudioSourcePointer openOriginal(const TrackPointer& pTrack, QString* pError) {
    if (!pTrack) {
        fail(pError, QStringLiteral("No track"));
        return {};
    }
    mixxx::AudioSource::OpenParams openParams;
    openParams.setChannelCount(mixxx::audio::ChannelCount::stereo());
    mixxx::AudioSourcePointer pSource = SoundSourceProxy(pTrack).openAudioSource(openParams);
    if (!pSource) {
        fail(pError, QStringLiteral("Cannot decode %1").arg(pTrack->getLocation()));
        return {};
    }
    if (pSource->getSignalInfo().getChannelCount() != mixxx::audio::ChannelCount::stereo()) {
        pSource = std::make_shared<mixxx::AudioSourceStereoProxy>(pSource, kBlockFrames);
    }
    return pSource;
}

/// Reads `count` stereo frames of `pSource` from `first` (relative to its
/// first frame) into `pOut`; frames that do not decode are silent.
void readFrames(const mixxx::AudioSourcePointer& pSource,
        mixxx::SampleBuffer* pBuffer,
        SINT first,
        SINT count,
        std::vector<float>* pOut) {
    pOut->assign(static_cast<std::size_t>(2 * count), 0.0f);
    const mixxx::IndexRange range = pSource->frameIndexRange();
    SINT done = 0;
    while (done < count) {
        const SINT start = range.start() + first + done;
        const SINT chunk = std::min<SINT>(count - done, pBuffer->size() / 2);
        const mixxx::IndexRange request = mixxx::IndexRange::forward(start, chunk);
        const mixxx::ReadableSampleFrames readable =
                pSource->readSampleFrames(mixxx::WritableSampleFrames(
                        request, mixxx::SampleBuffer::WritableSlice(*pBuffer)));
        const mixxx::IndexRange read = intersect(readable.frameIndexRange(), request);
        if (!read.empty()) {
            std::copy_n(readable.readableData(2 *
                                (read.start() -
                                        readable.frameIndexRange().start())),
                    2 * read.length(),
                    pOut->begin() + 2 * (done + read.start() - start));
        }
        done += chunk;
    }
}

// QFile as the IO of libFLAC's metadata interface
size_t ioRead(void* ptr, size_t size, size_t nmemb, FLAC__IOHandle handle) {
    auto* pFile = static_cast<QFile*>(handle);
    const qint64 bytes = pFile->read(static_cast<char*>(ptr), static_cast<qint64>(size * nmemb));
    return bytes > 0 && size > 0 ? static_cast<size_t>(bytes) / size : 0;
}
size_t ioWrite(const void* ptr, size_t size, size_t nmemb, FLAC__IOHandle handle) {
    auto* pFile = static_cast<QFile*>(handle);
    const qint64 bytes =
            pFile->write(static_cast<const char*>(ptr), static_cast<qint64>(size * nmemb));
    return bytes > 0 && size > 0 ? static_cast<size_t>(bytes) / size : 0;
}
int ioSeek(FLAC__IOHandle handle, FLAC__int64 offset, int whence) {
    auto* pFile = static_cast<QFile*>(handle);
    qint64 position = offset;
    if (whence == SEEK_CUR) {
        position += pFile->pos();
    } else if (whence == SEEK_END) {
        position += pFile->size();
    }
    return pFile->seek(position) ? 0 : -1;
}
FLAC__int64 ioTell(FLAC__IOHandle handle) {
    return static_cast<QFile*>(handle)->pos();
}
int ioEof(FLAC__IOHandle handle) {
    return static_cast<QFile*>(handle)->atEnd() ? 1 : 0;
}
int ioClose(FLAC__IOHandle handle) {
    Q_UNUSED(handle);
    return 0;
}
const FLAC__IOCallbacks kIoCallbacks = {ioRead, ioWrite, ioSeek, ioTell, ioEof, ioClose};

/// The chain of metadata blocks of a FLAC file.
class Chain {
  public:
    Chain()
            : m_pChain(FLAC__metadata_chain_new()) {
    }
    ~Chain() {
        FLAC__metadata_chain_delete(m_pChain);
    }
    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;

    bool read(QFile* pFile) {
        return m_pChain && FLAC__metadata_chain_read_with_callbacks(m_pChain, pFile, kIoCallbacks);
    }
    FLAC__Metadata_Chain* get() const {
        return m_pChain;
    }

  private:
    FLAC__Metadata_Chain* m_pChain;
};

QString tagValue(const FLAC__StreamMetadata_VorbisComment& comment, const char* name) {
    const QByteArray prefix = QByteArray(name) + '=';
    for (FLAC__uint32 i = 0; i < comment.num_comments; ++i) {
        const QByteArray entry(reinterpret_cast<const char*>(comment.comments[i].entry),
                static_cast<qsizetype>(comment.comments[i].length));
        if (entry.startsWith(prefix)) {
            return QString::fromUtf8(entry.mid(prefix.size()));
        }
    }
    return QString();
}

bool appendTag(FLAC__StreamMetadata* pBlock, const char* name, const QByteArray& value) {
    FLAC__StreamMetadata_VorbisComment_Entry entry;
    return FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
                   &entry, name, value.constData()) &&
            FLAC__metadata_object_vorbiscomment_append_comment(pBlock, entry, false);
}

} // namespace

// The libFLAC encoder, writing to a QFile.
class StemSidecarWriter::Encoder {
  public:
    ~Encoder() {
        if (m_pEncoder) {
            FLAC__stream_encoder_delete(m_pEncoder);
        }
        if (m_pPadding) {
            FLAC__metadata_object_delete(m_pPadding);
        }
    }

    bool open(const QString& path, int sampleRate, SINT frames, QString* pError) {
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
            return fail(pError,
                    QStringLiteral("Cannot write %1: %2")
                            .arg(path, m_file.errorString()));
        }
        m_pEncoder = FLAC__stream_encoder_new();
        m_pPadding = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
        if (!m_pEncoder || !m_pPadding) {
            return fail(pError, QStringLiteral("Out of memory"));
        }
        m_pPadding->length = kPaddingBytes;
        FLAC__stream_encoder_set_channels(m_pEncoder, StemSidecar::kChannels);
        FLAC__stream_encoder_set_bits_per_sample(m_pEncoder, StemSidecar::kBitsPerSample);
        FLAC__stream_encoder_set_sample_rate(m_pEncoder, static_cast<uint32_t>(sampleRate));
        FLAC__stream_encoder_set_compression_level(m_pEncoder, 5);
        FLAC__stream_encoder_set_total_samples_estimate(
                m_pEncoder, static_cast<FLAC__uint64>(frames));
        FLAC__stream_encoder_set_metadata(m_pEncoder, &m_pPadding, 1);
        const FLAC__StreamEncoderInitStatus status = FLAC__stream_encoder_init_stream(
                m_pEncoder, &Encoder::onWrite, &Encoder::onSeek, &Encoder::onTell, nullptr, this);
        if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            return fail(pError,
                    QStringLiteral("Cannot start the FLAC encoder: %1")
                            .arg(QString::fromLatin1(FLAC__StreamEncoderInitStatusString[status])));
        }
        m_open = true;
        return true;
    }

    bool process(const std::vector<qint32>& interleaved, SINT frames, QString* pError) {
        static_assert(sizeof(qint32) == sizeof(FLAC__int32));
        if (!FLAC__stream_encoder_process_interleaved(m_pEncoder,
                    reinterpret_cast<const FLAC__int32*>(interleaved.data()),
                    static_cast<uint32_t>(frames))) {
            return fail(pError, QStringLiteral("The FLAC encoder failed"));
        }
        return true;
    }

    /// Ends the stream and writes `tags` into the padding.
    bool finish(const QList<std::pair<const char*, QByteArray>>& tags, QString* pError) {
        m_open = false;
        if (!FLAC__stream_encoder_finish(m_pEncoder)) {
            return fail(pError, QStringLiteral("The FLAC encoder failed to finish"));
        }
        if (!m_file.seek(0)) {
            return fail(pError, QStringLiteral("Cannot read back the sidecar"));
        }
        Chain chain;
        if (!chain.read(&m_file)) {
            return fail(pError, QStringLiteral("Cannot read back the sidecar's metadata"));
        }
        // The encoder wrote an empty comment block: the tags go into it
        bool tagged = false;
        FLAC__Metadata_Iterator* pIterator = FLAC__metadata_iterator_new();
        if (pIterator) {
            FLAC__metadata_iterator_init(pIterator, chain.get());
            do {
                FLAC__StreamMetadata* pBlock = FLAC__metadata_iterator_get_block(pIterator);
                if (pBlock->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
                    tagged = true;
                    for (const auto& [name, value] : tags) {
                        tagged = tagged && appendTag(pBlock, name, value);
                    }
                    break;
                }
            } while (FLAC__metadata_iterator_next(pIterator));
            FLAC__metadata_iterator_delete(pIterator);
        }
        if (!tagged || FLAC__metadata_chain_check_if_tempfile_needed(chain.get(), true)) {
            return fail(pError, QStringLiteral("Cannot tag the sidecar"));
        }
        if (!FLAC__metadata_chain_write_with_callbacks(chain.get(), true, &m_file, kIoCallbacks)) {
            return fail(pError, QStringLiteral("Cannot write the sidecar's tags"));
        }
        m_file.close();
        return true;
    }

    void abandon() {
        if (m_open) {
            FLAC__stream_encoder_finish(m_pEncoder);
            m_open = false;
        }
        m_file.close();
    }

  private:
    static FLAC__StreamEncoderWriteStatus onWrite(const FLAC__StreamEncoder* pEncoder,
            const FLAC__byte buffer[],
            size_t bytes,
            uint32_t samples,
            uint32_t currentFrame,
            void* pClientData) {
        Q_UNUSED(pEncoder);
        Q_UNUSED(samples);
        Q_UNUSED(currentFrame);
        auto* pThis = static_cast<Encoder*>(pClientData);
        return pThis->m_file.write(reinterpret_cast<const char*>(buffer),
                       static_cast<qint64>(bytes)) == static_cast<qint64>(bytes)
                ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK
                : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
    static FLAC__StreamEncoderSeekStatus onSeek(const FLAC__StreamEncoder* pEncoder,
            FLAC__uint64 offset,
            void* pClientData) {
        Q_UNUSED(pEncoder);
        auto* pThis = static_cast<Encoder*>(pClientData);
        return pThis->m_file.seek(static_cast<qint64>(offset))
                ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK
                : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    }
    static FLAC__StreamEncoderTellStatus onTell(const FLAC__StreamEncoder* pEncoder,
            FLAC__uint64* pOffset,
            void* pClientData) {
        Q_UNUSED(pEncoder);
        auto* pThis = static_cast<Encoder*>(pClientData);
        *pOffset = static_cast<FLAC__uint64>(pThis->m_file.pos());
        return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
    }

    QFile m_file;
    FLAC__StreamEncoder* m_pEncoder = nullptr;
    FLAC__StreamMetadata* m_pPadding = nullptr;
    bool m_open = false;
};

// The original, read in order, with the fingerprint of what passes.
class StemSidecarWriter::Original {
  public:
    bool open(const TrackPointer& pTrack, QString* pError) {
        m_pSource = openOriginal(pTrack, pError);
        if (!m_pSource) {
            return false;
        }
        m_sampleRate = static_cast<int>(m_pSource->getSignalInfo().getSampleRate());
        m_frames = m_pSource->frameIndexRange().length();
        m_starts = excerptStarts(m_frames);
        m_excerptLength = excerptLength(m_frames);
        m_fingerprint.assign(m_starts.size() * static_cast<std::size_t>(2 * m_excerptLength), 0);
        return true;
    }

    int sampleRate() const {
        return m_sampleRate;
    }
    SINT frames() const {
        return m_frames;
    }
    SINT next() const {
        return m_next;
    }

    /// The next `count` frames, stereo interleaved.
    void read(SINT count, std::vector<float>* pOut) {
        readFrames(m_pSource, &m_buffer, m_next, count, pOut);
        for (std::size_t k = 0; k < m_starts.size(); ++k) {
            const SINT from = std::max(m_next, m_starts[k]);
            const SINT to = std::min(m_next + count, m_starts[k] + m_excerptLength);
            for (SINT frame = from; frame < to; ++frame) {
                const std::size_t at = 2 *
                        (k * static_cast<std::size_t>(m_excerptLength) +
                                static_cast<std::size_t>(frame - m_starts[k]));
                const std::size_t in = static_cast<std::size_t>(2 * (frame - m_next));
                m_fingerprint[at] = quantise16((*pOut)[in]);
                m_fingerprint[at + 1] = quantise16((*pOut)[in + 1]);
            }
        }
        m_next += count;
    }

    QByteArray fingerprint() const {
        QByteArray bytes(static_cast<qsizetype>(2 * m_fingerprint.size()), '\0');
        qToLittleEndian<qint16>(m_fingerprint.data(),
                static_cast<qsizetype>(m_fingerprint.size()),
                bytes.data());
        return bytes;
    }

    void close() {
        if (m_pSource) {
            m_pSource->close();
        }
    }

  private:
    mixxx::AudioSourcePointer m_pSource;
    mixxx::SampleBuffer m_buffer{2 * kBlockFrames};
    int m_sampleRate = 0;
    SINT m_frames = 0;
    SINT m_next = 0;
    std::vector<SINT> m_starts;
    SINT m_excerptLength = 0;
    std::vector<qint16> m_fingerprint;
};

QString StemSidecar::pathFor(const QString& originalPath) {
    const QFileInfo info(originalPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".stems.flac"));
}

std::optional<StemSidecar::Info> StemSidecar::readInfo(
        const QString& sidecarPath, QString* pError) {
    QFile file(sidecarPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(pError, QStringLiteral("No sidecar at %1").arg(sidecarPath));
        return std::nullopt;
    }
    Chain chain;
    if (!chain.read(&file)) {
        fail(pError, QStringLiteral("Not a FLAC file: %1").arg(sidecarPath));
        return std::nullopt;
    }
    Info info;
    info.bytes = file.size();
    bool hasFormat = false;
    bool hasTags = false;
    FLAC__Metadata_Iterator* pIterator = FLAC__metadata_iterator_new();
    if (!pIterator) {
        fail(pError, QStringLiteral("Out of memory"));
        return std::nullopt;
    }
    FLAC__metadata_iterator_init(pIterator, chain.get());
    do {
        const FLAC__StreamMetadata* pBlock = FLAC__metadata_iterator_get_block(pIterator);
        if (pBlock->type == FLAC__METADATA_TYPE_STREAMINFO) {
            const FLAC__StreamMetadata_StreamInfo& stream = pBlock->data.stream_info;
            hasFormat = stream.channels == kChannels && stream.bits_per_sample == kBitsPerSample;
            info.sampleRate = static_cast<int>(stream.sample_rate);
            info.frames = static_cast<SINT>(stream.total_samples);
        } else if (pBlock->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            const FLAC__StreamMetadata_VorbisComment& comment = pBlock->data.vorbis_comment;
            if (tagValue(comment, kTagSources).isEmpty()) {
                continue;
            }
            hasTags = tagValue(comment, kTagSources) == QLatin1String("drums,bass,other,vocals") &&
                    tagValue(comment, kTagScale).toDouble() == kScale &&
                    tagValue(comment, kTagFrames).toLongLong() == info.frames &&
                    tagValue(comment, kTagSampleRate).toInt() == info.sampleRate;
            info.separator = tagValue(comment, kTagSeparator);
            info.sourceRevision = tagValue(comment, kTagSourceRevision);
            info.fingerprint = QByteArray::fromBase64(
                    tagValue(comment, kTagFingerprint).toLatin1());
        }
    } while (FLAC__metadata_iterator_next(pIterator));
    FLAC__metadata_iterator_delete(pIterator);
    if (!hasFormat || !hasTags || info.frames <= 0) {
        fail(pError, QStringLiteral("Not a stem sidecar: %1").arg(sidecarPath));
        return std::nullopt;
    }
    return info;
}

bool StemSidecar::isValidFor(const TrackPointer& pTrack, QString* pReason) {
    if (!pTrack) {
        return fail(pReason, QStringLiteral("No track"));
    }
    return isValidFor(pTrack, pathFor(pTrack->getLocation()), pReason);
}

bool StemSidecar::isValidFor(
        const TrackPointer& pTrack, const QString& sidecarPath, QString* pReason) {
    const std::optional<Info> info = readInfo(sidecarPath, pReason);
    if (!info) {
        return false;
    }
    const mixxx::AudioSourcePointer pSource = openOriginal(pTrack, pReason);
    if (!pSource) {
        return false;
    }
    const int sampleRate = static_cast<int>(pSource->getSignalInfo().getSampleRate());
    const SINT frames = pSource->frameIndexRange().length();
    if (sampleRate != info->sampleRate || frames != info->frames) {
        pSource->close();
        return fail(pReason,
                QStringLiteral("The file decodes to %1 frames at %2 Hz, the "
                               "sidecar has %3 at %4 Hz")
                        .arg(QString::number(frames),
                                QString::number(sampleRate),
                                QString::number(info->frames),
                                QString::number(info->sampleRate)));
    }
    const std::vector<SINT> starts = excerptStarts(frames);
    const SINT length = excerptLength(frames);
    if (info->fingerprint.size() !=
            static_cast<qsizetype>(starts.size()) * 4 * static_cast<qsizetype>(length)) {
        pSource->close();
        return fail(pReason, QStringLiteral("The sidecar's fingerprint has another size"));
    }
    mixxx::SampleBuffer buffer(2 * kBlockFrames);
    std::vector<float> excerpt;
    for (std::size_t k = 0; k < starts.size(); ++k) {
        readFrames(pSource, &buffer, starts[k], length, &excerpt);
        for (std::size_t i = 0; i < excerpt.size(); ++i) {
            const qint16 stored = qFromLittleEndian<qint16>(info->fingerprint.constData() +
                    2 * (k * excerpt.size() + i));
            if (std::abs(quantise16(excerpt[i]) - stored) > kFingerprintTolerance) {
                pSource->close();
                return fail(pReason,
                        QStringLiteral("The file does not decode as when its stems were made"));
            }
        }
    }
    pSource->close();
    return true;
}

namespace {

struct DecodeState {
    QFile file;
    std::vector<float>* pSamples = nullptr;
    bool failed = false;
};

FLAC__StreamDecoderReadStatus decodeRead(
        const FLAC__StreamDecoder* pDecoder, FLAC__byte buffer[], size_t* pBytes, void* pData) {
    Q_UNUSED(pDecoder);
    auto* pState = static_cast<DecodeState*>(pData);
    const qint64 read =
            pState->file.read(reinterpret_cast<char*>(buffer), static_cast<qint64>(*pBytes));
    if (read < 0) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    *pBytes = static_cast<size_t>(read);
    return read == 0 ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM
                     : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus decodeWrite(const FLAC__StreamDecoder* pDecoder,
        const FLAC__Frame* pFrame,
        const FLAC__int32* const buffer[],
        void* pData) {
    Q_UNUSED(pDecoder);
    auto* pState = static_cast<DecodeState*>(pData);
    const uint32_t channels = pFrame->header.channels;
    if (channels != StemSidecar::kChannels) {
        pState->failed = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    for (uint32_t i = 0; i < pFrame->header.blocksize; ++i) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            pState->pSamples->push_back(
                    static_cast<float>(static_cast<double>(buffer[channel][i]) / kFullScale));
        }
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void decodeError(const FLAC__StreamDecoder* pDecoder,
        FLAC__StreamDecoderErrorStatus status,
        void* pData) {
    Q_UNUSED(pDecoder);
    Q_UNUSED(status);
    static_cast<DecodeState*>(pData)->failed = true;
}

} // namespace

bool StemSidecar::decode(
        const QString& sidecarPath, std::vector<float>* pSamples, SINT* pFrames, QString* pError) {
    DecodeState state;
    state.file.setFileName(sidecarPath);
    state.pSamples = pSamples;
    pSamples->clear();
    if (!state.file.open(QIODevice::ReadOnly)) {
        return fail(pError, QStringLiteral("Cannot read %1").arg(sidecarPath));
    }
    FLAC__StreamDecoder* pDecoder = FLAC__stream_decoder_new();
    if (!pDecoder) {
        return fail(pError, QStringLiteral("Out of memory"));
    }
    bool ok = FLAC__stream_decoder_init_stream(pDecoder,
                      decodeRead,
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      decodeWrite,
                      nullptr,
                      decodeError,
                      &state) == FLAC__STREAM_DECODER_INIT_STATUS_OK;
    ok = ok && FLAC__stream_decoder_process_until_end_of_stream(pDecoder) && !state.failed;
    FLAC__stream_decoder_finish(pDecoder);
    FLAC__stream_decoder_delete(pDecoder);
    if (!ok) {
        return fail(pError, QStringLiteral("Cannot decode %1").arg(sidecarPath));
    }
    *pFrames = static_cast<SINT>(pSamples->size()) / kChannels;
    return true;
}

struct StemSidecarReader::State {
    QFile file;
    FLAC__StreamDecoder* pDecoder = nullptr;
    /// Decoded and not read yet, interleaved.
    std::vector<float> pending;
    int sampleRate = 0;
    SINT frames = 0;
    bool failed = false;
    bool ended = false;

    ~State() {
        if (pDecoder) {
            FLAC__stream_decoder_finish(pDecoder);
            FLAC__stream_decoder_delete(pDecoder);
        }
    }

    static FLAC__StreamDecoderReadStatus onRead(const FLAC__StreamDecoder* pDecoder,
            FLAC__byte buffer[],
            size_t* pBytes,
            void* pData) {
        Q_UNUSED(pDecoder);
        auto* pState = static_cast<State*>(pData);
        const qint64 read =
                pState->file.read(reinterpret_cast<char*>(buffer), static_cast<qint64>(*pBytes));
        if (read < 0) {
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        *pBytes = static_cast<size_t>(read);
        return read == 0 ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM
                         : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    static FLAC__StreamDecoderWriteStatus onWrite(const FLAC__StreamDecoder* pDecoder,
            const FLAC__Frame* pFrame,
            const FLAC__int32* const buffer[],
            void* pData) {
        Q_UNUSED(pDecoder);
        auto* pState = static_cast<State*>(pData);
        if (pFrame->header.channels != StemSidecar::kChannels) {
            pState->failed = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        for (uint32_t i = 0; i < pFrame->header.blocksize; ++i) {
            for (uint32_t channel = 0; channel < StemSidecar::kChannels; ++channel) {
                pState->pending.push_back(static_cast<float>(
                        static_cast<double>(buffer[channel][i]) / kFullScale));
            }
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    static void onError(const FLAC__StreamDecoder* pDecoder,
            FLAC__StreamDecoderErrorStatus status,
            void* pData) {
        Q_UNUSED(pDecoder);
        Q_UNUSED(status);
        static_cast<State*>(pData)->failed = true;
    }
};

StemSidecarReader::StemSidecarReader() = default;
StemSidecarReader::~StemSidecarReader() = default;

bool StemSidecarReader::open(const QString& sidecarPath, QString* pError) {
    const std::optional<StemSidecar::Info> info = StemSidecar::readInfo(sidecarPath, pError);
    if (!info) {
        return false;
    }
    m_pState = std::make_unique<State>();
    m_pState->sampleRate = info->sampleRate;
    m_pState->frames = info->frames;
    m_pState->file.setFileName(sidecarPath);
    m_pState->pDecoder = FLAC__stream_decoder_new();
    if (!m_pState->file.open(QIODevice::ReadOnly) || !m_pState->pDecoder ||
            FLAC__stream_decoder_init_stream(m_pState->pDecoder,
                    &State::onRead,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    &State::onWrite,
                    nullptr,
                    &State::onError,
                    m_pState.get()) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        m_pState.reset();
        return fail(pError, QStringLiteral("Cannot read %1").arg(sidecarPath));
    }
    return true;
}

int StemSidecarReader::sampleRate() const {
    return m_pState ? m_pState->sampleRate : 0;
}

SINT StemSidecarReader::frames() const {
    return m_pState ? m_pState->frames : 0;
}

bool StemSidecarReader::read(SINT count, std::vector<float>* pFrames, QString* pError) {
    if (!m_pState) {
        return fail(pError, QStringLiteral("No sidecar"));
    }
    const std::size_t wanted = static_cast<std::size_t>(StemSidecar::kChannels * count);
    while (m_pState->pending.size() < wanted && !m_pState->ended) {
        if (!FLAC__stream_decoder_process_single(m_pState->pDecoder) || m_pState->failed) {
            return fail(pError, QStringLiteral("Cannot decode the sidecar"));
        }
        m_pState->ended = FLAC__stream_decoder_get_state(m_pState->pDecoder) ==
                FLAC__STREAM_DECODER_END_OF_STREAM;
    }
    const std::size_t taken = std::min(wanted, m_pState->pending.size());
    pFrames->assign(m_pState->pending.begin(),
            m_pState->pending.begin() + static_cast<std::ptrdiff_t>(taken));
    m_pState->pending.erase(m_pState->pending.begin(),
            m_pState->pending.begin() + static_cast<std::ptrdiff_t>(taken));
    return true;
}

StemSidecarWriter::StemSidecarWriter(Options options)
        : m_options(std::move(options)) {
    if (m_options.sidecarPath.isEmpty() && m_options.pTrack) {
        m_options.sidecarPath = StemSidecar::pathFor(m_options.pTrack->getLocation());
    }
}

StemSidecarWriter::~StemSidecarWriter() {
    if (!m_finished) {
        abandon();
    }
}

void StemSidecarWriter::abandon() {
    if (m_pEncoder) {
        m_pEncoder->abandon();
        m_pEncoder.reset();
        QFile::remove(m_options.sidecarPath + QStringLiteral(".part"));
    }
    if (m_pOriginal) {
        m_pOriginal->close();
        m_pOriginal.reset();
    }
}

bool StemSidecarWriter::begin(const StemSeparationSource& source, QString* pError) {
    abandon();
    m_pending.clear();
    m_nextStemFrame = 0;
    m_encodedFrames = 0;
    m_clipped = 0;
    m_finished = false;
    m_pOriginal = std::make_unique<Original>();
    if (!m_pOriginal->open(m_options.pTrack, pError)) {
        return false;
    }
    if (source.originalSampleRate > 0 && source.originalSampleRate != m_pOriginal->sampleRate()) {
        return fail(pError, QStringLiteral("The original decodes at another sample rate"));
    }
    m_pResampler = std::make_unique<Resampler>(source.stemSampleRate,
            m_pOriginal->sampleRate(),
            static_cast<int>(kStemRows.size()));
    m_pEncoder = std::make_unique<Encoder>();
    return m_pEncoder->open(m_options.sidecarPath + QStringLiteral(".part"),
            m_pOriginal->sampleRate(),
            m_pOriginal->frames(),
            pError);
}

bool StemSidecarWriter::write(
        SINT firstFrame, SINT frameCount, std::span<const float> stems, QString* pError) {
    if (!m_pEncoder || m_finished) {
        return fail(pError, QStringLiteral("The sidecar is not open"));
    }
    if (firstFrame != m_nextStemFrame) {
        return fail(pError, QStringLiteral("The stems are not in order"));
    }
    if (stems.size() < static_cast<std::size_t>(8 * frameCount)) {
        return fail(pError, QStringLiteral("Fewer stems than frames"));
    }
    m_resampleInput.resize(kStemRows.size() * static_cast<std::size_t>(frameCount));
    for (SINT frame = 0; frame < frameCount; ++frame) {
        for (std::size_t channel = 0; channel < kStemRows.size(); ++channel) {
            m_resampleInput[kStemRows.size() * static_cast<std::size_t>(frame) + channel] =
                    stems[static_cast<std::size_t>(kStemRows[channel] * frameCount + frame)];
        }
    }
    m_pResampler->process(m_resampleInput, &m_pending);
    m_nextStemFrame += frameCount;
    return encodePending(false, pError);
}

bool StemSidecarWriter::encodePending(bool last, QString* pError) {
    constexpr std::size_t kIn = kStemRows.size();
    std::vector<float> original;
    std::size_t used = 0;
    while (m_encodedFrames < m_pOriginal->frames()) {
        const SINT pendingFrames = static_cast<SINT>((m_pending.size() - used) / kIn);
        SINT count = std::min({kBlockFrames,
                pendingFrames,
                m_pOriginal->frames() - m_encodedFrames});
        if (count == 0) {
            if (!last) {
                break;
            }
            // Past the stems: the original alone, as `other`
            count = std::min(kBlockFrames, m_pOriginal->frames() - m_encodedFrames);
        }
        m_pOriginal->read(count, &original);
        m_block.resize(static_cast<std::size_t>(StemSidecar::kChannels * count));
        for (SINT frame = 0; frame < count; ++frame) {
            const bool hasStems = frame < pendingFrames;
            const float* pStems = m_pending.data() + used + kIn * static_cast<std::size_t>(frame);
            qint32* pOut = m_block.data() + StemSidecar::kChannels * frame;
            for (int channel = 0; channel < 2; ++channel) {
                const qint32 drums = hasStems ? quantise24(pStems[channel]) : 0;
                const qint32 bass = hasStems ? quantise24(pStems[2 + channel]) : 0;
                const qint32 vocals = hasStems ? quantise24(pStems[4 + channel]) : 0;
                const qint64 residual =
                        static_cast<qint64>(quantise24(original[static_cast<std::size_t>(
                                2 * frame + channel)])) -
                        drums - bass - vocals;
                const qint64 other = std::clamp<qint64>(residual, kMin24, kMax24);
                if (other != residual) {
                    ++m_clipped;
                }
                pOut[channel] = drums;
                pOut[2 + channel] = bass;
                pOut[4 + channel] = static_cast<qint32>(other);
                pOut[6 + channel] = vocals;
            }
        }
        if (!m_pEncoder->process(m_block, count, pError)) {
            return false;
        }
        used += kIn * static_cast<std::size_t>(std::min(count, pendingFrames));
        m_encodedFrames += count;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(used));
    if (last) {
        // Stems past the original's end, if any, are dropped
        m_pending.clear();
    }
    return true;
}

bool StemSidecarWriter::finish(QString* pError) {
    if (!m_pEncoder || m_finished) {
        return fail(pError, QStringLiteral("The sidecar is not open"));
    }
    m_pResampler->finish(&m_pending);
    if (!encodePending(true, pError)) {
        abandon();
        return false;
    }
    if (m_clipped > 0) {
        kLogger.warning() << "The residual of" << m_clipped
                          << "samples did not fit in 24 bits:" << m_options.sidecarPath;
    }
    const QList<std::pair<const char*, QByteArray>> tags = {
            {kTagSources, QByteArrayLiteral("drums,bass,other,vocals")},
            {kTagScale, QByteArray::number(StemSidecar::kScale)},
            {kTagFrames, QByteArray::number(m_pOriginal->frames())},
            {kTagSampleRate, QByteArray::number(m_pOriginal->sampleRate())},
            {kTagSeparator, m_options.separator.toUtf8()},
            {kTagSourceRevision, m_options.sourceRevision.toUtf8()},
            {kTagFingerprint, m_pOriginal->fingerprint().toBase64()},
    };
    m_pOriginal->close();
    const QString partPath = m_options.sidecarPath + QStringLiteral(".part");
    if (!m_pEncoder->finish(tags, pError)) {
        abandon();
        return false;
    }
    m_pEncoder.reset();
    QFile::remove(m_options.sidecarPath);
    if (!QFile::rename(partPath, m_options.sidecarPath)) {
        QFile::remove(partPath);
        return fail(pError,
                QStringLiteral("Cannot move the sidecar to %1")
                        .arg(m_options.sidecarPath));
    }
    m_finished = true;
    return true;
}

} // namespace crate
