#pragma once

#include "BiquadFilter.hpp"
#include <atomic>
#include <vector>
#include <algorithm>

// Precision Butterworth High-Pass & Low-Pass Filter with selectable slope (12 dB / 24 dB per octave)
class HighLowPassFilter;

struct HighLowPassCoefficients {
    BiquadCoefficients hpfStage1 = BiquadCoefficients::identity();
    BiquadCoefficients hpfStage2 = BiquadCoefficients::identity();
    BiquadCoefficients lpfStage1 = BiquadCoefficients::identity();
    BiquadCoefficients lpfStage2 = BiquadCoefficients::identity();
    bool hpfEnabled = false;
    bool lpfEnabled = false;
    int hpfSlope = 0; // 0: 12dB, 1: 24dB
    int lpfSlope = 0; // 0: 12dB, 1: 24dB
    double hpfQ = 0.7071067811865475;
    double lpfQ = 0.7071067811865475;
};

namespace desk32::engine::dsp {
    using HighLowPassCoefficients = ::HighLowPassCoefficients;
}

class HighLowPassFilter
{
public:
    enum class FilterSlope {
        Slope12dB = 0, // 2nd-order Butterworth
        Slope24dB = 1  // 4th-order (cascaded Butterworth)
    };

    HighLowPassFilter()
    {
        configure(48000.0);
    }

    HighLowPassFilter(const HighLowPassFilter& other)
    {
        *this = other;
    }

    HighLowPassFilter& operator=(const HighLowPassFilter& other)
    {
        if (this != &other) {
            m_sampleRate = other.m_sampleRate;
            m_hpfFreq = other.m_hpfFreq;
            m_lpfFreq = other.m_lpfFreq;
            m_hpfSlope = other.m_hpfSlope;
            m_lpfSlope = other.m_lpfSlope;
            m_hpfQ = other.m_hpfQ;
            m_lpfQ = other.m_lpfQ;
            m_hpfEnabled.store(other.m_hpfEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_lpfEnabled.store(other.m_lpfEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_hpfLeft1 = other.m_hpfLeft1;
            m_hpfRight1 = other.m_hpfRight1;
            m_hpfLeft2 = other.m_hpfLeft2;
            m_hpfRight2 = other.m_hpfRight2;
            m_lpfLeft1 = other.m_lpfLeft1;
            m_lpfRight1 = other.m_lpfRight1;
            m_lpfLeft2 = other.m_lpfLeft2;
            m_lpfRight2 = other.m_lpfRight2;
        }
        return *this;
    }

    /// Calculate filter coefficients strictly outside Audio Thread (Control / Worker Thread, P0-27)
    static HighLowPassCoefficients calculateCoefficients(
        double sampleRate,
        double hpfFreq,
        double lpfFreq,
        FilterSlope hpfSlope,
        FilterSlope lpfSlope,
        bool hpfEnabled,
        bool lpfEnabled,
        double hpfQ = 0.7071067811865475,
        double lpfQ = 0.7071067811865475) noexcept
    {
        HighLowPassCoefficients c;
        c.hpfEnabled = hpfEnabled;
        c.lpfEnabled = lpfEnabled;
        c.hpfSlope = static_cast<int>(hpfSlope);
        c.lpfSlope = static_cast<int>(lpfSlope);
        c.hpfQ = std::clamp(hpfQ, 0.1, 10.0);
        c.lpfQ = std::clamp(lpfQ, 0.1, 10.0);

        if (sampleRate <= 0.0) return c;

        const double clampedHpf = std::clamp(hpfFreq, 10.0, 1000.0);
        const double clampedLpf = std::clamp(lpfFreq, 1000.0, std::min(22000.0, sampleRate * 0.48));

        c.hpfStage1 = BiquadFilter::calculateCoefficients(BiquadFilterType::HighPass, sampleRate, clampedHpf, c.hpfQ, 0.0);
        c.hpfStage2 = c.hpfStage1;
        c.lpfStage1 = BiquadFilter::calculateCoefficients(BiquadFilterType::LowPass, sampleRate, clampedLpf, c.lpfQ, 0.0);
        c.lpfStage2 = c.lpfStage1;

        return c;
    }

    /// Fast, real-time safe assignment of precalculated coefficients (Audio Thread execution, P0-27)
    /// Zero allocations, zero trigonometric functions, zero locks, bounded O(1) execution.
    inline void applyCoefficients(const HighLowPassCoefficients& c) noexcept
    {
        m_hpfLeft1.setCoefficients(c.hpfStage1);
        m_hpfRight1.setCoefficients(c.hpfStage1);
        m_hpfLeft2.setCoefficients(c.hpfStage2);
        m_hpfRight2.setCoefficients(c.hpfStage2);

        m_lpfLeft1.setCoefficients(c.lpfStage1);
        m_lpfRight1.setCoefficients(c.lpfStage1);
        m_lpfLeft2.setCoefficients(c.lpfStage2);
        m_lpfRight2.setCoefficients(c.lpfStage2);

        m_hpfSlope = (c.hpfSlope == 1) ? FilterSlope::Slope24dB : FilterSlope::Slope12dB;
        m_lpfSlope = (c.lpfSlope == 1) ? FilterSlope::Slope24dB : FilterSlope::Slope12dB;
        m_hpfQ = std::clamp(c.hpfQ, 0.1, 10.0);
        m_lpfQ = std::clamp(c.lpfQ, 0.1, 10.0);
        m_hpfEnabled.store(c.hpfEnabled, std::memory_order_relaxed);
        m_lpfEnabled.store(c.lpfEnabled, std::memory_order_relaxed);
    }

    void configure(double sampleRate, double hpfFreq = 20.0, double lpfFreq = 20000.0,
                   FilterSlope hpfSlope = FilterSlope::Slope12dB, FilterSlope lpfSlope = FilterSlope::Slope12dB,
                   double hpfQ = 0.7071067811865475, double lpfQ = 0.7071067811865475)
    {
        m_sampleRate = sampleRate;
        m_hpfFreq = std::clamp(hpfFreq, 10.0, 1000.0);
        m_lpfFreq = std::clamp(lpfFreq, 1000.0, std::min(22000.0, sampleRate * 0.48));
        m_hpfSlope = hpfSlope;
        m_lpfSlope = lpfSlope;
        m_hpfQ = std::clamp(hpfQ, 0.1, 10.0);
        m_lpfQ = std::clamp(lpfQ, 0.1, 10.0);

        auto c = calculateCoefficients(sampleRate, m_hpfFreq, m_lpfFreq, m_hpfSlope, m_lpfSlope,
                                       m_hpfEnabled.load(std::memory_order_relaxed),
                                       m_lpfEnabled.load(std::memory_order_relaxed),
                                       m_hpfQ, m_lpfQ);
        applyCoefficients(c);
    }

    [[nodiscard]] double hpfFrequency() const noexcept { return m_hpfFreq; }
    [[nodiscard]] double lpfFrequency() const noexcept { return m_lpfFreq; }
    [[nodiscard]] FilterSlope hpfSlope() const noexcept { return m_hpfSlope; }
    [[nodiscard]] FilterSlope lpfSlope() const noexcept { return m_lpfSlope; }
    [[nodiscard]] double hpfQ() const noexcept { return m_hpfQ; }
    [[nodiscard]] double lpfQ() const noexcept { return m_lpfQ; }

    void setHpfFrequency(double freq) {
        m_hpfFreq = std::clamp(freq, 10.0, 1000.0);
        m_hpfLeft1.configure(BiquadFilterType::HighPass, m_sampleRate, m_hpfFreq, m_hpfQ, 0.0);
        m_hpfRight1.configure(BiquadFilterType::HighPass, m_sampleRate, m_hpfFreq, m_hpfQ, 0.0);
        m_hpfLeft2.configure(BiquadFilterType::HighPass, m_sampleRate, m_hpfFreq, m_hpfQ, 0.0);
        m_hpfRight2.configure(BiquadFilterType::HighPass, m_sampleRate, m_hpfFreq, m_hpfQ, 0.0);
    }

    void setLpfFrequency(double freq) {
        m_lpfFreq = std::clamp(freq, 1000.0, std::min(22000.0, m_sampleRate * 0.48));
        m_lpfLeft1.configure(BiquadFilterType::LowPass, m_sampleRate, m_lpfFreq, m_lpfQ, 0.0);
        m_lpfRight1.configure(BiquadFilterType::LowPass, m_sampleRate, m_lpfFreq, m_lpfQ, 0.0);
        m_lpfLeft2.configure(BiquadFilterType::LowPass, m_sampleRate, m_lpfFreq, m_lpfQ, 0.0);
        m_lpfRight2.configure(BiquadFilterType::LowPass, m_sampleRate, m_lpfFreq, m_lpfQ, 0.0);
    }

    void setHpfQ(double q) {
        m_hpfQ = std::clamp(q, 0.1, 10.0);
        setHpfFrequency(m_hpfFreq);
    }

    void setLpfQ(double q) {
        m_lpfQ = std::clamp(q, 0.1, 10.0);
        setLpfFrequency(m_lpfFreq);
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
    double m_hpfQ = 0.7071067811865475;
    double m_lpfQ = 0.7071067811865475;
    std::atomic<bool> m_hpfEnabled{false};
    std::atomic<bool> m_lpfEnabled{false};

    BiquadFilter m_hpfLeft1, m_hpfRight1;
    BiquadFilter m_hpfLeft2, m_hpfRight2;
    BiquadFilter m_lpfLeft1, m_lpfRight1;
    BiquadFilter m_lpfLeft2, m_lpfRight2;
};

