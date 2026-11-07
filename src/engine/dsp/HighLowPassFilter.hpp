#pragma once

#include "BiquadFilter.hpp"
#include <atomic>
#include <vector>
#include <algorithm>

// Precision Butterworth High-Pass & Low-Pass Filter with selectable slope (12 dB / 24 dB per octave)
class HighLowPassFilter
{
public:
    enum class FilterSlope {
        Slope12dB, // 2nd-order Butterworth
        Slope24dB  // 4th-order (cascaded Butterworth)
    };

    HighLowPassFilter()
    {
        configure(48000.0);
    }

    void configure(double sampleRate, double hpfFreq = 20.0, double lpfFreq = 20000.0,
                   FilterSlope hpfSlope = FilterSlope::Slope12dB, FilterSlope lpfSlope = FilterSlope::Slope12dB)
    {
        m_sampleRate = sampleRate;
        m_hpfFreq = std::clamp(hpfFreq, 10.0, 1000.0);
        m_lpfFreq = std::clamp(lpfFreq, 1000.0, std::min(22000.0, sampleRate * 0.48));
        m_hpfSlope = hpfSlope;
        m_lpfSlope = lpfSlope;

        const double Q = 0.7071067811865475; // Butterworth Q

        // Stage 1
        m_hpfLeft1.configure(BiquadFilterType::HighPass, sampleRate, m_hpfFreq, Q, 0.0);
        m_hpfRight1.configure(BiquadFilterType::HighPass, sampleRate, m_hpfFreq, Q, 0.0);
        m_lpfLeft1.configure(BiquadFilterType::LowPass, sampleRate, m_lpfFreq, Q, 0.0);
        m_lpfRight1.configure(BiquadFilterType::LowPass, sampleRate, m_lpfFreq, Q, 0.0);

        // Stage 2 (for 24 dB/oct)
        m_hpfLeft2.configure(BiquadFilterType::HighPass, sampleRate, m_hpfFreq, Q, 0.0);
        m_hpfRight2.configure(BiquadFilterType::HighPass, sampleRate, m_hpfFreq, Q, 0.0);
        m_lpfLeft2.configure(BiquadFilterType::LowPass, sampleRate, m_lpfFreq, Q, 0.0);
        m_lpfRight2.configure(BiquadFilterType::LowPass, sampleRate, m_lpfFreq, Q, 0.0);
    }

    [[nodiscard]] double hpfFrequency() const noexcept { return m_hpfFreq; }
    [[nodiscard]] double lpfFrequency() const noexcept { return m_lpfFreq; }
    [[nodiscard]] FilterSlope hpfSlope() const noexcept { return m_hpfSlope; }
    [[nodiscard]] FilterSlope lpfSlope() const noexcept { return m_lpfSlope; }

    void setHpfFrequency(double freq) {
        m_hpfFreq = std::clamp(freq, 10.0, 1000.0);
        const double Q = 0.7071067811865475;
        m_hpfLeft1.configure(BiquadFilterType::HighPass, m_sampleRate, m_hpfFreq, Q, 0.0);
        m_hpfRight1.configure(BiquadFilterType::HighPass, m_sampleRate, m_hpfFreq, Q, 0.0);
        m_hpfLeft2.configure(BiquadFilterType::HighPass, m_sampleRate, m_hpfFreq, Q, 0.0);
        m_hpfRight2.configure(BiquadFilterType::HighPass, m_sampleRate, m_hpfFreq, Q, 0.0);
    }

    void setLpfFrequency(double freq) {
        m_lpfFreq = std::clamp(freq, 1000.0, std::min(22000.0, m_sampleRate * 0.48));
        const double Q = 0.7071067811865475;
        m_lpfLeft1.configure(BiquadFilterType::LowPass, m_sampleRate, m_lpfFreq, Q, 0.0);
        m_lpfRight1.configure(BiquadFilterType::LowPass, m_sampleRate, m_lpfFreq, Q, 0.0);
        m_lpfLeft2.configure(BiquadFilterType::LowPass, m_sampleRate, m_lpfFreq, Q, 0.0);
        m_lpfRight2.configure(BiquadFilterType::LowPass, m_sampleRate, m_lpfFreq, Q, 0.0);
    }

    void setHpfSlope(FilterSlope slope) { m_hpfSlope = slope; }
    void setLpfSlope(FilterSlope slope) { m_lpfSlope = slope; }

    void setHpfEnabled(bool enabled) noexcept { m_hpfEnabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isHpfEnabled() const noexcept { return m_hpfEnabled.load(std::memory_order_relaxed); }

    void setLpfEnabled(bool enabled) noexcept { m_lpfEnabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isLpfEnabled() const noexcept { return m_lpfEnabled.load(std::memory_order_relaxed); }

    void reset()
    {
        m_hpfLeft1.reset();
        m_hpfRight1.reset();
        m_lpfLeft1.reset();
        m_lpfRight1.reset();
        m_hpfLeft2.reset();
        m_hpfRight2.reset();
        m_lpfLeft2.reset();
        m_lpfRight2.reset();
    }

    /// Calculate combined magnitude response curve for visualization
    void calculateResponseCurve(const std::vector<double>& frequencies, std::vector<double>& outDb) const
    {
        outDb.resize(frequencies.size(), 0.0);
        const bool hpfOn = m_hpfEnabled.load(std::memory_order_relaxed);
        const bool lpfOn = m_lpfEnabled.load(std::memory_order_relaxed);

        for (size_t f = 0; f < frequencies.size(); ++f) {
            double sumDb = 0.0;
            const double freq = frequencies[f];

            if (hpfOn) {
                sumDb += m_hpfLeft1.magnitudeResponseDb(freq);
                if (m_hpfSlope == FilterSlope::Slope24dB) {
                    sumDb += m_hpfLeft2.magnitudeResponseDb(freq);
                }
            }
            if (lpfOn) {
                sumDb += m_lpfLeft1.magnitudeResponseDb(freq);
                if (m_lpfSlope == FilterSlope::Slope24dB) {
                    sumDb += m_lpfLeft2.magnitudeResponseDb(freq);
                }
            }
            outDb[f] = sumDb;
        }
    }

    inline void process(double& left, double& right)
    {
        if (m_hpfEnabled.load(std::memory_order_relaxed)) {
            left = m_hpfLeft1.process(left);
            right = m_hpfRight1.process(right);
            if (m_hpfSlope == FilterSlope::Slope24dB) {
                left = m_hpfLeft2.process(left);
                right = m_hpfRight2.process(right);
            }
        }
        if (m_lpfEnabled.load(std::memory_order_relaxed)) {
            left = m_lpfLeft1.process(left);
            right = m_lpfRight1.process(right);
            if (m_lpfSlope == FilterSlope::Slope24dB) {
                left = m_lpfLeft2.process(left);
                right = m_lpfRight2.process(right);
            }
        }
    }

private:
    double m_sampleRate = 48000.0;
    double m_hpfFreq = 20.0;
    double m_lpfFreq = 20000.0;
    FilterSlope m_hpfSlope = FilterSlope::Slope12dB;
    FilterSlope m_lpfSlope = FilterSlope::Slope12dB;
    std::atomic<bool> m_hpfEnabled{false};
    std::atomic<bool> m_lpfEnabled{false};

    BiquadFilter m_hpfLeft1, m_hpfRight1;
    BiquadFilter m_hpfLeft2, m_hpfRight2;
    BiquadFilter m_lpfLeft1, m_lpfRight1;
    BiquadFilter m_lpfLeft2, m_lpfRight2;
};

