#pragma once

#include "BiquadFilter.hpp"
#include <atomic>
#include <vector>
#include <algorithm>

// Precision Butterworth High-Pass & Low-Pass Filter with selectable slope (12 / 18 / 24 dB per octave)
class HighLowPassFilter;

struct HighLowPassCoefficients {
    BiquadCoefficients hpfStage1 = BiquadCoefficients::identity();
    BiquadCoefficients hpfStage2 = BiquadCoefficients::identity();
    BiquadCoefficients lpfStage1 = BiquadCoefficients::identity();
    BiquadCoefficients lpfStage2 = BiquadCoefficients::identity();
    bool hpfEnabled = false;
    bool lpfEnabled = false;
    int hpfSlope = 0; // 0: 12dB, 1: 24dB, 2: 18dB
    int lpfSlope = 0; // 0: 12dB, 1: 24dB, 2: 18dB
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
        Slope12dB = 0, // 2nd-order (12 dB/oct)
        Slope24dB = 1, // 4th-order cascaded biquads (24 dB/oct)
        Slope18dB = 2  // 3rd-order: 2nd-order (user Q) + 1st-order bilinear (18 dB/oct)
    };

    [[nodiscard]] static constexpr FilterSlope slopeFromInt(int raw) noexcept {
        if (raw == 1 || raw == 24) return FilterSlope::Slope24dB;
        if (raw == 2 || raw == 18) return FilterSlope::Slope18dB;
        return FilterSlope::Slope12dB;
    }

    [[nodiscard]] static constexpr int slopeToDbPerOct(FilterSlope s) noexcept {
        switch (s) {
        case FilterSlope::Slope12dB: return 12;
        case FilterSlope::Slope18dB: return 18;
        case FilterSlope::Slope24dB: return 24;
        }
        return 12;
    }

    [[nodiscard]] static constexpr FilterSlope slopeFromDbPerOct(int dbOct) noexcept {
        if (dbOct == 24) return FilterSlope::Slope24dB;
        if (dbOct == 18) return FilterSlope::Slope18dB;
        return FilterSlope::Slope12dB;
    }

    HighLowPassFilter()
    {
        m_hpfInTrimGain.setImmediate(1.0);
        m_hpfOutTrimGain.setImmediate(1.0);
        m_lpfInTrimGain.setImmediate(1.0);
        m_lpfOutTrimGain.setImmediate(1.0);
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
            m_hpfInputTrimDb = other.m_hpfInputTrimDb;
            m_hpfOutputTrimDb = other.m_hpfOutputTrimDb;
            m_lpfInputTrimDb = other.m_lpfInputTrimDb;
            m_lpfOutputTrimDb = other.m_lpfOutputTrimDb;
            m_hpfEnabled.store(other.m_hpfEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_lpfEnabled.store(other.m_lpfEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_hpfMix = other.m_hpfMix;
            m_lpfMix = other.m_lpfMix;
            m_hpfSlopeMix = other.m_hpfSlopeMix;
            m_lpfSlopeMix = other.m_lpfSlopeMix;
            m_hpfInTrimGain = other.m_hpfInTrimGain;
            m_hpfOutTrimGain = other.m_hpfOutTrimGain;
            m_lpfInTrimGain = other.m_lpfInTrimGain;
            m_lpfOutTrimGain = other.m_lpfOutTrimGain;
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
        c.hpfStage2 = (hpfSlope == FilterSlope::Slope18dB)
                          ? BiquadFilter::calculateFirstOrderHighPass(sampleRate, clampedHpf)
                          : c.hpfStage1;
        c.lpfStage1 = BiquadFilter::calculateCoefficients(BiquadFilterType::LowPass, sampleRate, clampedLpf, c.lpfQ, 0.0);
        c.lpfStage2 = (lpfSlope == FilterSlope::Slope18dB)
                          ? BiquadFilter::calculateFirstOrderLowPass(sampleRate, clampedLpf)
                          : c.lpfStage1;

        return c;
    }

    /// Fast, real-time safe assignment of precalculated coefficients (Audio Thread execution, P0-27)
    /// Zero allocations, zero trigonometric functions, zero locks, bounded O(1) execution.
    inline void applyCoefficients(const HighLowPassCoefficients& c, size_t smoothSamples = 0) noexcept
    {
        if (smoothSamples > 1) {
            m_hpfLeft1.setCoefficientsSmooth(c.hpfStage1, smoothSamples);
            m_hpfRight1.setCoefficientsSmooth(c.hpfStage1, smoothSamples);
            m_hpfLeft2.setCoefficientsSmooth(c.hpfStage2, smoothSamples);
            m_hpfRight2.setCoefficientsSmooth(c.hpfStage2, smoothSamples);

            m_lpfLeft1.setCoefficientsSmooth(c.lpfStage1, smoothSamples);
            m_lpfRight1.setCoefficientsSmooth(c.lpfStage1, smoothSamples);
            m_lpfLeft2.setCoefficientsSmooth(c.lpfStage2, smoothSamples);
            m_lpfRight2.setCoefficientsSmooth(c.lpfStage2, smoothSamples);
        } else {
            m_hpfLeft1.setCoefficients(c.hpfStage1);
            m_hpfRight1.setCoefficients(c.hpfStage1);
            m_hpfLeft2.setCoefficients(c.hpfStage2);
            m_hpfRight2.setCoefficients(c.hpfStage2);

            m_lpfLeft1.setCoefficients(c.lpfStage1);
            m_lpfRight1.setCoefficients(c.lpfStage1);
            m_lpfLeft2.setCoefficients(c.lpfStage2);
            m_lpfRight2.setCoefficients(c.lpfStage2);
        }

        m_hpfSlope = slopeFromInt(c.hpfSlope);
        m_lpfSlope = slopeFromInt(c.lpfSlope);
        m_hpfQ = std::clamp(c.hpfQ, 0.1, 10.0);
        m_lpfQ = std::clamp(c.lpfQ, 0.1, 10.0);
        m_hpfEnabled.store(c.hpfEnabled, std::memory_order_relaxed);
        m_lpfEnabled.store(c.lpfEnabled, std::memory_order_relaxed);
        m_hpfMix.setTarget(c.hpfEnabled ? 1.0 : 0.0, smoothSamples);
        m_lpfMix.setTarget(c.lpfEnabled ? 1.0 : 0.0, smoothSamples);
        m_hpfSlopeMix.setTarget((m_hpfSlope != FilterSlope::Slope12dB) ? 1.0 : 0.0, smoothSamples);
        m_lpfSlopeMix.setTarget((m_lpfSlope != FilterSlope::Slope12dB) ? 1.0 : 0.0, smoothSamples);
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
    [[nodiscard]] double hpfInputTrimDb() const noexcept { return m_hpfInputTrimDb; }
    [[nodiscard]] double hpfOutputTrimDb() const noexcept { return m_hpfOutputTrimDb; }
    [[nodiscard]] double lpfInputTrimDb() const noexcept { return m_lpfInputTrimDb; }
    [[nodiscard]] double lpfOutputTrimDb() const noexcept { return m_lpfOutputTrimDb; }

    [[nodiscard]] static inline double trimDbToLinear(double db) noexcept {
        const double clamped = std::clamp(db, -12.0, 12.0);
        return (std::abs(clamped) <= 1e-9) ? 1.0 : std::pow(10.0, clamped / 20.0);
    }

    void setHpfTrimDb(double inDb, double outDb, size_t smoothSamples = 0) noexcept {
        const double cin = std::clamp(inDb, -12.0, 12.0);
        const double cout = std::clamp(outDb, -12.0, 12.0);
        m_hpfInputTrimDb = (std::abs(cin) <= 1e-9) ? 0.0 : cin;
        m_hpfOutputTrimDb = (std::abs(cout) <= 1e-9) ? 0.0 : cout;
        m_hpfInTrimGain.setTarget(trimDbToLinear(m_hpfInputTrimDb), smoothSamples);
        m_hpfOutTrimGain.setTarget(trimDbToLinear(m_hpfOutputTrimDb), smoothSamples);
    }

    void setLpfTrimDb(double inDb, double outDb, size_t smoothSamples = 0) noexcept {
        const double cin = std::clamp(inDb, -12.0, 12.0);
        const double cout = std::clamp(outDb, -12.0, 12.0);
        m_lpfInputTrimDb = (std::abs(cin) <= 1e-9) ? 0.0 : cin;
        m_lpfOutputTrimDb = (std::abs(cout) <= 1e-9) ? 0.0 : cout;
        m_lpfInTrimGain.setTarget(trimDbToLinear(m_lpfInputTrimDb), smoothSamples);
        m_lpfOutTrimGain.setTarget(trimDbToLinear(m_lpfOutputTrimDb), smoothSamples);
    }

    void setHpfFrequency(double freq) {
        m_hpfFreq = std::clamp(freq, 10.0, 1000.0);
        auto c = calculateCoefficients(m_sampleRate, m_hpfFreq, m_lpfFreq, m_hpfSlope, m_lpfSlope,
                                       m_hpfEnabled.load(std::memory_order_relaxed),
                                       m_lpfEnabled.load(std::memory_order_relaxed),
                                       m_hpfQ, m_lpfQ);
        m_hpfLeft1.setCoefficients(c.hpfStage1);
        m_hpfRight1.setCoefficients(c.hpfStage1);
        m_hpfLeft2.setCoefficients(c.hpfStage2);
        m_hpfRight2.setCoefficients(c.hpfStage2);
    }

    void setLpfFrequency(double freq) {
        m_lpfFreq = std::clamp(freq, 1000.0, std::min(22000.0, m_sampleRate * 0.48));
        auto c = calculateCoefficients(m_sampleRate, m_hpfFreq, m_lpfFreq, m_hpfSlope, m_lpfSlope,
                                       m_hpfEnabled.load(std::memory_order_relaxed),
                                       m_lpfEnabled.load(std::memory_order_relaxed),
                                       m_hpfQ, m_lpfQ);
        m_lpfLeft1.setCoefficients(c.lpfStage1);
        m_lpfRight1.setCoefficients(c.lpfStage1);
        m_lpfLeft2.setCoefficients(c.lpfStage2);
        m_lpfRight2.setCoefficients(c.lpfStage2);
    }

    void setHpfQ(double q) {
        m_hpfQ = std::clamp(q, 0.1, 10.0);
        setHpfFrequency(m_hpfFreq);
    }

    void setLpfQ(double q) {
        m_lpfQ = std::clamp(q, 0.1, 10.0);
        setLpfFrequency(m_lpfFreq);
    }

    void setHpfSlope(FilterSlope slope) {
        m_hpfSlope = slope;
        setHpfFrequency(m_hpfFreq);
        m_hpfSlopeMix.setImmediate((slope != FilterSlope::Slope12dB) ? 1.0 : 0.0);
    }
    void setLpfSlope(FilterSlope slope) {
        m_lpfSlope = slope;
        setLpfFrequency(m_lpfFreq);
        m_lpfSlopeMix.setImmediate((slope != FilterSlope::Slope12dB) ? 1.0 : 0.0);
    }

    void setHpfEnabled(bool enabled) noexcept {
        m_hpfEnabled.store(enabled, std::memory_order_relaxed);
        m_hpfMix.setImmediate(enabled ? 1.0 : 0.0);
    }
    [[nodiscard]] bool isHpfEnabled() const noexcept { return m_hpfEnabled.load(std::memory_order_relaxed); }

    void setLpfEnabled(bool enabled) noexcept {
        m_lpfEnabled.store(enabled, std::memory_order_relaxed);
        m_lpfMix.setImmediate(enabled ? 1.0 : 0.0);
    }
    [[nodiscard]] bool isLpfEnabled() const noexcept { return m_lpfEnabled.load(std::memory_order_relaxed); }

    [[nodiscard]] bool isSmoothingTransition() const noexcept {
        return m_hpfMix.remaining > 0 || m_lpfMix.remaining > 0 ||
               m_hpfSlopeMix.remaining > 0 || m_lpfSlopeMix.remaining > 0;
    }

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
                if (m_hpfSlope != FilterSlope::Slope12dB) {
                    sumDb += m_hpfLeft2.magnitudeResponseDb(freq);
                }
            }
            if (lpfOn) {
                sumDb += m_lpfLeft1.magnitudeResponseDb(freq);
                if (m_lpfSlope != FilterSlope::Slope12dB) {
                    sumDb += m_lpfLeft2.magnitudeResponseDb(freq);
                }
            }
            outDb[f] = sumDb;
        }
    }

    inline void process(double& left, double& right)
    {
        processWithBypass(left, right, false);
    }

    inline void processWithBypass(double& left, double& right, bool bypassFilters) noexcept
    {
        applyTrimSample(left, right, m_hpfInTrimGain);
        if (!bypassFilters) {
            processSection(left, right,
                           m_hpfLeft1, m_hpfRight1, m_hpfLeft2, m_hpfRight2,
                           m_hpfMix, m_hpfSlopeMix);
        }
        applyTrimSample(left, right, m_hpfOutTrimGain);
        applyTrimSample(left, right, m_lpfInTrimGain);
        if (!bypassFilters) {
            processSection(left, right,
                           m_lpfLeft1, m_lpfRight1, m_lpfLeft2, m_lpfRight2,
                           m_lpfMix, m_lpfSlopeMix);
        }
        applyTrimSample(left, right, m_lpfOutTrimGain);
    }

    inline void processWithDry(double& left, double& right, double& dryLeft, double& dryRight) noexcept
    {
        applyTrimSamplePair(left, right, dryLeft, dryRight, m_hpfInTrimGain);
        processSection(left, right,
                       m_hpfLeft1, m_hpfRight1, m_hpfLeft2, m_hpfRight2,
                       m_hpfMix, m_hpfSlopeMix);
        applyTrimSamplePair(left, right, dryLeft, dryRight, m_hpfOutTrimGain);
        applyTrimSamplePair(left, right, dryLeft, dryRight, m_lpfInTrimGain);
        processSection(left, right,
                       m_lpfLeft1, m_lpfRight1, m_lpfLeft2, m_lpfRight2,
                       m_lpfMix, m_lpfSlopeMix);
        applyTrimSamplePair(left, right, dryLeft, dryRight, m_lpfOutTrimGain);
    }

    inline void processBypassedTrimsStatic(double& left, double& right) const noexcept
    {
        if (m_hpfInTrimGain.current != 1.0) { left *= m_hpfInTrimGain.current; right *= m_hpfInTrimGain.current; }
        if (m_hpfOutTrimGain.current != 1.0) { left *= m_hpfOutTrimGain.current; right *= m_hpfOutTrimGain.current; }
        if (m_lpfInTrimGain.current != 1.0) { left *= m_lpfInTrimGain.current; right *= m_lpfInTrimGain.current; }
        if (m_lpfOutTrimGain.current != 1.0) { left *= m_lpfOutTrimGain.current; right *= m_lpfOutTrimGain.current; }
    }

private:
    struct MixRamp {
        double current = 0.0;
        double target = 0.0;
        double step = 0.0;
        size_t remaining = 0;

        void setImmediate(double v) noexcept {
            current = v;
            target = v;
            step = 0.0;
            remaining = 0;
        }

        void setTarget(double newTarget, size_t smoothSamples) noexcept {
            if (smoothSamples <= 1) {
                setImmediate(newTarget);
                return;
            }
            if (remaining == 0 && current == newTarget && target == newTarget) {
                return;
            }
            if (remaining > 0 && target == newTarget) {
                return;
            }
            target = newTarget;
            step = (target - current) / static_cast<double>(smoothSamples);
            remaining = smoothSamples;
        }

        inline double advance() noexcept {
            if (remaining > 0) {
                current += step;
                --remaining;
                if (remaining == 0) {
                    current = target;
                }
            }
            return current;
        }
    };

    static inline void processSection(
        double& left,
        double& right,
        BiquadFilter& s1L,
        BiquadFilter& s1R,
        BiquadFilter& s2L,
        BiquadFilter& s2R,
        MixRamp& enableMix,
        MixRamp& slopeMix) noexcept
    {
        const double dryL = left;
        const double dryR = right;

        // Keep all filter stages warmed on the live signal even when disabled or at 12 dB/oct
        const double st1L = s1L.process(dryL);
        const double st1R = s1R.process(dryR);
        const double st2L = s2L.process(st1L);
        const double st2R = s2R.process(st1R);

        const double sm = slopeMix.advance();
        const double wetL = (sm <= 0.0) ? st1L : ((sm >= 1.0) ? st2L : (st1L + (st2L - st1L) * sm));
        const double wetR = (sm <= 0.0) ? st1R : ((sm >= 1.0) ? st2R : (st1R + (st2R - st1R) * sm));

        const double em = enableMix.advance();
        if (em <= 0.0) {
            left = dryL;
            right = dryR;
        } else if (em >= 1.0) {
            left = wetL;
            right = wetR;
        } else {
            left = dryL + (wetL - dryL) * em;
            right = dryR + (wetR - dryR) * em;
        }
    }

    static inline void applyTrimSample(double& left, double& right, MixRamp& ramp) noexcept
    {
        const double g = ramp.advance();
        if (g != 1.0) {
            left *= g;
            right *= g;
        }
    }

    static inline void applyTrimSamplePair(double& l1, double& r1, double& l2, double& r2, MixRamp& ramp) noexcept
    {
        const double g = ramp.advance();
        if (g != 1.0) {
            l1 *= g;
            r1 *= g;
            l2 *= g;
            r2 *= g;
        }
    }

    double m_sampleRate = 48000.0;
    double m_hpfFreq = 20.0;
    double m_lpfFreq = 20000.0;
    FilterSlope m_hpfSlope = FilterSlope::Slope12dB;
    FilterSlope m_lpfSlope = FilterSlope::Slope12dB;
    double m_hpfQ = 0.7071067811865475;
    double m_lpfQ = 0.7071067811865475;
    double m_hpfInputTrimDb = 0.0;
    double m_hpfOutputTrimDb = 0.0;
    double m_lpfInputTrimDb = 0.0;
    double m_lpfOutputTrimDb = 0.0;
    std::atomic<bool> m_hpfEnabled{false};
    std::atomic<bool> m_lpfEnabled{false};
    MixRamp m_hpfMix;
    MixRamp m_lpfMix;
    MixRamp m_hpfSlopeMix;
    MixRamp m_lpfSlopeMix;
    MixRamp m_hpfInTrimGain;
    MixRamp m_hpfOutTrimGain;
    MixRamp m_lpfInTrimGain;
    MixRamp m_lpfOutTrimGain;

    BiquadFilter m_hpfLeft1, m_hpfRight1;
    BiquadFilter m_hpfLeft2, m_hpfRight2;
    BiquadFilter m_lpfLeft1, m_lpfRight1;
    BiquadFilter m_lpfLeft2, m_lpfRight2;
};

