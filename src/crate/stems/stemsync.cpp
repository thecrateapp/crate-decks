#include "crate/stems/stemsync.h"

#ifdef __FFMPEG__
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#endif

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QtConcurrentRun>
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include "crate/client/crateclient.h"
#include "crate/stems/stemsidecar.h"
#include "track/track.h"
#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::StemSync");

/// The streams of a server stem file, in order (STEM-D6), and the rows of a
/// StemSink write they go to: drums 0-1, bass 2-3, vocals 6-7.
constexpr std::array<int, 3> kFirstRows = {0, 2, 6};

bool fail(QString* pError, const QString& message) {
    if (pError) {
        *pError = message;
    }
    return false;
}

#ifdef __FFMPEG__
struct FormatDeleter {
    void operator()(AVFormatContext* pContext) const {
        avformat_close_input(&pContext);
    }
};
struct CodecDeleter {
    void operator()(AVCodecContext* pContext) const {
        avcodec_free_context(&pContext);
    }
};
struct ResamplerDeleter {
    void operator()(SwrContext* pContext) const {
        swr_free(&pContext);
    }
};
struct FrameDeleter {
    void operator()(AVFrame* pFrame) const {
        av_frame_free(&pFrame);
    }
};
struct PacketDeleter {
    void operator()(AVPacket* pPacket) const {
        av_packet_free(&pPacket);
    }
};

/// One AAC stream of the file, decoded to interleaved float stereo.
struct Stream {
    int index = -1;
    std::unique_ptr<AVCodecContext, CodecDeleter> pCodec;
    std::unique_ptr<SwrContext, ResamplerDeleter> pResampler;
    /// Decoded and not written yet.
    std::vector<float> samples;
    bool ended = false;

    SINT frames() const {
        return static_cast<SINT>(samples.size() / 2);
    }

    /// Appends the samples of `pFrame`, or what the resampler holds back
    /// when `pFrame` is null.
    bool append(const AVFrame* pFrame) {
        const int inCount = pFrame ? pFrame->nb_samples : 0;
        const int outCount = swr_get_out_samples(pResampler.get(), inCount);
        if (outCount <= 0) {
            return outCount == 0;
        }
        const std::size_t at = samples.size();
        samples.resize(at + 2 * static_cast<std::size_t>(outCount));
        auto* pOut = reinterpret_cast<uint8_t*>(samples.data() + at);
        const int converted = swr_convert(pResampler.get(),
                &pOut,
                outCount,
                pFrame ? const_cast<const uint8_t**>(pFrame->extended_data) : nullptr,
                inCount);
        if (converted < 0) {
            samples.resize(at);
            return false;
        }
        samples.resize(at + 2 * static_cast<std::size_t>(converted));
        return true;
    }
};

/// Receives the frames the decoder has.
bool drain(Stream* pStream, AVFrame* pFrame) {
    while (true) {
        const int received = avcodec_receive_frame(pStream->pCodec.get(), pFrame);
        if (received == AVERROR(EAGAIN)) {
            return true;
        }
        if (received == AVERROR_EOF) {
            pStream->ended = true;
            return pStream->append(nullptr);
        }
        if (received < 0) {
            return false;
        }
        const bool appended = pStream->append(pFrame);
        av_frame_unref(pFrame);
        if (!appended) {
            return false;
        }
    }
}

/// Writes the frames all streams have, or all of them with `last` (a
/// stream that ended is silent for the rest).
bool writeDecoded(std::array<Stream, 3>* pStreams,
        StemSink* pSink,
        SINT* pNext,
        bool last,
        std::vector<float>* pRows,
        QString* pError) {
    SINT count = last ? 0 : std::numeric_limits<SINT>::max();
    for (const Stream& stream : *pStreams) {
        count = last ? std::max(count, stream.frames()) : std::min(count, stream.frames());
    }
    if (count <= 0) {
        return true;
    }
    pRows->assign(static_cast<std::size_t>(8 * count), 0.0f);
    for (std::size_t s = 0; s < pStreams->size(); ++s) {
        Stream& stream = (*pStreams)[s];
        const SINT have = std::min(count, stream.frames());
        for (int channel = 0; channel < 2; ++channel) {
            float* pRow = pRows->data() + (kFirstRows[s] + channel) * count;
            for (SINT frame = 0; frame < have; ++frame) {
                pRow[frame] = stream.samples[static_cast<std::size_t>(2 * frame + channel)];
            }
        }
        stream.samples.erase(stream.samples.begin(),
                stream.samples.begin() + static_cast<std::ptrdiff_t>(2 * have));
    }
    if (!pSink->write(*pNext, count, *pRows, pError)) {
        return false;
    }
    *pNext += count;
    return true;
}

#endif // __FFMPEG__

} // namespace

bool decodeServerStems(const QString& path,
        StemSink* pSink,
        const std::atomic<bool>* pCancel,
        QString* pError) {
#ifndef __FFMPEG__
    Q_UNUSED(pSink);
    Q_UNUSED(pCancel);
    return fail(pError, QStringLiteral("No FFmpeg to decode the stem file %1").arg(path));
#else
    AVFormatContext* pOpened = nullptr;
    if (avformat_open_input(&pOpened, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        return fail(pError, QStringLiteral("Cannot open the stem file %1").arg(path));
    }
    const std::unique_ptr<AVFormatContext, FormatDeleter> pFormat(pOpened);
    if (avformat_find_stream_info(pFormat.get(), nullptr) < 0) {
        return fail(pError, QStringLiteral("Cannot read the streams of %1").arg(path));
    }
    std::array<Stream, 3> streams;
    std::size_t found = 0;
    for (unsigned int i = 0; i < pFormat->nb_streams && found < streams.size(); ++i) {
        const AVCodecParameters* pParameters = pFormat->streams[i]->codecpar;
        if (pParameters->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        Stream& stream = streams[found++];
        stream.index = static_cast<int>(i);
        const AVCodec* pDecoder = avcodec_find_decoder(pParameters->codec_id);
        stream.pCodec.reset(avcodec_alloc_context3(pDecoder));
        if (!pDecoder || !stream.pCodec ||
                avcodec_parameters_to_context(stream.pCodec.get(), pParameters) < 0 ||
                avcodec_open2(stream.pCodec.get(), pDecoder, nullptr) < 0) {
            return fail(pError, QStringLiteral("Cannot decode the stems of %1").arg(path));
        }
    }
    if (found != streams.size()) {
        return fail(pError,
                QStringLiteral("%1 has %2 audio streams, not drums, bass and vocals")
                        .arg(path, QString::number(found)));
    }
    const int sampleRate = streams[0].pCodec->sample_rate;
    for (Stream& stream : streams) {
        if (stream.pCodec->sample_rate != sampleRate) {
            return fail(pError, QStringLiteral("The stems of %1 differ in rate").arg(path));
        }
        SwrContext* pResampler = nullptr;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&pResampler,
                    &stereo,
                    AV_SAMPLE_FMT_FLT,
                    sampleRate,
                    &stream.pCodec->ch_layout,
                    stream.pCodec->sample_fmt,
                    sampleRate,
                    0,
                    nullptr) < 0 ||
                swr_init(pResampler) < 0) {
            swr_free(&pResampler);
            return fail(pError, QStringLiteral("Cannot convert the stems of %1").arg(path));
        }
        stream.pResampler.reset(pResampler);
    }

    StemSeparationSource source;
    source.stemSampleRate = sampleRate;
    if (pFormat->duration > 0) {
        source.frames = static_cast<SINT>(av_rescale(pFormat->duration, sampleRate, AV_TIME_BASE));
    }
    if (!pSink->begin(source, pError)) {
        return false;
    }
    const std::unique_ptr<AVPacket, PacketDeleter> pPacket(av_packet_alloc());
    const std::unique_ptr<AVFrame, FrameDeleter> pFrame(av_frame_alloc());
    if (!pPacket || !pFrame) {
        return fail(pError, QStringLiteral("Out of memory"));
    }
    std::vector<float> rows;
    SINT next = 0;
    while (av_read_frame(pFormat.get(), pPacket.get()) >= 0) {
        if (pCancel && pCancel->load()) {
            return fail(pError, QStringLiteral("Canceled"));
        }
        const auto stream = std::find_if(streams.begin(), streams.end(), [&](const Stream& s) {
            return s.index == pPacket->stream_index;
        });
        if (stream != streams.end()) {
            const int sent = avcodec_send_packet(stream->pCodec.get(), pPacket.get());
            if ((sent < 0 && sent != AVERROR(EAGAIN)) || !drain(&*stream, pFrame.get())) {
                av_packet_unref(pPacket.get());
                return fail(pError, QStringLiteral("Cannot decode the stems of %1").arg(path));
            }
        }
        av_packet_unref(pPacket.get());
        if (!writeDecoded(&streams, pSink, &next, false, &rows, pError)) {
            return false;
        }
    }
    for (Stream& stream : streams) {
        avcodec_send_packet(stream.pCodec.get(), nullptr);
        if (!drain(&stream, pFrame.get())) {
            return fail(pError, QStringLiteral("Cannot decode the stems of %1").arg(path));
        }
    }
    return writeDecoded(&streams, pSink, &next, true, &rows, pError);
#endif // __FFMPEG__
}

struct StemSync::Fetch {
    QString serverId;
    QString entityUid;
    QString cachedPath;
    QString sourceRevision;
    std::function<void(const Outcome&)> done;
    std::optional<StemAnalysis> serverAnalysis;
    /// The server's stem file while it downloads.
    QString downloadPath;
    std::unique_ptr<QFile> pDownload;
    bool finished = false;
};

StemSync::StemSync(Options options, QObject* parent)
        : QObject(parent),
          m_options(std::move(options)) {
    // One sidecar at a time: they decode a whole track
    m_pool.setMaxThreadCount(1);
}

StemSync::~StemSync() {
    m_stopping = true;
    m_pool.waitForDone();
}

bool StemSync::isCurrent(
        const QString& serverId, const QString& entityUid, const QString& sourceRevision) const {
    if (!m_options.currentRevision) {
        return true;
    }
    return m_options.currentRevision(serverId, entityUid) == sourceRevision;
}

void StemSync::fetch(const QString& serverId,
        const QString& entityUid,
        const QString& cachedPath,
        const QString& sourceRevision,
        std::function<void(const Outcome&)> done) {
    auto pFetch = std::make_shared<Fetch>();
    pFetch->serverId = serverId;
    pFetch->entityUid = entityUid.toLower();
    pFetch->cachedPath = cachedPath;
    pFetch->sourceRevision = sourceRevision;
    pFetch->done = std::move(done);
    fetchAnalysis(pFetch);
}

void StemSync::fetchAnalysis(const FetchPtr& pFetch) {
    Client* pClient = m_options.clientFor ? m_options.clientFor(pFetch->serverId) : nullptr;
    if (!pClient) {
        Outcome outcome;
        outcome.error = QStringLiteral("No connection to the server");
        finish(pFetch, outcome);
        return;
    }
    RequestOptions options;
    options.rawBody = true;
    Reply* pReply = pClient->get(
            QStringLiteral("/api/decks/tracks/%1/stem-analysis").arg(pFetch->entityUid), options);
    connect(pReply, &Reply::finished, this, [this, pFetch, pReply](const JsonResult& result) {
        if (result.ok()) {
            QString error;
            std::optional<StemAnalysis> analysis = StemAnalysis::fromJson(pReply->body(), &error);
            if (!analysis) {
                kLogger.warning() << "The server's stem analysis of" << pFetch->entityUid
                                  << "does not read:" << error;
            } else if (analysis->sourceRevision == pFetch->sourceRevision) {
                pFetch->serverAnalysis = std::move(analysis);
            }
        } else if (result.error().httpStatus != 404) {
            kLogger.info() << "No stem analysis of" << pFetch->entityUid << "from the server:"
                           << result.error().message;
        }
        fetchStemFile(pFetch);
    });
}

void StemSync::fetchStemFile(const FetchPtr& pFetch) {
    Client* pClient = m_options.clientFor ? m_options.clientFor(pFetch->serverId) : nullptr;
    if (!pClient) {
        Outcome outcome;
        outcome.error = QStringLiteral("No connection to the server");
        finish(pFetch, outcome);
        return;
    }
    // Beside the sidecar, so on the cache's volume
    pFetch->downloadPath = StemSidecar::pathFor(pFetch->cachedPath) +
            QStringLiteral(".server.part");
    RequestOptions options;
    options.streamBody = true;
    Reply* pReply = pClient->get(
            QStringLiteral("/api/decks/tracks/%1/stems").arg(pFetch->entityUid), options);
    connect(pReply, &Reply::headersReceived, this, [pFetch] {
        pFetch->pDownload = std::make_unique<QFile>(pFetch->downloadPath);
        if (!pFetch->pDownload->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pFetch->pDownload.reset();
        }
    });
    connect(pReply, &Reply::bodyReceived, this, [pFetch, pReply](const QByteArray& chunk) {
        if (!pFetch->pDownload || pFetch->pDownload->write(chunk) != chunk.size()) {
            pReply->cancel();
        }
    });
    connect(pReply, &Reply::finished, this, [this, pFetch](const JsonResult& result) {
        const bool written = pFetch->pDownload && pFetch->pDownload->flush();
        if (pFetch->pDownload) {
            pFetch->pDownload->close();
            pFetch->pDownload.reset();
        }
        if (!result.ok() || !written) {
            QFile::remove(pFetch->downloadPath);
            Outcome outcome;
            outcome.serverAnalysis = pFetch->serverAnalysis;
            if (!result.ok() && result.error().httpStatus == 404) {
                outcome.result = Outcome::Result::NoServerStems;
            } else {
                outcome.error = result.ok() ? QStringLiteral("Cannot store the server's stems")
                                            : result.error().message;
            }
            finish(pFetch, outcome);
            return;
        }
        buildSidecar(pFetch);
    });
}

void StemSync::buildSidecar(const FetchPtr& pFetch) {
    // Created and let go of on the main thread
    const TrackPointer pTrack = Track::newTemporary(pFetch->cachedPath);
    auto* pWatcher = new QFutureWatcher<QString>(this);
    connect(pWatcher, &QFutureWatcherBase::finished, this, [this, pWatcher, pFetch, pTrack] {
        pWatcher->deleteLater();
        const QString error = pWatcher->result();
        QFile::remove(pFetch->downloadPath);
        Outcome outcome;
        outcome.serverAnalysis = pFetch->serverAnalysis;
        if (!error.isEmpty()) {
            outcome.error = error;
            finish(pFetch, outcome);
            return;
        }
        outcome.result = Outcome::Result::ServerStems;
        outcome.sidecarPath = StemSidecar::pathFor(pFetch->cachedPath);
        finish(pFetch, outcome);
    });
    pWatcher->setFuture(QtConcurrent::run(&m_pool,
            [pTrack,
                    downloadPath = pFetch->downloadPath,
                    sourceRevision = pFetch->sourceRevision,
                    pStopping = &m_stopping]() -> QString {
                StemSidecarWriter::Options options;
                options.pTrack = pTrack;
                options.sourceRevision = sourceRevision;
                StemSidecarWriter writer(options);
                QString error;
                if (!decodeServerStems(downloadPath, &writer, pStopping, &error) ||
                        !writer.finish(&error)) {
                    return error.isEmpty() ? QStringLiteral("Cannot write the sidecar") : error;
                }
                return QString();
            }));
}

void StemSync::finish(const FetchPtr& pFetch, Outcome outcome) {
    if (pFetch->finished) {
        return;
    }
    pFetch->finished = true;
    if (!isCurrent(pFetch->serverId, pFetch->entityUid, pFetch->sourceRevision)) {
        // Made for a file the track no longer has
        if (outcome.result == Outcome::Result::ServerStems) {
            QFile::remove(outcome.sidecarPath);
        }
        outcome = Outcome();
        outcome.result = Outcome::Result::Discarded;
    } else if (outcome.result == Outcome::Result::Failed) {
        kLogger.warning() << "Cannot fetch the stems of" << pFetch->entityUid << ":"
                          << outcome.error;
    }
    if (pFetch->done) {
        pFetch->done(outcome);
    }
}

void StemSync::upload(const QString& serverId,
        const QString& entityUid,
        const StemAnalysis& analysis,
        std::function<void(UploadResult)> done) {
    Client* pClient = m_options.clientFor ? m_options.clientFor(serverId) : nullptr;
    if (!pClient || !m_options.mayUpload || !m_options.mayUpload(serverId) ||
            !isCurrent(serverId, entityUid, analysis.sourceRevision)) {
        done(UploadResult::NotUploaded);
        return;
    }
    Reply* pReply = pClient->put(
            QStringLiteral("/api/decks/tracks/%1/stem-analysis").arg(entityUid.toLower()),
            QJsonDocument::fromJson(analysis.toJson()));
    connect(pReply,
            &Reply::finished,
            this,
            [entityUid, done = std::move(done)](const JsonResult& result) {
                if (!result.ok()) {
                    kLogger.warning() << "Cannot upload the stem analysis of" << entityUid << ":"
                                      << result.error().message;
                    done(result.error().httpStatus == 409 || result.error().httpStatus == 403
                                    ? UploadResult::NotUploaded
                                    : UploadResult::Failed);
                    return;
                }
                const QString stored =
                        result.value().object().value(QStringLiteral("result")).toString();
                done(stored == QLatin1String("kept_stronger") ? UploadResult::KeptStronger
                                                              : UploadResult::Stored);
            });
}

} // namespace crate

#include "moc_stemsync.cpp"
