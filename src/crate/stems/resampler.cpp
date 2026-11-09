#include "crate/stems/resampler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>

#include "util/assert.h"

namespace crate {

namespace {

// julius.resample_frac's defaults, which Demucs uses.
constexpr int kZeroCrossings = 24;
constexpr double kRollOff = 0.945;

double sinc(double x) {
    return x == 0.0 ? 1.0 : std::sin(x) / x;
}

} // namespace

Resampler::Resampler(int inputRate, int outputRate, int channels)
        : m_inputRate(inputRate),
          m_outputRate(outputRate),
          m_channels(channels) {
    VERIFY_OR_DEBUG_ASSERT(inputRate > 0 && outputRate > 0 && channels > 0) {
        return;
    }
    const int divisor = std::gcd(inputRate, outputRate);
    m_inputStep = inputRate / divisor;
    m_outputStep = outputRate / divisor;
    if (m_inputStep == m_outputStep) {
        return;
    }
    // julius.ResampleFrac._init_kernels(): output sample `phase` of every
    // block of m_outputStep samples reads the input block of m_inputStep
    // samples around it. Only the taps within the zero crossings are kept;
    // the rest are zero.
    const double cutoff = std::min(m_inputStep, m_outputStep) * kRollOff;
    const double halfWidth = kZeroCrossings * double(m_inputStep) / cutoff;
    const int width = int(std::ceil(halfWidth));
    m_firstTap.resize(m_outputStep);
    m_taps.resize(m_outputStep);
    for (int phase = 0; phase < m_outputStep; ++phase) {
        const double centre = double(phase) * m_inputStep / m_outputStep;
        const int first = std::max(-width, int(std::ceil(centre - halfWidth)));
        const int last = std::min(width + m_inputStep - 1, int(std::floor(centre + halfWidth)));
        std::vector<double> taps;
        double total = 0.0;
        for (int tap = first; tap <= last; ++tap) {
            double t = (double(tap) / m_inputStep - double(phase) / m_outputStep) * cutoff;
            t = std::clamp(t, -double(kZeroCrossings), double(kZeroCrossings)) *
                    std::numbers::pi;
            const double window = std::cos(t / kZeroCrossings / 2);
            const double value = sinc(t) * window * window;
            taps.push_back(value);
            total += value;
        }
        m_firstTap[phase] = first;
        // Normalised so that a constant signal stays constant.
        m_taps[phase].reserve(taps.size());
        for (const double value : taps) {
            m_taps[phase].push_back(float(value / total));
        }
    }
    m_lowestTap = *std::min_element(m_firstTap.begin(), m_firstTap.end());
}

SINT Resampler::outputFrames(SINT inputFrames) const {
    return SINT(m_outputStep) * inputFrames / m_inputStep;
}

void Resampler::process(std::span<const float> input, std::vector<float>* pOutput) {
    VERIFY_OR_DEBUG_ASSERT(!m_finished && input.size() % m_channels == 0) {
        return;
    }
    const SINT frames = SINT(input.size()) / m_channels;
    if (m_inputStep == m_outputStep) {
        pOutput->insert(pOutput->end(), input.begin(), input.end());
        m_inputFrames += frames;
        return;
    }
    if (frames == 0) {
        return;
    }
    if (m_inputFrames == 0) {
        m_firstFrame.assign(input.begin(), input.begin() + m_channels);
    }
    m_buffer.insert(m_buffer.end(), input.begin(), input.end());
    m_inputFrames += frames;
    produce(pOutput, false);
}

void Resampler::finish(std::vector<float>* pOutput) {
    if (m_finished) {
        return;
    }
    m_finished = true;
    if (m_inputStep != m_outputStep && m_inputFrames > 0) {
        produce(pOutput, true);
    }
    m_buffer.clear();
    m_buffer.shrink_to_fit();
}

void Resampler::produce(std::vector<float>* pOutput, bool last) {
    const SINT total = last ? outputFrames(m_inputFrames) : std::numeric_limits<SINT>::max();
    const SINT bufferEnd = m_bufferStart + SINT(m_buffer.size()) / m_channels;
    DEBUG_ASSERT(bufferEnd == m_inputFrames);
    std::vector<float> frame(m_channels);
    while (m_nextOutput < total) {
        const SINT block = m_nextOutput / m_outputStep;
        const int phase = int(m_nextOutput % m_outputStep);
        const std::vector<float>& taps = m_taps[phase];
        const SINT first = block * m_inputStep + m_firstTap[phase];
        const SINT end = first + SINT(taps.size());
        if (!last && end > m_inputFrames) {
            break;
        }
        std::fill(frame.begin(), frame.end(), 0.0f);
        if (first >= m_bufferStart && end <= m_inputFrames) {
            const float* const pInput = m_buffer.data() + (first - m_bufferStart) * m_channels;
            for (size_t tap = 0; tap < taps.size(); ++tap) {
                for (int channel = 0; channel < m_channels; ++channel) {
                    frame[channel] += taps[tap] * pInput[tap * m_channels + channel];
                }
            }
        } else {
            // Past an end of the track: julius repeats the first and the last
            // frame.
            for (size_t tap = 0; tap < taps.size(); ++tap) {
                const SINT index = std::min(first + SINT(tap), m_inputFrames - 1);
                const float* const pInput = index < 0
                        ? m_firstFrame.data()
                        : m_buffer.data() + (index - m_bufferStart) * m_channels;
                DEBUG_ASSERT(index < 0 || index >= m_bufferStart);
                for (int channel = 0; channel < m_channels; ++channel) {
                    frame[channel] += taps[tap] * pInput[channel];
                }
            }
        }
        pOutput->insert(pOutput->end(), frame.begin(), frame.end());
        ++m_nextOutput;
    }
    // Drops the input that no later output reads.
    const SINT needed = std::min(
            (m_nextOutput / m_outputStep) * m_inputStep + m_lowestTap, m_inputFrames - 1);
    if (needed > m_bufferStart) {
        m_buffer.erase(m_buffer.begin(),
                m_buffer.begin() + (needed - m_bufferStart) * m_channels);
        m_bufferStart = needed;
    }
}

} // namespace crate
