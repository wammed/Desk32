#pragma once

#include <cmath>
#include <vector>
#include <algorithm>

// Sample-Peak Brickwall Limiter with fast lookahead (Explicitly NOT a True Peak Limiter, per spec 5.5)
class PeakLimiter
{
public:
    PeakLimiter()
    {
        configure(48000.0);
    }

    void configure(double sampleRate, double ceilingDb = -0.1, double releaseMs = 50.0, double lookaheadMs = 1.0)
    {
        m_sampleRate = sampleRate;
        m_ceiling = std::pow(10.0, ceilingDb / 20.0);
        m_releaseAlpha = std::exp(-1.0 / (sampleRate * (releaseMs * 0.001)));

        m_lookaheadSamples = static_cast<size_t>(std::ceil(sampleRate * (lookaheadMs * 0.001)));
        m_lookaheadSamples = std::max<size_t>(m_lookaheadSamples, 16);

        m_bufferLeft.assign(m_lookaheadSamples, 0.0);
        m_bufferRight.assign(m_lookaheadSamples, 0.0);
        m_writePos = 0;
        m_currentGain = 1.0;
    }

    void reset()
    {
        std::fill(m_bufferLeft.begin(), m_bufferLeft.end(), 0.0);
        std::fill(m_bufferRight.begin(), m_bufferRight.end(), 0.0);
        m_writePos = 0;
        m_currentGain = 1.0;
    }

    inline void process(double& left, double& right)
    {
        if (!m_enabled || m_bufferLeft.empty()) return;

        // Sample peak of incoming frame
        double peak = std::max(std::abs(left), std::abs(right));

        double targetGain = 1.0;
        if (peak > m_ceiling) {
            targetGain = m_ceiling / peak;
        }

        // Instantaneous attack (brickwall lookahead), exponential release
        if (targetGain < m_currentGain) {
            m_currentGain = targetGain;
        } else {
            m_currentGain = m_releaseAlpha * m_currentGain + (1.0 - m_releaseAlpha) * targetGain;
        }

        // Retrieve delayed sample
        double delayedLeft = m_bufferLeft[m_writePos];
        double delayedRight = m_bufferRight[m_writePos];

        // Store new sample into ring buffer
        m_bufferLeft[m_writePos] = left;
        m_bufferRight[m_writePos] = right;
        m_writePos = (m_writePos + 1) % m_lookaheadSamples;

        // Output delayed sample scaled by currentGain
        left = delayedLeft * m_currentGain;
        right = delayedRight * m_currentGain;

        // Final safety ceiling clamp against numerical transients
        if (std::abs(left) > m_ceiling) {
            left = (left > 0.0) ? m_ceiling : -m_ceiling;
        }
        if (std::abs(right) > m_ceiling) {
            right = (right > 0.0) ? m_ceiling : -m_ceiling;
        }
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }
    double ceiling() const { return m_ceiling; }

private:
    double m_sampleRate = 48000.0;
    double m_ceiling = 0.98855; // -0.1 dB
    double m_releaseAlpha = 0.0;
    size_t m_lookaheadSamples = 48;
    size_t m_writePos = 0;
    double m_currentGain = 1.0;
    bool m_enabled = true;

    std::vector<double> m_bufferLeft;
    std::vector<double> m_bufferRight;
};
