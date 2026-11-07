#pragma once

#include "BiquadFilter.hpp"
#include <array>

// 24-Band PEQ (Left 12 Bands, Right 12 Bands) strictly independent
class ParametricEQ
{
public:
    static constexpr size_t BANDS_PER_CHANNEL = 12;

    struct BandConfig {
        bool enabled = true;
        BiquadFilterType type = BiquadFilterType::Peak;
        double frequency = 1000.0;
        double Q = 1.0;
        double gainDb = 0.0;
    };

    ParametricEQ()
    {
        initDefaults();
    }

    void initDefaults()
    {
        // Standard ISO frequencies distributed across audio band
        const double defaultFreqs[BANDS_PER_CHANNEL] = {
            30.0, 60.0, 120.0, 250.0, 500.0, 1000.0,
            2000.0, 4000.0, 6000.0, 8000.0, 12000.0, 16000.0
        };

        for (size_t i = 0; i < BANDS_PER_CHANNEL; ++i) {
            m_leftBands[i].frequency = defaultFreqs[i];
            m_leftBands[i].gainDb = 0.0;
            m_leftBands[i].Q = 1.0;
            m_leftBands[i].type = (i == 0) ? BiquadFilterType::LowShelf :
                                  (i == BANDS_PER_CHANNEL - 1) ? BiquadFilterType::HighShelf :
                                  BiquadFilterType::Peak;

            m_rightBands[i] = m_leftBands[i];
        }
    }

    void configure(double sampleRate)
    {
        m_sampleRate = sampleRate;
        for (size_t i = 0; i < BANDS_PER_CHANNEL; ++i) {
            m_leftFilters[i].configure(m_leftBands[i].type, sampleRate,
                                       m_leftBands[i].frequency, m_leftBands[i].Q, m_leftBands[i].gainDb);
            m_rightFilters[i].configure(m_rightBands[i].type, sampleRate,
                                        m_rightBands[i].frequency, m_rightBands[i].Q, m_rightBands[i].gainDb);
        }
    }

    void setLeftBand(size_t index, const BandConfig& config)
    {
        if (index < BANDS_PER_CHANNEL) {
            m_leftBands[index] = config;
            m_leftFilters[index].configure(config.type, m_sampleRate, config.frequency, config.Q, config.gainDb);
        }
    }

    void setRightBand(size_t index, const BandConfig& config)
    {
        if (index < BANDS_PER_CHANNEL) {
            m_rightBands[index] = config;
            m_rightFilters[index].configure(config.type, m_sampleRate, config.frequency, config.Q, config.gainDb);
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

        for (size_t i = 0; i < BANDS_PER_CHANNEL; ++i) {
            if (m_leftBands[i].enabled) {
                left = m_leftFilters[i].process(left);
            }
            if (m_rightBands[i].enabled) {
                right = m_rightFilters[i].process(right);
            }
        }
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

private:
    double m_sampleRate = 48000.0;
    bool m_enabled = true;
    std::array<BandConfig, BANDS_PER_CHANNEL> m_leftBands;
    std::array<BandConfig, BANDS_PER_CHANNEL> m_rightBands;
    std::array<BiquadFilter, BANDS_PER_CHANNEL> m_leftFilters;
    std::array<BiquadFilter, BANDS_PER_CHANNEL> m_rightFilters;
};

