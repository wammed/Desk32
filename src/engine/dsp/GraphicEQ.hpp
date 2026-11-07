#pragma once

#include "BiquadFilter.hpp"
#include <array>

// 32-Band Graphic Equalizer (1/3 Octave ISO Frequencies)
class GraphicEQ
{
public:
    static constexpr size_t NUM_BANDS = 32;

    GraphicEQ()
    {
        initFrequencies();
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

    void setBandGain(size_t index, double gainDb)
    {
        if (index < NUM_BANDS) {
            m_gainsDb[index] = gainDb;
            const double Q = 4.318;
            double freq = std::min(m_frequencies[index], m_sampleRate * 0.48);
            m_leftFilters[index].configure(BiquadFilterType::Peak, m_sampleRate, freq, Q, gainDb);
            m_rightFilters[index].configure(BiquadFilterType::Peak, m_sampleRate, freq, Q, gainDb);
        }
    }

    void reset()
    {
        for (auto& f : m_leftFilters) f.reset();
        for (auto& f : m_rightFilters) f.reset();
    }

    inline void process(double& left, double& right)
    {
        if (!m_enabled) return;

        for (size_t i = 0; i < NUM_BANDS; ++i) {
            left = m_leftFilters[i].process(left);
            right = m_rightFilters[i].process(right);
        }
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

private:
    double m_sampleRate = 48000.0;
    bool m_enabled = true;
    std::array<double, NUM_BANDS> m_frequencies;
    std::array<double, NUM_BANDS> m_gainsDb;
    std::array<BiquadFilter, NUM_BANDS> m_leftFilters;
    std::array<BiquadFilter, NUM_BANDS> m_rightFilters;
};

