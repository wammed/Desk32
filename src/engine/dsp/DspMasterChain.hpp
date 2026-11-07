#pragma once

#include "ParametricEQ.hpp"
#include "GraphicEQ.hpp"
#include "HighLowPassFilter.hpp"
#include "Compressor.hpp"
#include "PeakLimiter.hpp"
#include "AlgorithmicReverb.hpp"

// Master DSP Chain with all v1 built-in DSPs strictly adhering to Section 5.8 order
class DspMasterChain
{
public:
    DspMasterChain() = default;

    void configure(double sampleRate)
    {
        m_sampleRate = sampleRate;
        m_peq.configure(sampleRate);
        m_geq.configure(sampleRate);
        m_filters.configure(sampleRate);
        m_compressor.configure(sampleRate);
        m_limiter.configure(sampleRate);
        m_reverb.configure(sampleRate);
    }

    void reset()
    {
        m_peq.reset();
        m_geq.reset();
        m_filters.reset();
        m_compressor.reset();
        m_limiter.reset();
        m_reverb.reset();
    }

    inline void process(double& left, double& right)
    {
        if (m_globalBypass) return;

        // 1. PEQ & GEQ (5.8)
        m_peq.process(left, right);
        m_geq.process(left, right);

        // 2. HPF / LPF
        m_filters.process(left, right);

        // 3. Compressor
        m_compressor.process(left, right);

        // 4. Limiter
        m_limiter.process(left, right);

        // 5. Reverb
        m_reverb.process(left, right);
    }

    ParametricEQ& peq() { return m_peq; }
    GraphicEQ& geq() { return m_geq; }
    HighLowPassFilter& filters() { return m_filters; }
    Compressor& compressor() { return m_compressor; }
    PeakLimiter& limiter() { return m_limiter; }
    AlgorithmicReverb& reverb() { return m_reverb; }

    void setGlobalBypass(bool bypass) { m_globalBypass = bypass; }
    bool isGlobalBypass() const { return m_globalBypass; }

private:
    double m_sampleRate = 48000.0;
    bool m_globalBypass = false;

    ParametricEQ m_peq;
    GraphicEQ m_geq;
    HighLowPassFilter m_filters;
    Compressor m_compressor;
    PeakLimiter m_limiter;
    AlgorithmicReverb m_reverb;
};

