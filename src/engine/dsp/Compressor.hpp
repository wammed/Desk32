#pragma once

#include <cmath>
#include <algorithm>

// Clean, uncolored 64-bit float Feedforward Compressor
class Compressor
{
public:
    Compressor() = default;

    void configure(double sampleRate, double thresholdDb = -12.0, double ratio = 4.0,
                   double attackMs = 10.0, double releaseMs = 100.0, double makeupGainDb = 0.0)
    {
        m_sampleRate = sampleRate;
        m_thresholdDb = thresholdDb;
        m_ratio = std::max(ratio, 1.0);
        m_makeupGain = std::pow(10.0, makeupGainDb / 20.0);

        m_attackAlpha = std::exp(-1.0 / (sampleRate * (attackMs * 0.001)));
        m_releaseAlpha = std::exp(-1.0 / (sampleRate * (releaseMs * 0.001)));
    }

    void reset()
    {
        m_envelope = 0.0;
    }

    inline void process(double& left, double& right)
    {
        if (!m_enabled) return;

        // Peak detector (max of abs channels)
        double peak = std::max(std::abs(left), std::abs(right));

        // Envelope follower with attack/release smoothing
        if (peak > m_envelope) {
            m_envelope = (1.0 - m_attackAlpha) * peak + m_attackAlpha * m_envelope;
        } else {
            m_envelope = (1.0 - m_releaseAlpha) * peak + m_releaseAlpha * m_envelope;
        }

        double envDb = (m_envelope > 1e-9) ? 20.0 * std::log10(m_envelope) : -180.0;

        double gainReductionDb = 0.0;
        if (envDb > m_thresholdDb) {
            double excess = envDb - m_thresholdDb;
            gainReductionDb = excess * (1.0 - 1.0 / m_ratio);
        }

        double gain = std::pow(10.0, -gainReductionDb / 20.0) * m_makeupGain;
        left *= gain;
        right *= gain;
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

private:
    double m_sampleRate = 48000.0;
    double m_thresholdDb = -12.0;
    double m_ratio = 4.0;
    double m_makeupGain = 1.0;
    double m_attackAlpha = 0.0;
    double m_releaseAlpha = 0.0;
    double m_envelope = 0.0;
    bool m_enabled = true;
};

