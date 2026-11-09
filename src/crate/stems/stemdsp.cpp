#include "crate/stems/stemdsp.h"

#include <dsp/transforms/FFT.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "util/assert.h"

namespace crate::stemdsp {

namespace {

/// `normalized=True`: the forward transform is divided by sqrt(n_fft) and
/// the inverse multiplied by it.
constexpr double kNormalisation = 64.0;
static_assert(kNormalisation * kNormalisation == kFftSize);
/// Frames dropped on each side of the spectrum, and restored as zeros.
constexpr int kEdgeFrames = 2;
/// The output sample 0 of the inverse, in its overlap-add buffer: the centre
/// padding plus the pre-padding.
constexpr SINT kInverseStart = kFftSize / 2 + kPrePad;
constexpr double kNormalisationEpsilon = 1e-8;

/// numpy's "reflect" padding: the sample at `index` of a signal of `length`
/// samples mirrored at its ends without repeating them. Valid for
/// -length < index < 2 × length - 1.
SINT reflect(SINT index, SINT length) {
    if (index < 0) {
        return -index;
    }
    if (index >= length) {
        return 2 * (length - 1) - index;
    }
    return index;
}

} // namespace

Stft::Stft()
        : m_pFft(std::make_unique<FFTReal>(kFftSize)),
          m_window(kFftSize),
          m_frame(kFftSize),
          m_real(kFftSize),
          m_imaginary(kFftSize) {
    // torch.hann_window(4096) is periodic.
    for (int n = 0; n < kFftSize; ++n) {
        m_window[n] = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * n / kFftSize);
    }
}

Stft::~Stft() = default;

void Stft::forward(std::span<const float> input,
        int channels,
        SINT length,
        std::span<float> output) {
    const int frames = spectrumFrames(length);
    const SINT bins = SINT(kBins) * frames;
    VERIFY_OR_DEBUG_ASSERT(length > kPrePad + kHop &&
            input.size() >= size_t(channels * length) &&
            output.size() >= size_t(2 * channels * bins)) {
        return;
    }
    // Reflect-padded by kPrePad on the left and up to whole hops plus
    // kPrePad on the right. The centred STFT pads by n_fft / 2 more, but only
    // its first and last two frames, which are dropped, read that padding:
    // kept frame f starts at sample f × hop of this buffer.
    const SINT paddedLength = SINT(frames) * kHop + 2 * kPrePad;
    m_padded.resize(paddedLength);
    for (int channel = 0; channel < channels; ++channel) {
        const float* const pInput = input.data() + channel * length;
        for (SINT i = 0; i < paddedLength; ++i) {
            m_padded[i] = pInput[reflect(i - kPrePad, length)];
        }
        float* const pReal = output.data() + 2 * channel * bins;
        float* const pImaginary = pReal + bins;
        for (int frame = 0; frame < frames; ++frame) {
            const double* const pFrame = m_padded.data() + SINT(frame) * kHop;
            for (int n = 0; n < kFftSize; ++n) {
                m_frame[n] = pFrame[n] * m_window[n];
            }
            m_pFft->forward(m_frame.data(), m_real.data(), m_imaginary.data());
            for (int bin = 0; bin < kBins; ++bin) {
                pReal[SINT(bin) * frames + frame] = float(m_real[bin] / kNormalisation);
                pImaginary[SINT(bin) * frames + frame] =
                        float(m_imaginary[bin] / kNormalisation);
            }
        }
    }
}

void Stft::inverse(std::span<const float> input,
        int channels,
        int frames,
        SINT length,
        std::span<float> output) {
    const SINT bins = SINT(kBins) * frames;
    // The overlap-add buffer of the centred inverse, with the two zero
    // frames restored on each side: frame t (0 <= t < frames + 4) adds to
    // samples [t × hop, t × hop + n_fft).
    const SINT bufferLength = kFftSize + SINT(kHop) * (frames + 2 * kEdgeFrames - 1);
    VERIFY_OR_DEBUG_ASSERT(frames > 0 && kInverseStart + length <= bufferLength &&
            input.size() >= size_t(2 * channels * bins) &&
            output.size() >= size_t(channels * length)) {
        return;
    }
    // The summed squared window, including the zero frames.
    m_padded.assign(bufferLength, 0.0);
    for (int t = 0; t < frames + 2 * kEdgeFrames; ++t) {
        for (int n = 0; n < kFftSize; ++n) {
            m_padded[SINT(t) * kHop + n] += m_window[n] * m_window[n];
        }
    }
    m_sum.resize(bufferLength);
    // Bins up to Nyquist, which is zero.
    m_real[kBins] = 0.0;
    m_imaginary[kBins] = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
        std::fill(m_sum.begin(), m_sum.end(), 0.0);
        const float* const pReal = input.data() + 2 * channel * bins;
        const float* const pImaginary = pReal + bins;
        for (int frame = 0; frame < frames; ++frame) {
            for (int bin = 0; bin < kBins; ++bin) {
                m_real[bin] = pReal[SINT(bin) * frames + frame] * kNormalisation;
                m_imaginary[bin] = pImaginary[SINT(bin) * frames + frame] * kNormalisation;
            }
            // Scaled by 1 / n_fft, like numpy's irfft.
            m_pFft->inverse(m_real.data(), m_imaginary.data(), m_frame.data());
            double* const pSum = m_sum.data() + SINT(frame + kEdgeFrames) * kHop;
            for (int n = 0; n < kFftSize; ++n) {
                pSum[n] += m_frame[n] * m_window[n];
            }
        }
        float* const pOutput = output.data() + channel * length;
        for (SINT i = 0; i < length; ++i) {
            pOutput[i] = float(m_sum[kInverseStart + i] / m_padded[kInverseStart + i]);
        }
    }
}

Normalisation normalisation(std::span<const float> input, int channels, SINT length) {
    VERIFY_OR_DEBUG_ASSERT(length >= 2 && channels > 0 &&
            input.size() >= size_t(channels * length)) {
        return Normalisation();
    }
    const auto mono = [&](SINT i) {
        double sum = 0.0;
        for (int channel = 0; channel < channels; ++channel) {
            sum += input[channel * length + i];
        }
        return sum / channels;
    };
    double sum = 0.0;
    for (SINT i = 0; i < length; ++i) {
        sum += mono(i);
    }
    const double mean = sum / double(length);
    double squares = 0.0;
    for (SINT i = 0; i < length; ++i) {
        const double difference = mono(i) - mean;
        squares += difference * difference;
    }
    return Normalisation{mean, std::sqrt(squares / double(length - 1)) + kNormalisationEpsilon};
}

std::vector<float> overlapAddWeight(SINT segment) {
    std::vector<float> weight(segment);
    const SINT half = segment / 2;
    const double maximum = double(std::max(half, segment - half));
    for (SINT i = 0; i < segment; ++i) {
        weight[i] = float(double(i < half ? i + 1 : segment - i) / maximum);
    }
    return weight;
}

bool overlapAdd(std::span<const float> input,
        SINT length,
        const OverlapAddShape& shape,
        const OverlapAddModel& model,
        const OverlapAddOutput& output) {
    const int rows = shape.sources * shape.channels;
    const SINT segmentLength = shape.segment;
    VERIFY_OR_DEBUG_ASSERT(length > 0 && segmentLength > 0 && shape.stride > 0 &&
            shape.stride <= segmentLength &&
            input.size() >= size_t(shape.channels * length)) {
        return false;
    }
    const std::vector<float> weight = overlapAddWeight(segmentLength);
    std::vector<float> segment(shape.channels * segmentLength);
    std::vector<float> result(rows * segmentLength);
    // The weighted sums and summed weights of the frames
    // [offset, offset + segment), the furthest any segment so far reaches.
    std::vector<double> sum(rows * segmentLength, 0.0);
    std::vector<double> sumWeight(segmentLength, 0.0);
    std::vector<float> chunk;
    for (SINT offset = 0; offset < length; offset += shape.stride) {
        const SINT chunkLength = std::min(segmentLength, length - offset);
        // demucs TensorChunk.padded(): the chunk centred in the segment.
        const SINT trim = (segmentLength - chunkLength) / 2;
        const SINT start = offset - trim;
        for (int channel = 0; channel < shape.channels; ++channel) {
            const float* const pInput = input.data() + channel * length;
            float* const pSegment = segment.data() + channel * segmentLength;
            for (SINT n = 0; n < segmentLength; ++n) {
                const SINT i = start + n;
                pSegment[n] = (i >= 0 && i < length) ? pInput[i] : 0.0f;
            }
        }
        if (!model(segment, result)) {
            return false;
        }
        // demucs.utils.center_trim() back to the chunk, then weighted.
        for (int row = 0; row < rows; ++row) {
            const float* const pResult = result.data() + row * segmentLength + trim;
            double* const pSum = sum.data() + row * segmentLength;
            for (SINT n = 0; n < chunkLength; ++n) {
                pSum[n] += double(weight[n]) * pResult[n];
            }
        }
        for (SINT n = 0; n < chunkLength; ++n) {
            sumWeight[n] += weight[n];
        }
        // No later segment reaches before offset + stride.
        const SINT finalFrames = std::min(shape.stride, length - offset);
        chunk.resize(rows * finalFrames);
        for (int row = 0; row < rows; ++row) {
            const double* const pSum = sum.data() + row * segmentLength;
            float* const pChunk = chunk.data() + row * finalFrames;
            for (SINT n = 0; n < finalFrames; ++n) {
                pChunk[n] = float(pSum[n] / sumWeight[n]);
            }
        }
        if (!output(offset, finalFrames, chunk)) {
            return false;
        }
        for (int row = 0; row < rows; ++row) {
            const auto rowBegin = sum.begin() + row * segmentLength;
            std::move(rowBegin + shape.stride, rowBegin + segmentLength, rowBegin);
            std::fill(rowBegin + (segmentLength - shape.stride), rowBegin + segmentLength, 0.0);
        }
        std::move(sumWeight.begin() + shape.stride, sumWeight.end(), sumWeight.begin());
        std::fill(sumWeight.end() - shape.stride, sumWeight.end(), 0.0);
    }
    return true;
}

} // namespace crate::stemdsp
