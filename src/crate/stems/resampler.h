#pragma once

#include <span>
#include <vector>

#include "util/types.h"

namespace crate {

/// Converts the sample rate of interleaved float audio with a windowed sinc:
/// the algorithm of julius.resample_frac, which Demucs uses to bring audio to
/// 44.1 kHz. 24 zero crossings, a roll-off of 0.945 of the lower Nyquist
/// frequency, a Hann window, and the first and last samples repeated past
/// the ends.
///
/// It streams: process() takes the input in blocks of any size and returns
/// every output sample whose input has arrived; finish() returns the rest.
/// In total there are floor(outputRate × input frames / inputRate) output
/// frames, the same as julius. With equal rates the audio passes unchanged.
class Resampler {
  public:
    Resampler(int inputRate, int outputRate, int channels);

    int inputRate() const {
        return m_inputRate;
    }
    int outputRate() const {
        return m_outputRate;
    }

    /// The output frames for `inputFrames` input frames.
    SINT outputFrames(SINT inputFrames) const;

    /// Appends the output of the next `input` frames to `pOutput`.
    void process(std::span<const float> input, std::vector<float>* pOutput);
    /// Appends the rest of the output after the last input. The object
    /// takes no more input afterwards.
    void finish(std::vector<float>* pOutput);

  private:
    /// Appends output frames while their input is there; all of them up to
    /// the total when `last`.
    void produce(std::vector<float>* pOutput, bool last);

    const int m_inputRate;
    const int m_outputRate;
    const int m_channels;
    /// The rates divided by their greatest common divisor.
    int m_inputStep = 1;
    int m_outputStep = 1;
    /// Per output phase: the first tap, relative to the phase's input block,
    /// and the taps, normalised to sum 1.
    std::vector<int> m_firstTap;
    std::vector<std::vector<float>> m_taps;
    int m_lowestTap = 0;

    /// Interleaved input from frame m_bufferStart on.
    std::vector<float> m_buffer;
    SINT m_bufferStart = 0;
    SINT m_inputFrames = 0;
    /// The first frame, repeated before the start.
    std::vector<float> m_firstFrame;
    SINT m_nextOutput = 0;
    bool m_finished = false;
};

} // namespace crate
