#pragma once

#include <QElapsedTimer>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QString>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "crate/inference/inferenceruntime.h"
#include "track/track_decl.h"
#include "util/types.h"

namespace crate {

/// The htdemucs graph, run on one segment at a time (STEM-D7). The real one
/// runs the exported graph with ONNX Runtime (createHtdemucsModel()); tests
/// use mocks.
class StemModel {
  public:
    virtual ~StemModel() = default;

    /// Runs the graph on one segment, on the separator's worker thread:
    /// - `mix`: the graph input `mix`, 2 rows of stemdsp::kSegmentFrames
    ///   samples;
    /// - `spec`: the graph input `spec`, its STFT (stemdsp::Stft);
    /// - `pSpecOut`: per source, a spectrum in the layout of `spec`;
    /// - `pWaveOut`: per source, 2 rows of stemdsp::kSegmentFrames samples.
    ///
    /// Sources come in the order drums, bass, other, vocals. Returns false,
    /// with the reason in `pError`, when the run fails or was aborted.
    virtual bool run(std::span<const float> mix,
            std::span<const float> spec,
            std::vector<float>* pSpecOut,
            std::vector<float>* pWaveOut,
            QString* pError) = 0;

    /// Makes the run in progress, and every later one until clearAbort(),
    /// fail promptly. Thread-safe.
    virtual void abort() = 0;
    virtual void clearAbort() = 0;

    /// What runs the graph, for the log: the provider, and why it is not the
    /// preferred one.
    virtual QString description() const = 0;
};

/// Loads the htdemucs-onnx-1 graph on `preferred`, or on the CPU when that
/// provider cannot run it (InferenceRuntime::createSession()), and checks its
/// inputs and outputs. Blocks for seconds: call it on a worker thread.
std::unique_ptr<StemModel> createHtdemucsModel(const InferenceRuntime& runtime,
        const QString& modelPath,
        InferenceProvider preferred,
        QString* pError);

/// The audio a separation starts from.
struct StemSeparationSource {
    /// The original as Decks decodes it with SoundSourceProxy.
    int originalSampleRate = 0;
    int originalChannelCount = 0;
    SINT originalFrames = 0;
    /// Frames of the stems, at 44.1 kHz.
    SINT frames = 0;
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

/// A StemSink that keeps the stems in memory: 1.4 MB per second of audio.
class StemBuffer : public StemSink {
  public:
    bool begin(const StemSeparationSource& source, QString* pError) override;
    bool write(SINT firstFrame,
            SINT frameCount,
            std::span<const float> stems,
            QString* pError) override;

    const StemSeparationSource& source() const {
        return m_source;
    }
    /// The frames written so far.
    SINT frames() const {
        return m_frames;
    }
    /// One channel (0 left, 1 right) of a source (0 drums, 1 bass, 2 other,
    /// 3 vocals).
    std::span<const float> channel(int source, int channel) const;

  private:
    StemSeparationSource m_source;
    std::vector<float> m_stems;
    SINT m_frames = 0;
};

/// Separates `input`, 2 rows of `frames` samples at 44.1 kHz, with the DSP
/// around the graph (stemdsp): normalisation, segments, STFT, the model,
/// iSTFT, overlap-add and de-normalisation. Passes the stems to `pSink` as
/// they are final, without calling StemSink::begin(). Runs on the calling
/// thread; the transforms of each segment run on up to four low-priority
/// threads of its own.
///
/// `input` is normalised in place, so that a long track is not held twice.
/// `progress` (optional) gets the fraction of segments done, and stops the
/// separation by returning false. `pGraphMs` (optional) gets the time spent
/// in the model.
bool separateStems(std::span<float> input,
        SINT frames,
        StemModel* pModel,
        StemSink* pSink,
        const std::function<bool(double)>& progress,
        QString* pError,
        qint64* pGraphMs = nullptr);

/// How a separation ended.
struct StemSeparationResult {
    enum class Status {
        Done,
        Canceled,
        Failed,
    };
    Status status = Status::Failed;
    /// Why it failed.
    QString error;
    StemSeparationSource source;
    /// What ran the graph (StemModel::description()).
    QString model;
    /// Wall-clock times in milliseconds: loading the model (first job
    /// only), decoding and resampling, the DSP around the graph, and the
    /// graph.
    qint64 modelMs = 0;
    qint64 decodeMs = 0;
    qint64 dspMs = 0;
    qint64 graphMs = 0;
};

/// Separates tracks into four stems at 44.1 kHz on its own worker thread
/// (STEM-D7), one at a time and in order.
///
/// For each job it decodes the track's file with Decks' own
/// SoundSourceProxy, takes it to stereo (the first two channels, or mono
/// twice), resamples it to 44.1 kHz (crate::Resampler) and separates it
/// (separateStems()). The model is created on the worker thread with the
/// first job and kept for the next ones.
///
/// Lives on the main thread; its methods never block. Signals come from the
/// worker thread, so connections to the main thread are queued.
class StemSeparator : public QObject {
    Q_OBJECT
  public:
    using JobId = quint64;
    /// Creates the model, on the worker thread. Returns nullptr, with the
    /// reason in `pError`, if it cannot; the next job tries again.
    using ModelFactory = std::function<std::unique_ptr<StemModel>(QString* pError)>;

    explicit StemSeparator(ModelFactory modelFactory, QObject* parent = nullptr);
    /// Cancels every job, without finished(), and waits for the worker
    /// thread: at most one node of the graph or the DSP of one segment.
    ~StemSeparator() override;

    /// Queues the separation of the track's file. `pSink` gets the stems on
    /// the worker thread. finished() follows once for the job.
    JobId separate(TrackPointer pTrack, std::shared_ptr<StemSink> pSink);

    /// Cancels a queued or running job. A running job stops within a decoded
    /// chunk, one node of the graph or the DSP of one segment, and its sink
    /// gets no more stems.
    void cancel(JobId id);

  signals:
    /// From 0 to 1: decoding counts for a tenth, the segments for the rest.
    void progress(crate::StemSeparator::JobId id, double fraction);
    void finished(crate::StemSeparator::JobId id, const crate::StemSeparationResult& result);

  private:
    class Worker;
    struct Job {
        JobId id = 0;
        TrackPointer pTrack;
        std::shared_ptr<StemSink> pSink;
    };

    /// On the worker thread.
    bool fetchJob();
    void runJob();
    StemSeparationResult separateJob(const Job& job);
    bool isCanceled() const {
        return m_canceled.load();
    }
    /// Locks the model while it is replaced or aborted.
    void setModel(std::unique_ptr<StemModel> pModel);
    /// At most every 100 ms, unless `always`.
    void emitProgress(JobId id, double fraction, bool always = false);
    bool decode(const Job& job,
            std::vector<float>* pInput,
            StemSeparationSource* pSource,
            QString* pError);

    const ModelFactory m_modelFactory;
    std::unique_ptr<Worker> m_pWorker;

    /// Guards the queue, the running job and the model's abort.
    mutable QMutex m_mutex;
    std::deque<Job> m_queue;
    JobId m_nextId = 1;
    JobId m_runningId = 0;
    std::atomic<bool> m_canceled = false;
    /// Set by the destructor: no more finished().
    std::atomic<bool> m_destroying = false;
    /// Used by the worker thread; abort() from any thread under m_mutex.
    std::unique_ptr<StemModel> m_pModel;

    /// The worker thread's job and its progress.
    Job m_job;
    QElapsedTimer m_progressTimer;
    qint64 m_lastProgressMs = 0;
};

} // namespace crate

Q_DECLARE_METATYPE(crate::StemSeparationResult);
