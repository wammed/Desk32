#pragma once

#include "BiquadFilter.hpp"
#include <array>
#include <vector>
#include <atomic>
#include <algorithm>

// 32-Band Graphic Equalizer (1/3 Octave ISO Frequencies)
class GraphicEQ
{
public:
    static constexpr size_t NUM_BANDS = 32;

    GraphicEQ()
    {
        initFrequencies();
        configure(48000.0);
    }

    GraphicEQ(const GraphicEQ& other)
    {
        *this = other;
    }

    GraphicEQ& operator=(const GraphicEQ& other)
    {
        if (this != &other) {
            m_sampleRate = other.m_sampleRate;
            m_enabled.store(other.m_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_frequencies = other.m_frequencies;
            m_gainsDb = other.m_gainsDb;
            m_leftFilters = other.m_leftFilters;
            m_rightFilters = other.m_rightFilters;
        }
        return *this;
    }

    void initFrequencies()
    {
        // 32 Standard ISO Frequencies (from 16 Hz to 20 kHz)
        const double isoFreqs[NUM_BANDS] = {
            16.0, 20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0,
            100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0,
            630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0,
            4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0
        };

        for (size_t i = 0; i < NUM_BANDS; ++i) {
            m_frequencies[i] = isoFreqs[i];
            m_gainsDb[i] = 0.0;
        }
    }

    void configure(double sampleRate)
    {
        m_sampleRate = sampleRate;
        const double Q = 4.318; // Standard Q for 1/3-octave band
        for (size_t i = 0; i < NUM_BANDS; ++i) {
            // Guard frequencies higher than Nyquist
            double freq = std::min(m_frequencies[i], sampleRate * 0.48);
            m_leftFilters[i].configure(BiquadFilterType::Peak, sampleRate, freq, Q, m_gainsDb[i]);
            m_rightFilters[i].configure(BiquadFilterType::Peak, sampleRate, freq, Q, m_gainsDb[i]);
        }
    }

    [[nodiscard]] double bandGain(size_t index) const noexcept {
        return (index < NUM_BANDS) ? m_gainsDb[index] : 0.0;
    }

    [[nodiscard]] double bandFrequency(size_t index) const noexcept {
        return (index < NUM_BANDS) ? m_frequencies[index] : 0.0;
    }

    void setBandGain(size_t index, double gainDb)
    {
        if (index < NUM_BANDS) {
            double clamped = std::clamp(gainDb, -12.0, 12.0);
            m_gainsDb[index] = clamped;
            const double Q = 4.318;
            double freq = std::min(m_frequencies[index], m_sampleRate * 0.48);
            m_leftFilters[index].configure(BiquadFilterType::Peak, m_sampleRate, freq, Q, clamped);
            m_rightFilters[index].configure(BiquadFilterType::Peak, m_sampleRate, freq, Q, clamped);
        }
    }

    void resetAllBands()
    {
        for (size_t i = 0; i < NUM_BANDS; ++i) {
            setBandGain(i, 0.0);
        }
    }

    /// Fast, real-time safe assignment of precalculated coefficients (Audio Thread, P0-27)
    inline void applyCoefficients(size_t index, const BiquadCoefficients& c) noexcept
    {
        if (index < NUM_BANDS) {
            m_leftFilters[index].setCoefficients(c);
            m_rightFilters[index].setCoefficients(c);
        }
    }

    /// Fast, real-time safe assignment of all precalculated band coefficients (Audio Thread, P0-27)
    inline void applyAllCoefficients(const std::array<BiquadCoefficients, NUM_BANDS>& coeffs) noexcept
    {
        for (size_t i = 0; i < NUM_BANDS; ++i) {
            m_leftFilters[i].setCoefficients(coeffs[i]);
            m_rightFilters[i].setCoefficients(coeffs[i]);
        }
    }

    /// Calculate combined magnitude response curve for visualization
    void calculateResponseCurve(const std::vector<double>& frequencies, std::vector<double>& outDb) const
    {
        outDb.resize(frequencies.size(), 0.0);
        if (!m_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        for (size_t f = 0; f < frequencies.size(); ++f) {
            double sumDb = 0.0;
            const double freq = frequencies[f];
            for (size_t b = 0; b < NUM_BANDS; ++b) {
                if (std::abs(m_gainsDb[b]) > 0.01) {
                    sumDb += m_leftFilters[b].magnitudeResponseDb(freq);
                }
            }
            outDb[f] = sumDb;
        }
    }

    void reset()
    {
        for (auto& f : m_leftFilters) f.reset();
        for (auto& f : m_rightFilters) f.reset();
    }

    inline void process(double& left, double& right)
    {
        if (!m_enabled.load(std::memory_order_relaxed)) return;

        for (size_t i = 0; i < NUM_BANDS; ++i) {
            left = m_leftFilters[i].process(left);
            right = m_rightFilters[i].process(right);
        }
    }

    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

private:
    double m_sampleRate = 48000.0;
    std::atomic<bool> m_enabled{false};
    std::array<double, NUM_BANDS> m_frequencies;
    std::array<double, NUM_BANDS> m_gainsDb;
    std::array<BiquadFilter, NUM_BANDS> m_leftFilters;
    std::array<BiquadFilter, NUM_BANDS> m_rightFilters;
};

