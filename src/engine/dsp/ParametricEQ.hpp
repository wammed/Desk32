#pragma once

#include "BiquadFilter.hpp"
#include <array>
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>
#include <cmath>

// 32-Band PEQ (Left 16 Bands, Right 16 Bands) strictly independent
class ParametricEQ
{
public:
    static constexpr size_t BANDS_PER_CHANNEL = 16;

    struct BandConfig {
        bool enabled = true;
        BiquadFilterType type = BiquadFilterType::Peak;
        double frequency = 1000.0;
        double Q = 1.0;
        double gainDb = 0.0;
    };

    struct BandImportReport {
        size_t requestedLeft = 0;
        size_t requestedRight = 0;
        size_t activeLeft = 0;
        size_t activeRight = 0;
        size_t overflowLeft = 0;
        size_t overflowRight = 0;
        bool truncated = false;
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
            m_overflowLeftBands = other.m_overflowLeftBands;
            m_overflowRightBands = other.m_overflowRightBands;
            m_leftFilters = other.m_leftFilters;
            m_rightFilters = other.m_rightFilters;
            m_preampDb = other.m_preampDb;
            m_preampLinear = other.m_preampLinear;
        }
        return *this;
    }

    void initDefaults()
    {
        // Standard 16 ISO frequencies distributed across audio band
        const double defaultFreqs[BANDS_PER_CHANNEL] = {
            25.0, 40.0, 63.0, 100.0, 160.0, 250.0, 400.0, 630.0,
            1000.0, 1600.0, 2500.0, 4000.0, 6300.0, 10000.0, 14000.0, 18000.0
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

    /// Fast, real-time safe assignment of precalculated coefficients (Audio Thread, P0-27)
    inline void applyCoefficients(const std::array<BiquadCoefficients, BANDS_PER_CHANNEL>& leftCoeffs,
                                  const std::array<BiquadCoefficients, BANDS_PER_CHANNEL>& rightCoeffs,
                                  const std::array<bool, BANDS_PER_CHANNEL>& leftEnabled,
                                  const std::array<bool, BANDS_PER_CHANNEL>& rightEnabled,
                                  size_t smoothSamples = 0) noexcept
    {
        for (size_t i = 0; i < BANDS_PER_CHANNEL; ++i) {
            if (smoothSamples > 1) {
                m_leftFilters[i].setCoefficientsSmooth(leftCoeffs[i], smoothSamples);
                m_rightFilters[i].setCoefficientsSmooth(rightCoeffs[i], smoothSamples);
            } else {
                m_leftFilters[i].setCoefficients(leftCoeffs[i]);
                m_rightFilters[i].setCoefficients(rightCoeffs[i]);
            }
            m_leftBands[i].enabled = leftEnabled[i];
            m_rightBands[i].enabled = rightEnabled[i];
        }
    }

    /// Export all bands including any >16 variable overflow bands (Spec §5.2 / v3.8 §5.6)
    [[nodiscard]] std::vector<BandConfig> exportBands(bool rightChannel) const {
        const auto& src = rightChannel ? m_rightBands : m_leftBands;
        const auto& overflow = rightChannel ? m_overflowRightBands : m_overflowLeftBands;
        std::vector<BandConfig> out(src.begin(), src.end());
        out.insert(out.end(), overflow.begin(), overflow.end());
        return out;
    }

    [[nodiscard]] const std::vector<BandConfig>& overflowLeftBands() const noexcept { return m_overflowLeftBands; }
    [[nodiscard]] const std::vector<BandConfig>& overflowRightBands() const noexcept { return m_overflowRightBands; }
    [[nodiscard]] size_t totalLeftBandCount() const noexcept { return BANDS_PER_CHANNEL + m_overflowLeftBands.size(); }
    [[nodiscard]] size_t totalRightBandCount() const noexcept { return BANDS_PER_CHANNEL + m_overflowRightBands.size(); }
    void clearOverflowBands() {
        m_overflowLeftBands.clear();
        m_overflowRightBands.clear();
    }

    /// Import bands from dynamic vector serialization: first 16 activate in DSP, excess (>16) retained in variable overflow vector (§5.2)
    /// Any remaining slots beyond bands.size() up to BANDS_PER_CHANNEL are reset to flat / OFF (enabled = false).
    void importBands(bool rightChannel, const std::vector<BandConfig>& bands) {
        const double defaultFreqs[BANDS_PER_CHANNEL] = {
            25.0, 40.0, 63.0, 100.0, 160.0, 250.0, 400.0, 630.0,
            1000.0, 1600.0, 2500.0, 4000.0, 6300.0, 10000.0, 14000.0, 18000.0
        };
        for (size_t i = 0; i < BANDS_PER_CHANNEL; ++i) {
            BandConfig offBand;
            offBand.enabled = false;
            offBand.frequency = defaultFreqs[i];
            offBand.Q = 1.0;
            offBand.gainDb = 0.0;
            offBand.type = (i == 0) ? BiquadFilterType::LowShelf
                                    : ((i == BANDS_PER_CHANNEL - 1) ? BiquadFilterType::HighShelf
                                                                    : BiquadFilterType::Peak);
            if (rightChannel) {
                setRightBand(i, offBand);
            } else {
                setLeftBand(i, offBand);
            }
        }
        const size_t n = std::min(bands.size(), BANDS_PER_CHANNEL);
        for (size_t i = 0; i < n; ++i) {
            if (rightChannel) {
                setRightBand(i, bands[i]);
            } else {
                setLeftBand(i, bands[i]);
            }
        }
        auto& overflow = rightChannel ? m_overflowRightBands : m_overflowLeftBands;
        overflow.clear();
        if (bands.size() > BANDS_PER_CHANNEL) {
            overflow.assign(bands.begin() + static_cast<std::ptrdiff_t>(BANDS_PER_CHANNEL), bands.end());
        }
    }

    /// Import L/R bands and report whether >16 bands triggered overflow truncation in active DSP (§5.2, §11.1)
    BandImportReport importBandsWithReport(const std::vector<BandConfig>& leftBands,
                                           const std::vector<BandConfig>& rightBands)
    {
        importBands(false, leftBands);
        importBands(true, rightBands);
        BandImportReport report;
        report.requestedLeft = leftBands.size();
        report.requestedRight = rightBands.size();
        report.activeLeft = std::min(leftBands.size(), BANDS_PER_CHANNEL);
        report.activeRight = std::min(rightBands.size(), BANDS_PER_CHANNEL);
        report.overflowLeft = m_overflowLeftBands.size();
        report.overflowRight = m_overflowRightBands.size();
        report.truncated = (report.overflowLeft > 0 || report.overflowRight > 0);
        return report;
    }

    static std::vector<std::string> factoryPresetNames()
    {
        return {
            "Monitor Flat",
            "Acoustic Clarity",
            "Harman Target",
            "Bass Punch",
            "Vocal Air",
            "Nearfield Speaker",
            "Open-Back Headphone",
            "Closed-Back Headphone",
            "DJ Club Monitor"
        };
    }

    /// Extract preset band configurations without modifying DSP state (Control Thread, P0-27)
    static bool getPresetBandConfigs(const std::string& presetName,
                                     std::array<BandConfig, BANDS_PER_CHANNEL>& leftBands,
                                     std::array<BandConfig, BANDS_PER_CHANNEL>& rightBands)
    {
        const double defaultFreqs[BANDS_PER_CHANNEL] = {
            25.0, 40.0, 63.0, 100.0, 160.0, 250.0, 400.0, 630.0,
            1000.0, 1600.0, 2500.0, 4000.0, 6300.0, 10000.0, 14000.0, 18000.0
        };

        for (size_t i = 0; i < BANDS_PER_CHANNEL; ++i) {
            leftBands[i].frequency = defaultFreqs[i];
            leftBands[i].gainDb = 0.0;
            leftBands[i].Q = 1.0;
            leftBands[i].type = (i == 0) ? BiquadFilterType::LowShelf : ((i == BANDS_PER_CHANNEL - 1) ? BiquadFilterType::HighShelf : BiquadFilterType::Peak);
            leftBands[i].enabled = true;

            rightBands[i] = leftBands[i];
        }

        if (presetName == "Monitor Flat") {
            // Flat defaults
        } else if (presetName == "Acoustic Clarity") {
            leftBands[5].gainDb = -2.0;  // 250 Hz
            leftBands[11].gainDb = 1.5;  // 4 kHz
            leftBands[13].gainDb = 2.5;  // 10 kHz
            rightBands[5].gainDb = -2.0;
            rightBands[11].gainDb = 1.5;
            rightBands[13].gainDb = 2.5;
        } else if (presetName == "Harman Target") {
            leftBands[1].frequency = 60.0;
            leftBands[1].gainDb = 4.0;   // Low boost
            leftBands[7].frequency = 4000.0;
            leftBands[7].gainDb = 2.5;   // Presence boost
            rightBands[1].frequency = 60.0;
            rightBands[1].gainDb = 4.0;
            rightBands[7].frequency = 4000.0;
            rightBands[7].gainDb = 2.5;
        } else if (presetName == "Bass Punch") {
            leftBands[2].gainDb = 3.5;   // 63 Hz
            leftBands[3].gainDb = 2.0;   // 100 Hz
            leftBands[5].gainDb = -1.5;  // 250 Hz
            rightBands[2].gainDb = 3.5;
            rightBands[3].gainDb = 2.0;
            rightBands[5].gainDb = -1.5;
        } else if (presetName == "Vocal Air") {
            leftBands[7].gainDb = -1.0;  // 630 Hz
            leftBands[10].gainDb = 2.0;  // 2.5 kHz
            leftBands[12].gainDb = 3.0;  // 6.3 kHz
            rightBands[7].gainDb = -1.0;
            rightBands[10].gainDb = 2.0;
            rightBands[12].gainDb = 3.0;
        } else if (presetName == "Nearfield Speaker") {
            leftBands[4].gainDb = -2.0;  // 160 Hz desk boundary dip
            leftBands[4].Q = 1.2;
            leftBands[10].gainDb = 1.0;  // 2.5 kHz presence
            leftBands[14].gainDb = 1.5;  // 14 kHz off-axis HF lift
            rightBands[4] = leftBands[4];
            rightBands[10] = leftBands[10];
            rightBands[14] = leftBands[14];
        } else if (presetName == "Open-Back Headphone") {
            leftBands[0].gainDb = 3.5;   // 25 Hz LowShelf sub-bass extension
            leftBands[2].gainDb = 2.0;   // 63 Hz warmth
            leftBands[10].gainDb = -1.5; // 2.5 kHz ear-gain smooth
            leftBands[12].gainDb = -1.5; // 6.3 kHz sibilance tame
            rightBands[0] = leftBands[0];
            rightBands[2] = leftBands[2];
            rightBands[10] = leftBands[10];
            rightBands[12] = leftBands[12];
        } else if (presetName == "Closed-Back Headphone") {
            leftBands[4].gainDb = -2.5;  // 160 Hz cup boom reduction
            leftBands[5].gainDb = -1.5;  // 250 Hz mud reduction
            leftBands[11].gainDb = 2.0;  // 4 kHz clarity
            leftBands[14].gainDb = 2.5;  // 14 kHz openness
            rightBands[4] = leftBands[4];
            rightBands[5] = leftBands[5];
            rightBands[11] = leftBands[11];
            rightBands[14] = leftBands[14];
        } else if (presetName == "DJ Club Monitor") {
            leftBands[1].gainDb = 2.5;   // 40 Hz sub weight
            leftBands[3].gainDb = 1.5;   // 100 Hz kick punch
            leftBands[10].gainDb = -2.5; // 2.5 kHz harshness protection
            leftBands[11].gainDb = -2.0; // 4 kHz fatigue control
            leftBands[13].gainDb = 1.0;  // 10 kHz hi-hat crispness
            rightBands[1] = leftBands[1];
            rightBands[3] = leftBands[3];
            rightBands[10] = leftBands[10];
            rightBands[11] = leftBands[11];
            rightBands[13] = leftBands[13];
        } else {
            return false;
        }
        return true;
    }

    /// Apply built-in Listening Correction profile (Section 5.10)
    bool applyPreset(const std::string& presetName)
    {
        if (!getPresetBandConfigs(presetName, m_leftBands, m_rightBands)) {
            return false;
        }
        setPreampDb(0.0);
        clearOverflowBands();
        configure(m_sampleRate);
        return true;
    }

    void setPreampDb(double db) noexcept
    {
        const double clamped = std::clamp(db, -24.0, 24.0);
        m_preampDb = (std::abs(clamped) <= 1e-9) ? 0.0 : clamped;
        m_preampLinear = (m_preampDb == 0.0) ? 1.0 : std::pow(10.0, m_preampDb / 20.0);
    }

    [[nodiscard]] double preampDb() const noexcept { return m_preampDb; }
    [[nodiscard]] double preampLinear() const noexcept { return m_preampLinear; }

    void reset()
    {
        for (auto& f : m_leftFilters) f.reset();
        for (auto& f : m_rightFilters) f.reset();
    }

    inline void processBandsOnly(double& left, double& right)
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

    inline void process(double& left, double& right)
    {
        if (!m_enabled.load(std::memory_order_relaxed)) return;

        if (m_preampLinear != 1.0) {
            left *= m_preampLinear;
            right *= m_preampLinear;
        }

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
    double m_preampDb = 0.0;
    double m_preampLinear = 1.0;
    std::atomic<bool> m_enabled{false};
    std::array<BandConfig, BANDS_PER_CHANNEL> m_leftBands;
    std::array<BandConfig, BANDS_PER_CHANNEL> m_rightBands;
    std::vector<BandConfig> m_overflowLeftBands;
    std::vector<BandConfig> m_overflowRightBands;
    std::array<BiquadFilter, BANDS_PER_CHANNEL> m_leftFilters;
    std::array<BiquadFilter, BANDS_PER_CHANNEL> m_rightFilters;
};

