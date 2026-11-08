#pragma once

#include <cmath>
#include <array>
#include <algorithm>
#include <atomic>

// Sample-Peak Brickwall Limiter with Lookahead & GR Telemetry (Explicitly NOT a True Peak Limiter, Section 5.5)
class PeakLimiter
{
public:
    static constexpr size_t MAX_LOOKAHEAD = 1024; // Up to 5.33 ms at 192 kHz

    PeakLimiter()
    {
        configure(48000.0);
    }

    void configure(double sampleRate, double ceilingDb = -0.1, double releaseMs = 50.0, double lookaheadMs = 1.0)
    {
        bool rateChanged = std::abs(m_sampleRate - sampleRate) > 0.01;
        m_sampleRate = sampleRate;
        m_ceilingDb = std::clamp(ceilingDb, -12.0, 0.0);
        m_ceiling = std::pow(10.0, m_ceilingDb / 20.0);
        m_releaseMs = std::clamp(releaseMs, 10.0, 500.0);
        m_lookaheadMs = std::clamp(lookaheadMs, 0.1, 5.0);

        m_releaseAlpha = std::exp(-1.0 / (sampleRate * (m_releaseMs * 0.001)));

        size_t samples = static_cast<size_t>(std::ceil(sampleRate * (m_lookaheadMs * 0.001)));
        size_t newLookahead = std::clamp(samples, size_t(8), MAX_LOOKAHEAD);

        if (rateChanged || newLookahead != m_lookaheadSamples) {
            m_lookaheadSamples = newLookahead;
            reset();
        }
    }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] double ceilingDb() const noexcept { return m_ceilingDb; }
    [[nodiscard]] double ceiling() const noexcept { return m_ceiling; }
    [[nodiscard]] double releaseMs() const noexcept { return m_releaseMs; }
    [[nodiscard]] double lookaheadMs() const noexcept { return m_lookaheadMs; }
    [[nodiscard]] size_t lookaheadSamples() const noexcept { return m_lookaheadSamples; }
    [[nodiscard]] int64_t latencyFrames() const noexcept {
        return m_enabled.load(std::memory_order_relaxed) ? static_cast<int64_t>(m_lookaheadSamples) : 0;
    }
    [[nodiscard]] double gainReductionDb() const noexcept { return m_gainReductionDb.load(std::memory_order_relaxed); }

    void setCeilingDb(double db) { configure(m_sampleRate, db, m_releaseMs, m_lookaheadMs); }
    void setReleaseMs(double ms) { configure(m_sampleRate, m_ceilingDb, ms, m_lookaheadMs); }
    void setLookaheadMs(double ms) { configure(m_sampleRate, m_ceilingDb, m_releaseMs, ms); }

    void reset()
    {
        m_bufferLeft.fill(0.0);
        m_bufferRight.fill(0.0);
        m_writePos = 0;
        m_currentGain = 1.0;
        m_gainReductionDb.store(0.0, std::memory_order_relaxed);
    }

    inline void process(double& left, double& right)
    {
        if (!m_enabled.load(std::memory_order_relaxed)) {
            m_gainReductionDb.store(0.0, std::memory_order_relaxed);
            return;
        }

        // Sample peak of incoming frame
        const double peak = std::max(std::abs(left), std::abs(right));

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

        // Record gain reduction in dB
        double grDb = (m_currentGain < 0.9999) ? -20.0 * std::log10(m_currentGain) : 0.0;
        m_gainReductionDb.store(grDb, std::memory_order_relaxed);

        // Retrieve delayed sample from ring buffer
        const double delayedLeft = m_bufferLeft[m_writePos];
        const double delayedRight = m_bufferRight[m_writePos];

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

    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

private:
    double m_sampleRate = 48000.0;
    double m_ceilingDb = -0.1;
    double m_ceiling = 0.98855; // -0.1 dB
    double m_releaseMs = 50.0;
    double m_lookaheadMs = 1.0;
    double m_releaseAlpha = 0.0;
    size_t m_lookaheadSamples = 48;
    size_t m_writePos = 0;
    double m_currentGain = 1.0;

    std::atomic<bool> m_enabled{false};
    std::atomic<double> m_gainReductionDb{0.0};

    // Preallocated fixed ring buffer (strictly zero dynamic allocation)
    alignas(64) std::array<double, MAX_LOOKAHEAD> m_bufferLeft{};
    alignas(64) std::array<double, MAX_LOOKAHEAD> m_bufferRight{};
};
