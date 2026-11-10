#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "util/types.h"

class FFTReal;

/// The DSP around the exported htdemucs graph (STEM-D7, spike report §5).
///
/// The graph holds only the network. This is the rest of Demucs'
/// `apply_model` and `HTDemucs.forward`, ported from the numpy reference
/// tools/crate-stems/stems_dsp.py and checked against its fixtures in
/// src/test/crate/data/htdemucs-dsp.
///
/// Audio is planar float: `channels` rows of `length` samples each.
namespace crate::stemdsp {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
/// drums, bass, other and vocals, in the order of the graph outputs.
constexpr int kSources = 4;
constexpr int kFftSize = 4096;
constexpr int kHop = kFftSize / 4;
/// The Nyquist bin is dropped.
constexpr int kBins = kFftSize / 2;
/// Reflect padding on each side before the centred STFT.
constexpr int kPrePad = kHop / 2 * 3;
/// The segment the graph takes: 7.8 s at 44.1 kHz.
constexpr SINT kSegmentFrames = 343980;
/// Segments overlap by a quarter.
constexpr SINT kStrideFrames = 257985;

/// Frames of the spectrum of `length` samples: ceil(length / hop).
constexpr int spectrumFrames(SINT length) {
    return static_cast<int>((length + kHop - 1) / kHop);
}

/// Spectrum frames of one segment: 336.
constexpr int kSegmentSpectrumFrames = spectrumFrames(kSegmentFrames);

/// The STFT and iSTFT of HTDemucs (`_spec`, `_ispec`): n_fft 4096, hop 1024,
/// periodic Hann window, normalised (forward ÷ 64, inverse × 64), centred,
/// reflect padding, the Nyquist bin and two frames on each side dropped.
///
/// Spectra are complex as channels, the layout of the graph's `spec` input
/// and of each stem of its `spec_out` output: rows [L.re, L.im, R.re, R.im],
/// each of kBins × frames values, bin-major.
///
/// Computes in double precision. Keeps buffers between calls, so one object
/// serves one thread.
class Stft {
  public:
    Stft();
    ~Stft();
    Stft(const Stft&) = delete;
    Stft& operator=(const Stft&) = delete;

    /// `input`: `channels` rows of `length` samples, `length` > 2559 so that
    /// the reflect padding fits. `output`: 2 × `channels` rows of
    /// kBins × spectrumFrames(length) values.
    void forward(std::span<const float> input,
            int channels,
            SINT length,
            std::span<float> output);

    /// `input`: 2 × `channels` rows of kBins × `frames` values, with
    /// `length` <= frames × hop + 3584. `output`: `channels` rows of `length`
    /// samples.
    void inverse(std::span<const float> input,
            int channels,
            int frames,
            SINT length,
            std::span<float> output);

  private:
    std::unique_ptr<FFTReal> m_pFft;
    std::vector<double> m_window;
    std::vector<double> m_frame;
    std::vector<double> m_real;
    std::vector<double> m_imaginary;
    std::vector<double> m_padded;
    std::vector<double> m_sum;
};

/// The mean and scale that the whole track is normalised with
/// (demucs.api.Separator): the mean and the unbiased standard deviation, plus
/// 1e-8, of its mono mix.
struct Normalisation {
    double mean = 0.0;
    double scale = 1.0;
};

/// `input`: `channels` rows of `length` samples, `length` >= 2.
Normalisation normalisation(std::span<const float> input, int channels, SINT length);

/// The triangular overlap-add weight of one segment
/// (demucs.apply.apply_model): [1, 2, ..., segment / 2] followed by
/// [segment - segment / 2, ..., 1], divided by its maximum.
std::vector<float> overlapAddWeight(SINT segment);

struct OverlapAddShape {
    int channels = kChannels;
    int sources = kSources;
    SINT segment = kSegmentFrames;
    SINT stride = kStrideFrames;
};

/// Runs the model on one segment. `segment`: `channels` rows of `segment`
/// samples. `output`: `sources` × `channels` rows of `segment` samples.
/// Returns false to stop the overlap-add.
using OverlapAddModel = std::function<bool(
        std::span<const float> segment, std::span<float> output)>;

/// Receives final output: the frames [firstFrame, firstFrame + frameCount)
/// as `sources` × `channels` rows of `frameCount` samples, in order and
/// without gaps. It may change them in place. Returns false to stop the
/// overlap-add.
using OverlapAddOutput = std::function<bool(
        SINT firstFrame, SINT frameCount, std::span<float> output)>;

/// demucs.apply.apply_model with split=True and shifts=0: cuts `input`
/// (`channels` rows of `length` samples) at every `stride` samples, centres
/// each cut in `segment` samples with its real neighbouring samples or zeros
/// past the ends, runs the model on it, trims the result back to the cut and
/// overlap-adds it with the triangular weight.
///
/// Output is passed on as soon as it is final, one stride at a time, so only
/// one segment is kept. Returns false if `model` or `output` stopped it.
bool overlapAdd(std::span<const float> input,
        SINT length,
        const OverlapAddShape& shape,
        const OverlapAddModel& model,
        const OverlapAddOutput& output);

} // namespace crate::stemdsp
