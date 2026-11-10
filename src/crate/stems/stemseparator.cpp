#include "crate/stems/stemseparator.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThreadPool>
#include <QtConcurrentMap>
#include <algorithm>
#include <numeric>
#include <utility>

#include "crate/stems/resampler.h"
#include "crate/stems/stemdsp.h"
#include "moc_stemseparator.cpp"
#include "sources/audiosourcestereoproxy.h"
#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "util/logger.h"
#include "util/samplebuffer.h"
#include "util/workerthread.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::StemSeparator");

constexpr int kStemRows = stemdsp::kSources * stemdsp::kChannels;
constexpr SINT kSegmentSamples = stemdsp::kChannels * stemdsp::kSegmentFrames;
constexpr SINT kSpectrumValues =
        2 * stemdsp::kChannels * stemdsp::kBins * stemdsp::kSegmentSpectrumFrames;

/// The htdemucs-onnx-1 export check measured 630 MiB of peak VRAM on CUDA
/// with the runtime's session options; its manifest asks for 1.5 GiB free.
constexpr qint64 kHtdemucsVideoMemoryBytes = qint64(1536) * 1024 * 1024;

const QString kMixInput = QStringLiteral("mix");
const QString kSpecInput = QStringLiteral("spec");
const QString kSpecOutput = QStringLiteral("spec_out");
const QString kWaveOutput = QStringLiteral("wave_out");

/// 1.5 s at 44.1 kHz: between two checks for cancellation.
constexpr SINT kDecodeChunkFrames = 65536;
/// The share of decoding in the progress.
constexpr double kDecodeShare = 0.1;
constexpr qint64 kProgressIntervalMs = 100;
/// Threads of the DSP around the graph. With four, the eight iSTFTs of a
/// segment take two rounds.
constexpr int kMaxDspThreads = 4;

bool fail(QString* pError, const QString& error) {
    if (pError) {
        *pError = error;
    }
    return false;
}

/// The exported graph on an ONNX Runtime session.
class HtdemucsModel : public StemModel {
  public:
    explicit HtdemucsModel(std::unique_ptr<InferenceSession> pSession)
            : m_pSession(std::move(pSession)) {
    }

    bool run(std::span<const float> mix,
            std::span<const float> spec,
            std::vector<float>* pSpecOut,
            std::vector<float>* pWaveOut,
            QString* pError) override {
        std::vector<InferenceOutput> outputs;
        if (!m_pSession->run({{kMixInput, {1, stemdsp::kChannels, stemdsp::kSegmentFrames}, mix},
                                     {kSpecInput,
                                             {1,
                                                     2 * stemdsp::kChannels,
                                                     stemdsp::kBins,
                                                     stemdsp::kSegmentSpectrumFrames},
                                             spec}},
                    &outputs,
                    pError)) {
            return false;
        }
        bool haveSpec = false;
        bool haveWave = false;
        for (InferenceOutput& output : outputs) {
            if (output.name == kSpecOutput &&
                    output.data.size() == size_t(stemdsp::kSources) * spec.size()) {
                *pSpecOut = std::move(output.data);
                haveSpec = true;
            } else if (output.name == kWaveOutput &&
                    output.data.size() == size_t(stemdsp::kSources) * mix.size()) {
                *pWaveOut = std::move(output.data);
                haveWave = true;
            }
        }
        if (!haveSpec || !haveWave) {
            return fail(pError, QStringLiteral("The graph's outputs are not those of htdemucs"));
        }
        return true;
    }

    void abort() override {
        m_pSession->terminate();
    }

    void clearAbort() override {
        m_pSession->clearTermination();
    }

    QString description() const override {
        const QString provider = inferenceProviderName(m_pSession->provider());
        if (m_pSession->fallbackReason().isEmpty()) {
            return provider;
        }
        return QStringLiteral("%1 (%2)").arg(provider, m_pSession->fallbackReason());
    }

  private:
    const std::unique_ptr<InferenceSession> m_pSession;
};

} // namespace

std::unique_ptr<StemModel> createHtdemucsModel(const InferenceRuntime& runtime,
        const QString& modelPath,
        InferenceProvider preferred,
        QString* pError) {
    std::unique_ptr<InferenceSession> pSession =
            runtime.createSession(modelPath, preferred, kHtdemucsVideoMemoryBytes, pError);
    if (!pSession) {
        return nullptr;
    }
    const QStringList inputs = pSession->inputNames();
    const QStringList outputs = pSession->outputNames();
    if (inputs.size() != 2 || !inputs.contains(kMixInput) || !inputs.contains(kSpecInput) ||
            !outputs.contains(kSpecOutput) || !outputs.contains(kWaveOutput)) {
        fail(pError,
                QStringLiteral("%1 is not an htdemucs graph: inputs %2, outputs %3")
                        .arg(modelPath,
                                inputs.join(QStringLiteral(", ")),
                                outputs.join(QStringLiteral(", "))));
        return nullptr;
    }
    if (!pSession->fallbackReason().isEmpty()) {
        kLogger.info() << "htdemucs runs on the CPU:" << pSession->fallbackReason();
    }
    return std::make_unique<HtdemucsModel>(std::move(pSession));
}

bool StemBuffer::begin(const StemSeparationSource& source, QString* pError) {
    Q_UNUSED(pError);
    m_source = source;
    m_frames = 0;
    m_stems.assign(size_t(kStemRows) * source.frames, 0.0f);
    return true;
}

bool StemBuffer::write(SINT firstFrame,
        SINT frameCount,
        std::span<const float> stems,
        QString* pError) {
    if (firstFrame != m_frames || firstFrame + frameCount > m_source.frames ||
            stems.size() < size_t(kStemRows * frameCount)) {
        return fail(pError, QStringLiteral("Stems out of order"));
    }
    for (int row = 0; row < kStemRows; ++row) {
        std::copy_n(stems.data() + row * frameCount,
                frameCount,
                m_stems.data() + row * m_source.frames + firstFrame);
    }
    m_frames += frameCount;
    return true;
}

std::span<const float> StemBuffer::channel(int source, int channel) const {
    VERIFY_OR_DEBUG_ASSERT(source >= 0 && source < stemdsp::kSources && channel >= 0 &&
            channel < stemdsp::kChannels) {
        return {};
    }
    return std::span<const float>(m_stems).subspan(
            (source * stemdsp::kChannels + channel) * m_source.frames, m_frames);
}

bool separateStems(std::span<float> input,
        SINT frames,
        StemModel* pModel,
        StemSink* pSink,
        const std::function<bool(double)>& progress,
        QString* pError,
        qint64* pGraphMs) {
    if (frames < 2 || input.size() < size_t(stemdsp::kChannels * frames)) {
        return fail(pError, QStringLiteral("Too short to separate"));
    }
    const stemdsp::Normalisation normalisation =
            stemdsp::normalisation(input, stemdsp::kChannels, frames);
    for (float& sample : input.first(stemdsp::kChannels * frames)) {
        sample = float((sample - normalisation.mean) / normalisation.scale);
    }

    // The transforms of a segment are independent: the STFT of each channel
    // and the iSTFT of each channel of each source. They run in parallel on
    // a few low-priority threads, each with its own Stft.
    QThreadPool pool;
    pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 1, kMaxDspThreads));
    pool.setThreadPriority(QThread::LowPriority);
    std::vector<std::unique_ptr<stemdsp::Stft>> stfts;
    for (int lane = 0; lane < pool.maxThreadCount(); ++lane) {
        stfts.push_back(std::make_unique<stemdsp::Stft>());
    }
    const auto inParallel = [&](int jobs, const std::function<void(stemdsp::Stft*, int)>& job) {
        std::vector<int> lanes(std::min(jobs, int(stfts.size())));
        std::iota(lanes.begin(), lanes.end(), 0);
        QtConcurrent::blockingMap(&pool, lanes, [&](const int& lane) {
            for (int i = lane; i < jobs; i += int(lanes.size())) {
                job(stfts[lane].get(), i);
            }
        });
    };
    constexpr SINT kChannelSpectrum = kSpectrumValues / stemdsp::kChannels;

    std::vector<float> spec(kSpectrumValues);
    std::vector<float> specOut;
    std::vector<float> waveOut;
    const SINT segments = (frames + stemdsp::kStrideFrames - 1) / stemdsp::kStrideFrames;
    SINT segmentsDone = 0;
    qint64 graphMs = 0;
    bool stopped = false;
    const auto model = [&](std::span<const float> mix, std::span<float> output) {
        inParallel(stemdsp::kChannels, [&](stemdsp::Stft* pStft, int channel) {
            pStft->forward(mix.subspan(channel * stemdsp::kSegmentFrames, stemdsp::kSegmentFrames),
                    1,
                    stemdsp::kSegmentFrames,
                    std::span<float>(spec).subspan(channel * kChannelSpectrum, kChannelSpectrum));
        });
        QElapsedTimer timer;
        timer.start();
        const bool ran = pModel->run(mix, spec, &specOut, &waveOut, pError);
        graphMs += timer.elapsed();
        if (!ran) {
            return false;
        }
        if (specOut.size() != size_t(stemdsp::kSources * kSpectrumValues) ||
                waveOut.size() != size_t(stemdsp::kSources * kSegmentSamples)) {
            return fail(pError, QStringLiteral("The model returned outputs of the wrong size"));
        }
        // Each stem is the iSTFT of the masked spectrum plus the time branch.
        inParallel(kStemRows, [&](stemdsp::Stft* pStft, int row) {
            const std::span<float> stem =
                    output.subspan(row * stemdsp::kSegmentFrames, stemdsp::kSegmentFrames);
            pStft->inverse(std::span<const float>(specOut).subspan(
                                   row * kChannelSpectrum, kChannelSpectrum),
                    1,
                    stemdsp::kSegmentSpectrumFrames,
                    stemdsp::kSegmentFrames,
                    stem);
            const float* const pWave = waveOut.data() + row * stemdsp::kSegmentFrames;
            for (SINT i = 0; i < stemdsp::kSegmentFrames; ++i) {
                stem[i] += pWave[i];
            }
        });
        ++segmentsDone;
        if (progress && !progress(double(segmentsDone) / double(segments))) {
            stopped = true;
            return fail(pError, QStringLiteral("Canceled"));
        }
        return true;
    };
    const auto write = [&](SINT firstFrame, SINT frameCount, std::span<float> stems) {
        for (float& sample : stems) {
            sample = float(sample * normalisation.scale + normalisation.mean);
        }
        return pSink->write(firstFrame, frameCount, stems, pError);
    };

    stemdsp::OverlapAddShape shape;
    const bool done = stemdsp::overlapAdd(input, frames, shape, model, write);
    if (pGraphMs) {
        *pGraphMs = graphMs;
    }
    DEBUG_ASSERT(done || stopped || !pError || !pError->isEmpty());
    return done;
}

class StemSeparator::Worker : public WorkerThread {
  public:
    explicit Worker(StemSeparator* pSeparator)
            : WorkerThread(QStringLiteral("StemSeparator"), QThread::LowPriority),
              m_pSeparator(pSeparator) {
    }

  protected:
    void doRun() override {
        while (awaitWorkItemsFetched()) {
            m_pSeparator->runJob();
        }
        // The model goes with the thread that used it.
        m_pSeparator->setModel(nullptr);
    }

    TryFetchWorkItemsResult tryFetchWorkItems() override {
        return m_pSeparator->fetchJob() ? TryFetchWorkItemsResult::Ready
                                        : TryFetchWorkItemsResult::Idle;
    }

  private:
    StemSeparator* const m_pSeparator;
};

StemSeparator::StemSeparator(ModelFactory modelFactory, QObject* parent)
        : QObject(parent),
          m_modelFactory(std::move(modelFactory)),
          m_pWorker(std::make_unique<Worker>(this)) {
    qRegisterMetaType<StemSeparationResult>();
    m_pWorker->start();
}

StemSeparator::~StemSeparator() {
    m_destroying = true;
    {
        const QMutexLocker locker(&m_mutex);
        m_queue.clear();
        m_canceled = true;
        if (m_pModel) {
            m_pModel->abort();
        }
    }
    m_pWorker->stop();
    m_pWorker->wait();
}

StemSeparator::JobId StemSeparator::separate(TrackPointer pTrack, std::shared_ptr<StemSink> pSink) {
    JobId id = 0;
    {
        const QMutexLocker locker(&m_mutex);
        id = m_nextId++;
        m_queue.push_back(Job{id, std::move(pTrack), std::move(pSink)});
    }
    m_pWorker->wake();
    return id;
}

void StemSeparator::cancel(JobId id) {
    const QMutexLocker locker(&m_mutex);
    if (id == m_runningId) {
        m_canceled = true;
        if (m_pModel) {
            m_pModel->abort();
        }
        return;
    }
    const auto it = std::find_if(
            m_queue.begin(), m_queue.end(), [id](const Job& job) { return job.id == id; });
    if (it == m_queue.end()) {
        return;
    }
    m_queue.erase(it);
    StemSeparationResult result;
    result.status = StemSeparationResult::Status::Canceled;
    // Later, so that no slot runs inside cancel().
    QMetaObject::invokeMethod(
            this,
            [this, id, result] {
                emit finished(id, result);
            },
            Qt::QueuedConnection);
}

bool StemSeparator::fetchJob() {
    const QMutexLocker locker(&m_mutex);
    if (m_queue.empty()) {
        return false;
    }
    m_job = std::move(m_queue.front());
    m_queue.pop_front();
    m_runningId = m_job.id;
    m_canceled = false;
    if (m_pModel) {
        m_pModel->clearAbort();
    }
    return true;
}

void StemSeparator::setModel(std::unique_ptr<StemModel> pModel) {
    std::unique_ptr<StemModel> pOldModel;
    {
        const QMutexLocker locker(&m_mutex);
        pOldModel = std::exchange(m_pModel, std::move(pModel));
        if (m_pModel && m_canceled) {
            m_pModel->abort();
        }
    }
}

void StemSeparator::emitProgress(JobId id, double fraction, bool always) {
    const qint64 now = m_progressTimer.elapsed();
    if (!always && now - m_lastProgressMs < kProgressIntervalMs && fraction < 1.0) {
        return;
    }
    m_lastProgressMs = now;
    emit progress(id, fraction);
}

void StemSeparator::runJob() {
    const Job job = std::exchange(m_job, Job());
    m_progressTimer.start();
    m_lastProgressMs = 0;
    StemSeparationResult result = separateJob(job);
    {
        const QMutexLocker locker(&m_mutex);
        m_runningId = 0;
        if (result.status == StemSeparationResult::Status::Failed && m_canceled) {
            result.status = StemSeparationResult::Status::Canceled;
            result.error.clear();
        }
    }
    const QString location = job.pTrack ? job.pTrack->getLocation() : QString();
    switch (result.status) {
    case StemSeparationResult::Status::Done:
        kLogger.info() << "Separated" << location << "on" << result.model << "in"
                       << (result.modelMs + result.decodeMs + result.dspMs + result.graphMs)
                       << "ms: model" << result.modelMs << "ms, decoding" << result.decodeMs
                       << "ms, DSP" << result.dspMs << "ms, graph" << result.graphMs << "ms";
        break;
    case StemSeparationResult::Status::Canceled:
        kLogger.info() << "Canceled the separation of" << location;
        break;
    case StemSeparationResult::Status::Failed:
        kLogger.warning() << "Cannot separate" << location << ":" << result.error;
        break;
    }
    if (!m_destroying) {
        emit finished(job.id, result);
    }
}

StemSeparationResult StemSeparator::separateJob(const Job& job) {
    StemSeparationResult result;
    const auto failed = [&result](const QString& error) {
        result.status = StemSeparationResult::Status::Failed;
        result.error = error;
        return result;
    };
    if (!job.pTrack || !job.pSink) {
        return failed(QStringLiteral("No track or no sink"));
    }
    QElapsedTimer timer;
    timer.start();
    QString error;
    if (!m_pModel) {
        std::unique_ptr<StemModel> pModel = m_modelFactory(&error);
        if (!pModel) {
            return failed(QStringLiteral("Cannot load the separation model: %1").arg(error));
        }
        setModel(std::move(pModel));
        result.modelMs = timer.restart();
    }
    result.model = m_pModel->description();

    std::vector<float> input;
    if (!decode(job, &input, &result.source, &error)) {
        return failed(error);
    }
    result.decodeMs = timer.restart();
    if (isCanceled()) {
        return failed(QStringLiteral("Canceled"));
    }
    emitProgress(job.id, kDecodeShare, true);

    if (!job.pSink->begin(result.source, &error)) {
        return failed(error);
    }
    qint64 graphMs = 0;
    const bool done = separateStems(
            input,
            result.source.frames,
            m_pModel.get(),
            job.pSink.get(),
            [this, &job](double fraction) {
                emitProgress(job.id, kDecodeShare + (1.0 - kDecodeShare) * fraction);
                return !isCanceled();
            },
            &error,
            &graphMs);
    result.graphMs = graphMs;
    result.dspMs = timer.elapsed() - graphMs;
    if (!done) {
        return failed(error);
    }
    result.status = StemSeparationResult::Status::Done;
    return result;
}

bool StemSeparator::decode(const Job& job,
        std::vector<float>* pInput,
        StemSeparationSource* pSource,
        QString* pError) {
    mixxx::AudioSource::OpenParams openParams;
    openParams.setChannelCount(mixxx::audio::ChannelCount::stereo());
    mixxx::AudioSourcePointer pAudioSource =
            SoundSourceProxy(job.pTrack).openAudioSource(openParams);
    if (!pAudioSource) {
        return fail(pError, QStringLiteral("Cannot decode %1").arg(job.pTrack->getLocation()));
    }
    const mixxx::audio::SignalInfo signalInfo = pAudioSource->getSignalInfo();
    pSource->originalSampleRate = int(signalInfo.getSampleRate());
    pSource->originalChannelCount = int(signalInfo.getChannelCount());
    if (signalInfo.getChannelCount() != mixxx::audio::ChannelCount::stereo()) {
        // The first two channels, or mono twice, like Demucs.
        pAudioSource = std::make_shared<mixxx::AudioSourceStereoProxy>(
                pAudioSource, kDecodeChunkFrames);
    }
    Resampler resampler(pSource->originalSampleRate, stemdsp::kSampleRate, stemdsp::kChannels);
    const mixxx::IndexRange range = pAudioSource->frameIndexRange();
    // Planar: the left channel from 0 and the right one from `capacity`.
    // The source can turn out shorter than it says, never longer.
    const SINT capacity = resampler.outputFrames(range.length());
    pInput->assign(stemdsp::kChannels * capacity, 0.0f);
    SINT frames = 0;
    mixxx::SampleBuffer buffer(stemdsp::kChannels * kDecodeChunkFrames);
    std::vector<float> chunk;
    std::vector<float> resampled;
    const auto store = [&] {
        const SINT count = std::min(SINT(resampled.size()) / stemdsp::kChannels, capacity - frames);
        for (SINT i = 0; i < count; ++i) {
            (*pInput)[frames + i] = resampled[2 * i];
            (*pInput)[capacity + frames + i] = resampled[2 * i + 1];
        }
        frames += count;
        resampled.clear();
    };
    SINT next = range.start();
    SINT end = range.end();
    while (next < end) {
        if (isCanceled() || m_pWorker->isStopping()) {
            return fail(pError, QStringLiteral("Canceled"));
        }
        const mixxx::IndexRange request =
                mixxx::IndexRange::forward(next, std::min(kDecodeChunkFrames, end - next));
        const mixxx::ReadableSampleFrames readable =
                pAudioSource->readSampleFrames(mixxx::WritableSampleFrames(
                        request, mixxx::SampleBuffer::WritableSlice(buffer)));
        end = std::min(end, pAudioSource->frameIndexRange().end());
        const mixxx::IndexRange decoded = intersect(request, mixxx::IndexRange::between(next, end));
        // Frames that do not decode stay silent, so that the stems keep the
        // frame positions of playback.
        // TEST_GAP: no test file has frames that fail to decode.
        chunk.assign(stemdsp::kChannels * decoded.length(), 0.0f);
        const mixxx::IndexRange read = intersect(readable.frameIndexRange(), decoded);
        if (!read.empty()) {
            std::copy_n(readable.readableData(stemdsp::kChannels *
                                (read.start() - readable.frameIndexRange().start())),
                    stemdsp::kChannels * read.length(),
                    chunk.begin() + stemdsp::kChannels * (read.start() - next));
        }
        resampler.process(chunk, &resampled);
        store();
        next += decoded.length();
        emitProgress(job.id, kDecodeShare * double(next - range.start()) / double(range.length()));
        if (decoded.empty()) {
            break;
        }
    }
    resampler.finish(&resampled);
    store();
    pAudioSource->close();
    pSource->originalFrames = next - range.start();
    pSource->frames = frames;
    if (frames < capacity) {
        std::copy_n(pInput->begin() + capacity, frames, pInput->begin() + frames);
        pInput->resize(stemdsp::kChannels * frames);
    }
    if (frames < 2) {
        return fail(pError, QStringLiteral("No audio in %1").arg(job.pTrack->getLocation()));
    }
    return true;
}

} // namespace crate
