#include "crate/stems/nistemexport.h"

#ifdef __FFMPEG__
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}
#endif

#include <QColor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "audio/types.h"
#include "crate/stems/stemsidecar.h"
#include "crate/ui/skin/stemcolors.h"
#include "sources/audiosourcestereoproxy.h"
#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "util/samplebuffer.h"

namespace crate {

namespace {

constexpr int kStreams = 5;
constexpr int kBitRate = 256000;

bool fail(QString* pError, const QString& message) {
    if (pError) {
        *pError = message;
    }
    return false;
}

QByteArray box(const char* type, const QByteArray& payload) {
    QByteArray bytes(4, '\0');
    qToBigEndian(static_cast<quint32>(8 + payload.size()), bytes.data());
    bytes.append(type, 4);
    bytes.append(payload);
    return bytes;
}

/// The header of a box at `offset` of `data` (or of the file): its size
/// and header length. 0 for a malformed one.
struct BoxHeader {
    quint64 size = 0;
    int headerBytes = 8;
    QByteArray type;
};

BoxHeader parseHeader(const QByteArray& header, quint64 remaining) {
    BoxHeader box;
    if (header.size() < 8) {
        return box;
    }
    const quint32 size32 = qFromBigEndian<quint32>(header.constData());
    box.type = header.mid(4, 4);
    if (size32 == 1) {
        if (header.size() < 16) {
            return box;
        }
        box.size = qFromBigEndian<quint64>(header.constData() + 8);
        box.headerBytes = 16;
    } else if (size32 == 0) {
        box.size = remaining;
    } else {
        box.size = size32;
    }
    if (box.size < static_cast<quint64>(box.headerBytes) || box.size > remaining) {
        box.size = 0;
    }
    return box;
}

/// Puts the `stem` box with `manifest` as the first child of `moov/udta`
/// (made if there is none) of the MP4 at `path`, whose `moov` is its last
/// box, as the muxer writes it without faststart: no chunk offset moves.
bool insertStemBox(const QString& path, const QByteArray& manifest, QString* pError) {
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return fail(pError, QStringLiteral("Cannot open %1").arg(path));
    }
    const quint64 fileSize = static_cast<quint64>(file.size());
    quint64 offset = 0;
    std::optional<std::pair<quint64, BoxHeader>> moov;
    while (offset < fileSize) {
        file.seek(static_cast<qint64>(offset));
        const BoxHeader header = parseHeader(file.read(16), fileSize - offset);
        if (header.size == 0) {
            return fail(pError, QStringLiteral("A malformed box in %1").arg(path));
        }
        if (header.type == "moov") {
            moov = std::make_pair(offset, header);
        }
        offset += header.size;
    }
    if (!moov || moov->first + moov->second.size != fileSize ||
            moov->second.headerBytes != 8) {
        return fail(pError, QStringLiteral("The movie box is not at the end of %1").arg(path));
    }
    file.seek(static_cast<qint64>(moov->first));
    QByteArray movie = file.read(static_cast<qint64>(moov->second.size));
    const QByteArray stem = box("stem", manifest);
    // The user data box among the movie's children
    qsizetype child = 8;
    qsizetype udta = -1;
    while (child < movie.size()) {
        const BoxHeader header = parseHeader(
                movie.mid(child, 16), static_cast<quint64>(movie.size() - child));
        if (header.size == 0) {
            return fail(pError, QStringLiteral("A malformed movie box in %1").arg(path));
        }
        if (header.type == "udta" && header.headerBytes == 8) {
            udta = child;
            break;
        }
        child += static_cast<qsizetype>(header.size);
    }
    if (udta >= 0) {
        const quint32 udtaSize = qFromBigEndian<quint32>(movie.constData() + udta);
        movie.insert(udta + 8, stem);
        qToBigEndian(static_cast<quint32>(udtaSize + stem.size()), movie.data() + udta);
    } else {
        movie.append(box("udta", stem));
    }
    qToBigEndian(static_cast<quint32>(movie.size()), movie.data());
    file.seek(static_cast<qint64>(moov->first));
    if (file.write(movie) != movie.size()) {
        return fail(pError, QStringLiteral("Cannot write %1").arg(path));
    }
    return true;
}

#ifdef __FFMPEG__
struct OutputDeleter {
    void operator()(AVFormatContext* pContext) const {
        if (pContext->pb) {
            avio_closep(&pContext->pb);
        }
        avformat_free_context(pContext);
    }
};
struct CodecDeleter {
    void operator()(AVCodecContext* pContext) const {
        avcodec_free_context(&pContext);
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

/// Sends `pFrame` (null to flush) and writes the packets that come out.
bool encode(AVFormatContext* pOutput,
        AVCodecContext* pCodec,
        AVStream* pStream,
        const AVFrame* pFrame,
        AVPacket* pPacket) {
    if (avcodec_send_frame(pCodec, pFrame) < 0) {
        return false;
    }
    while (true) {
        const int received = avcodec_receive_packet(pCodec, pPacket);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
            return true;
        }
        if (received < 0) {
            return false;
        }
        av_packet_rescale_ts(pPacket, pCodec->time_base, pStream->time_base);
        pPacket->stream_index = pStream->index;
        if (av_interleaved_write_frame(pOutput, pPacket) < 0) {
            return false;
        }
    }
}
#endif

} // namespace

QByteArray niStemManifest(const std::array<QColor, 4>& colors) {
    static const std::array<const char*, 4> kNames = {"Drums", "Bass", "Other", "Vocals"};
    QJsonArray stems;
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        stems.append(QJsonObject{
                {QStringLiteral("name"), QString::fromLatin1(kNames[i])},
                {QStringLiteral("color"), colors[i].name()},
        });
    }
    // The mastering chain of the format, off
    const QJsonObject compressor{
            {QStringLiteral("enabled"), false},
            {QStringLiteral("ratio"), 3},
            {QStringLiteral("output_gain"), 0.5},
            {QStringLiteral("release"), 0.3},
            {QStringLiteral("attack"), 0.003},
            {QStringLiteral("input_gain"), 0.5},
            {QStringLiteral("threshold"), 0},
            {QStringLiteral("hp_cutoff"), 300},
            {QStringLiteral("dry_wet"), 50},
    };
    const QJsonObject limiter{
            {QStringLiteral("enabled"), false},
            {QStringLiteral("release"), 0.05},
            {QStringLiteral("threshold"), 0},
            {QStringLiteral("ceiling"), -0.35},
    };
    return QJsonDocument(QJsonObject{
                                 {QStringLiteral("version"), 1},
                                 {QStringLiteral("mastering_dsp"),
                                         QJsonObject{
                                                 {QStringLiteral("compressor"), compressor},
                                                 {QStringLiteral("limiter"), limiter},
                                         }},
                                 {QStringLiteral("stems"), stems},
                         })
            .toJson(QJsonDocument::Compact);
}

bool exportNiStem(const TrackPointer& pTrack,
        const QString& sidecarPath,
        const QString& outPath,
        const std::atomic<bool>* pCancel,
        QString* pError) {
#ifndef __FFMPEG__
    Q_UNUSED(pTrack);
    Q_UNUSED(sidecarPath);
    Q_UNUSED(pCancel);
    return fail(pError, QStringLiteral("No FFmpeg to write %1").arg(outPath));
#else
    if (!pTrack) {
        return fail(pError, QStringLiteral("No track"));
    }
    StemSidecarReader sidecar;
    if (!sidecar.open(sidecarPath, pError)) {
        return false;
    }
    mixxx::AudioSource::OpenParams openParams;
    openParams.setChannelCount(mixxx::audio::ChannelCount::stereo());
    mixxx::AudioSourcePointer pMaster = SoundSourceProxy(pTrack).openAudioSource(openParams);
    if (!pMaster) {
        return fail(pError, QStringLiteral("Cannot decode %1").arg(pTrack->getLocation()));
    }
    if (pMaster->getSignalInfo().getChannelCount() != mixxx::audio::ChannelCount::stereo()) {
        pMaster = std::make_shared<mixxx::AudioSourceStereoProxy>(pMaster, 4096);
    }
    const int sampleRate = static_cast<int>(pMaster->getSignalInfo().getSampleRate());
    if (sampleRate != sidecar.sampleRate()) {
        pMaster->close();
        return fail(pError, QStringLiteral("The sidecar is not at the track's rate"));
    }
    const QString partPath = outPath + QStringLiteral(".part");
    const auto cleanUp = [&] {
        pMaster->close();
        QFile::remove(partPath);
    };

    AVFormatContext* pAllocated = nullptr;
    if (avformat_alloc_output_context2(
                &pAllocated, nullptr, "mp4", partPath.toUtf8().constData()) < 0) {
        cleanUp();
        return fail(pError, QStringLiteral("Cannot start an MP4 file"));
    }
    std::unique_ptr<AVFormatContext, OutputDeleter> pOutput(pAllocated);
    const AVCodec* pEncoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    std::array<std::unique_ptr<AVCodecContext, CodecDeleter>, kStreams> codecs;
    std::array<AVStream*, kStreams> streams{};
    static const std::array<const char*, kStreams> kHandlers = {
            "Master", "Drums", "Bass", "Other", "Vocals"};
    for (int i = 0; i < kStreams; ++i) {
        AVStream* pStream = avformat_new_stream(pOutput.get(), nullptr);
        codecs[static_cast<std::size_t>(i)].reset(avcodec_alloc_context3(pEncoder));
        AVCodecContext* pCodec = codecs[static_cast<std::size_t>(i)].get();
        if (!pEncoder || !pStream || !pCodec) {
            cleanUp();
            return fail(pError, QStringLiteral("No AAC encoder"));
        }
        pCodec->sample_rate = sampleRate;
        av_channel_layout_default(&pCodec->ch_layout, 2);
        pCodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
        pCodec->bit_rate = kBitRate;
        pCodec->time_base = AVRational{1, sampleRate};
        if (pOutput->oformat->flags & AVFMT_GLOBALHEADER) {
            pCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (avcodec_open2(pCodec, pEncoder, nullptr) < 0 ||
                avcodec_parameters_from_context(pStream->codecpar, pCodec) < 0) {
            cleanUp();
            return fail(pError, QStringLiteral("Cannot start the AAC encoder"));
        }
        pStream->time_base = pCodec->time_base;
        // Players that do not know stems play the master
        pStream->disposition = i == 0 ? AV_DISPOSITION_DEFAULT : 0;
        av_dict_set(&pStream->metadata, "handler_name", kHandlers[static_cast<std::size_t>(i)], 0);
        streams[static_cast<std::size_t>(i)] = pStream;
    }
    if (avio_open(&pOutput->pb, partPath.toUtf8().constData(), AVIO_FLAG_WRITE) < 0 ||
            avformat_write_header(pOutput.get(), nullptr) < 0) {
        cleanUp();
        return fail(pError, QStringLiteral("Cannot write %1").arg(partPath));
    }

    const std::unique_ptr<AVFrame, FrameDeleter> pFrame(av_frame_alloc());
    const std::unique_ptr<AVPacket, PacketDeleter> pPacket(av_packet_alloc());
    const int frameSize = codecs[0]->frame_size > 0 ? codecs[0]->frame_size : 1024;
    const mixxx::IndexRange range = pMaster->frameIndexRange();
    mixxx::SampleBuffer buffer(2 * frameSize);
    std::vector<float> master;
    std::vector<float> stems;
    SINT position = 0;
    const SINT total = std::min(range.length(), sidecar.frames());
    while (position < total) {
        if (pCancel && pCancel->load()) {
            pOutput.reset();
            cleanUp();
            return fail(pError, QStringLiteral("Canceled"));
        }
        const SINT count = std::min<SINT>(frameSize, total - position);
        // The master: what the decks decode
        master.assign(static_cast<std::size_t>(2 * count), 0.0f);
        const mixxx::IndexRange request =
                mixxx::IndexRange::forward(range.start() + position, count);
        const mixxx::ReadableSampleFrames readable = pMaster->readSampleFrames(
                mixxx::WritableSampleFrames(request, mixxx::SampleBuffer::WritableSlice(buffer)));
        const mixxx::IndexRange read = intersect(readable.frameIndexRange(), request);
        if (!read.empty()) {
            std::copy_n(readable.readableData(
                                2 * (read.start() - readable.frameIndexRange().start())),
                    2 * read.length(),
                    master.begin() + 2 * (read.start() - request.start()));
        }
        if (!sidecar.read(count, &stems, pError)) {
            pOutput.reset();
            cleanUp();
            return false;
        }
        stems.resize(static_cast<std::size_t>(StemSidecar::kChannels * count), 0.0f);
        for (int i = 0; i < kStreams; ++i) {
            av_frame_unref(pFrame.get());
            pFrame->nb_samples = static_cast<int>(count);
            pFrame->format = AV_SAMPLE_FMT_FLTP;
            pFrame->sample_rate = sampleRate;
            av_channel_layout_copy(&pFrame->ch_layout, &codecs[0]->ch_layout);
            pFrame->pts = position;
            if (av_frame_get_buffer(pFrame.get(), 0) < 0) {
                pOutput.reset();
                cleanUp();
                return fail(pError, QStringLiteral("Out of memory"));
            }
            for (int channel = 0; channel < 2; ++channel) {
                auto* pOut = reinterpret_cast<float*>(pFrame->data[channel]);
                for (SINT frame = 0; frame < count; ++frame) {
                    pOut[frame] = i == 0
                            ? master[static_cast<std::size_t>(2 * frame + channel)]
                            : stems[static_cast<std::size_t>(StemSidecar::kChannels * frame +
                                      2 * (i - 1) + channel)];
                }
            }
            if (!encode(pOutput.get(),
                        codecs[static_cast<std::size_t>(i)].get(),
                        streams[static_cast<std::size_t>(i)],
                        pFrame.get(),
                        pPacket.get())) {
                pOutput.reset();
                cleanUp();
                return fail(pError, QStringLiteral("The AAC encoder failed"));
            }
        }
        position += count;
    }
    for (int i = 0; i < kStreams; ++i) {
        if (!encode(pOutput.get(),
                    codecs[static_cast<std::size_t>(i)].get(),
                    streams[static_cast<std::size_t>(i)],
                    nullptr,
                    pPacket.get())) {
            pOutput.reset();
            cleanUp();
            return fail(pError, QStringLiteral("The AAC encoder failed"));
        }
    }
    if (av_write_trailer(pOutput.get()) < 0) {
        pOutput.reset();
        cleanUp();
        return fail(pError, QStringLiteral("Cannot finish %1").arg(partPath));
    }
    pOutput.reset();
    pMaster->close();
    if (!insertStemBox(partPath, niStemManifest(stemColors()), pError)) {
        QFile::remove(partPath);
        return false;
    }
    QFile::remove(outPath);
    if (!QFile::rename(partPath, outPath)) {
        QFile::remove(partPath);
        return fail(pError, QStringLiteral("Cannot move the stem file to %1").arg(outPath));
    }
    return true;
#endif
}

} // namespace crate
