#pragma once

#include <cmath>
#include <algorithm>
#include <atomic>

// Clean, neutral 64-bit float Feedforward Compressor with Soft-Knee and GR reporting (Section 5.4)
class Compressor
{
public:
    Compressor()
    {
        configure(48000.0);
    }

    void configure(double sampleRate, double thresholdDb = -12.0, double ratio = 4.0,
                   double attackMs = 10.0, double releaseMs = 100.0, double makeupGainDb = 0.0,
                   double kneeDb = 4.0)
    {
        m_sampleRate = sampleRate;
        m_thresholdDb = std::clamp(thresholdDb, -60.0, 0.0);
        m_ratio = std::clamp(ratio, 1.0, 20.0);
        m_attackMs = std::max(0.1, attackMs);
        m_releaseMs = std::max(1.0, releaseMs);
        m_makeupGainDb = std::clamp(makeupGainDb, -12.0, 24.0);
        m_makeupGain = std::pow(10.0, m_makeupGainDb / 20.0);
        m_kneeDb = std::clamp(kneeDb, 0.0, 20.0);

        m_attackAlpha = std::exp(-1.0 / (sampleRate * std::max(0.0001, m_attackMs * 0.001)));
        m_releaseAlpha = std::exp(-1.0 / (sampleRate * std::max(0.001, m_releaseMs * 0.001)));
    }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] double thresholdDb() const noexcept { return m_thresholdDb; }
    [[nodiscard]] double ratio() const noexcept { return m_ratio; }
    [[nodiscard]] double attackMs() const noexcept { return m_attackMs; }
    [[nodiscard]] double releaseMs() const noexcept { return m_releaseMs; }
    [[nodiscard]] double makeupGainDb() const noexcept { return m_makeupGainDb; }
    [[nodiscard]] double kneeDb() const noexcept { return m_kneeDb; }
    [[nodiscard]] double gainReductionDb() const noexcept { return m_gainReductionDb.load(std::memory_order_relaxed); }

    void setThresholdDb(double db) { configure(m_sampleRate, db, m_ratio, m_attackMs, m_releaseMs, m_makeupGainDb, m_kneeDb); }
    void setRatio(double r) { configure(m_sampleRate, m_thresholdDb, r, m_attackMs, m_releaseMs, m_makeupGainDb, m_kneeDb); }
    void setAttackMs(double ms) { m_attackMs = ms; configure(m_sampleRate, m_thresholdDb, m_ratio, ms, m_releaseMs, m_makeupGainDb, m_kneeDb); }
    void setReleaseMs(double ms) { m_releaseMs = ms; configure(m_sampleRate, m_thresholdDb, m_ratio, m_attackMs, ms, m_makeupGainDb, m_kneeDb); }
    void setMakeupGainDb(double db) { configure(m_sampleRate, m_thresholdDb, m_ratio, m_attackMs, m_releaseMs, db, m_kneeDb); }
    void setKneeDb(double db) { configure(m_sampleRate, m_thresholdDb, m_ratio, m_attackMs, m_releaseMs, m_makeupGainDb, db); }

    void reset()
    {
        m_envelope = 0.0;
        m_gainReductionDb.store(0.0, std::memory_order_relaxed);
    }

    inline void process(double& left, double& right)
    {
        if (!m_enabled.load(std::memory_order_relaxed)) {
            m_gainReductionDb.store(0.0, std::memory_order_relaxed);
            return;
        }

        // Peak detector (max of absolute stereo channels)
        const double peak = std::max(std::abs(left), std::abs(right));

        // Envelope follower with decoupled attack / release filtering
        if (peak > m_envelope) {
            m_envelope = (1.0 - m_attackAlpha) * peak + m_attackAlpha * m_envelope;
        } else {
            m_envelope = (1.0 - m_releaseAlpha) * peak + m_releaseAlpha * m_envelope;
        }

        const double envDb = (m_envelope > 1e-9) ? 20.0 * std::log10(m_envelope) : -180.0;

        // Dynamic curve with soft-knee interpolation
        double grDb = 0.0;
        const double delta = envDb - m_thresholdDb;
        const double halfKnee = m_kneeDb * 0.5;

        if (m_kneeDb > 0.1 && delta > -halfKnee && delta < halfKnee) {
            // Soft-knee quadratic transition
            const double normalizedKnee = (delta + halfKnee) / m_kneeDb;
            grDb = (normalizedKnee * normalizedKnee * halfKnee) * (1.0 - 1.0 / m_ratio);
        } else if (delta >= halfKnee) {
            // Full compression region
            grDb = delta * (1.0 - 1.0 / m_ratio);
        }

        m_gainReductionDb.store(grDb, std::memory_order_relaxed);

        const double gainLinear = std::pow(10.0, -grDb / 20.0) * m_makeupGain;
        left *= gainLinear;
        right *= gainLinear;
    }

    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

private:
    double m_sampleRate = 48000.0;
    double m_thresholdDb = -12.0;
    double m_ratio = 4.0;
    double m_attackMs = 10.0;
    double m_releaseMs = 100.0;
    double m_makeupGainDb = 0.0;
    double m_makeupGain = 1.0;
    double m_kneeDb = 4.0;
    double m_attackAlpha = 0.0;
    double m_releaseAlpha = 0.0;
    double m_envelope = 0.0;

    std::atomic<bool> m_enabled{false};
    std::atomic<double> m_gainReductionDb{0.0};
};

