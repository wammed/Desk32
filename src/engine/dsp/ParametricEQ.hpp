#pragma once

#include "BiquadFilter.hpp"
#include <array>
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>

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
        configure(48000.0);
    }

    ParametricEQ(const ParametricEQ& other)
    {
        *this = other;
    }

    ParametricEQ& operator=(const ParametricEQ& other)
    {
        if (this != &other) {
            m_sampleRate = other.m_sampleRate;
            m_enabled.store(other.m_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_leftBands = other.m_leftBands;
            m_rightBands = other.m_rightBands;
            m_leftFilters = other.m_leftFilters;
            m_rightFilters = other.m_rightFilters;
        }
        return *this;
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
            m_leftBands[i].enabled = true;

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

    [[nodiscard]] const BandConfig& leftBand(size_t index) const {
        static const BandConfig s_fallback{};
        return (index < BANDS_PER_CHANNEL) ? m_leftBands[index] : s_fallback;
    }

    [[nodiscard]] const BandConfig& rightBand(size_t index) const {
        static const BandConfig s_fallback{};
        return (index < BANDS_PER_CHANNEL) ? m_rightBands[index] : s_fallback;
    }

    void setLeftBand(size_t index, BandConfig config)
    {
        if (index < BANDS_PER_CHANNEL) {
            config.frequency = std::clamp(config.frequency, 10.0, std::min(24000.0, m_sampleRate * 0.48));
            config.gainDb = std::clamp(config.gainDb, -24.0, 24.0);
            config.Q = std::clamp(config.Q, 0.1, 20.0);
            m_leftBands[index] = config;
            m_leftFilters[index].configure(config.type, m_sampleRate, config.frequency, config.Q, config.gainDb);
        }
    }

    void setRightBand(size_t index, BandConfig config)
    {
        if (index < BANDS_PER_CHANNEL) {
            config.frequency = std::clamp(config.frequency, 10.0, std::min(24000.0, m_sampleRate * 0.48));
            config.gainDb = std::clamp(config.gainDb, -24.0, 24.0);
            config.Q = std::clamp(config.Q, 0.1, 20.0);
            m_rightBands[index] = config;
            m_rightFilters[index].configure(config.type, m_sampleRate, config.frequency, config.Q, config.gainDb);
        }
    }

    /// Calculate combined magnitude response curve across all active bands for visualization
    void calculateCompositeResponse(const std::vector<double>& frequencies,
                                    std::vector<double>& outLeftDb,
                                    std::vector<double>& outRightDb) const
    {
        outLeftDb.resize(frequencies.size(), 0.0);
        outRightDb.resize(frequencies.size(), 0.0);

        if (!m_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        for (size_t f = 0; f < frequencies.size(); ++f) {
            double leftSum = 0.0;
            double rightSum = 0.0;
            const double freq = frequencies[f];

            for (size_t b = 0; b < BANDS_PER_CHANNEL; ++b) {
                if (m_leftBands[b].enabled) {
                    leftSum += m_leftFilters[b].magnitudeResponseDb(freq);
                }
                if (m_rightBands[b].enabled) {
                    rightSum += m_rightFilters[b].magnitudeResponseDb(freq);
                }
            }
            outLeftDb[f] = leftSum;
            outRightDb[f] = rightSum;
        }
    }

    /// Apply built-in Listening Correction profile (Section 5.10)
    bool applyPreset(const std::string& presetName)
    {
        if (presetName == "Monitor Flat") {
            for (size_t i = 0; i < BANDS_PER_CHANNEL; ++i) {
                m_leftBands[i].gainDb = 0.0;
                m_leftBands[i].enabled = true;
                m_rightBands[i].gainDb = 0.0;
                m_rightBands[i].enabled = true;
            }
        } else if (presetName == "Acoustic Clarity") {
            // Low-mid boxiness scoop, gentle presence boost, airy top end
            applyPreset("Monitor Flat");
            m_leftBands[3].gainDb = -2.0; // 250 Hz
            m_leftBands[7].gainDb = 1.5;  // 4 kHz
            m_leftBands[10].gainDb = 2.5; // 12 kHz
            m_rightBands[3].gainDb = -2.0;
            m_rightBands[7].gainDb = 1.5;
            m_rightBands[10].gainDb = 2.5;
        } else if (presetName == "Harman Target") {
            // Sub-bass warmth (+4.0 dB shelf) + pinna ear gain (+2.5 dB peak at 4 kHz)
            applyPreset("Monitor Flat");
            m_leftBands[1].gainDb = 4.0;  // 60 Hz shelf/peak
            m_leftBands[7].gainDb = 2.5;  // 4 kHz
            m_rightBands[1].gainDb = 4.0;
            m_rightBands[7].gainDb = 2.5;
        } else if (presetName == "Bass Punch") {
            applyPreset("Monitor Flat");
            m_leftBands[1].gainDb = 3.5;  // 60 Hz
            m_leftBands[2].gainDb = 2.0;  // 120 Hz
            m_leftBands[3].gainDb = -1.5; // 250 Hz (clean mud)
            m_rightBands[1].gainDb = 3.5;
            m_rightBands[2].gainDb = 2.0;
            m_rightBands[3].gainDb = -1.5;
        } else if (presetName == "Vocal Air") {
            applyPreset("Monitor Flat");
            m_leftBands[4].gainDb = -1.0; // 500 Hz
            m_leftBands[6].gainDb = 2.0;  // 2 kHz
            m_leftBands[9].gainDb = 3.0;  // 8 kHz
            m_rightBands[4].gainDb = -1.0;
            m_rightBands[6].gainDb = 2.0;
            m_rightBands[9].gainDb = 3.0;
        } else {
            return false;
        }

        configure(m_sampleRate);
        return true;
    }

    void reset()
    {
        for (auto& f : m_leftFilters) f.reset();
        for (auto& f : m_rightFilters) f.reset();
    }

    inline void process(double& left, double& right)
    {
        if (!m_enabled.load(std::memory_order_relaxed)) return;

        for (size_t i = 0; i < BANDS_PER_CHANNEL; ++i) {
            if (m_leftBands[i].enabled) {
                left = m_leftFilters[i].process(left);
            }
            if (m_rightBands[i].enabled) {
                right = m_rightFilters[i].process(right);
            }
        }
    }

    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

private:
    double m_sampleRate = 48000.0;
    std::atomic<bool> m_enabled{false};
    std::array<BandConfig, BANDS_PER_CHANNEL> m_leftBands;
    std::array<BandConfig, BANDS_PER_CHANNEL> m_rightBands;
    std::array<BiquadFilter, BANDS_PER_CHANNEL> m_leftFilters;
    std::array<BiquadFilter, BANDS_PER_CHANNEL> m_rightFilters;
};

