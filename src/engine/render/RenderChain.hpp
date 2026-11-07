#pragma once

#include <memory>
#include <vector>
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/dsp/Compressor.hpp"
#include "engine/dsp/PeakLimiter.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include "engine/pipeline/AudioNode.hpp"

namespace desk32::engine::render {

/// Offline DSP Render Chain for File Output (Specification Section 5.11.3 & 20.1).
/// Operates exclusively on the background Render Worker Thread (NEVER on Audio Thread).
/// Architectural Guarantees:
/// - Strictly EXCLUDES Listening Correction (0.000000 leakage into rendered files)
/// - Strictly EXCLUDES Listening Volume and Render Monitor Volume
/// - Completely decoupled from real-time live playback state
class RenderChain {
public:
    RenderChain() = default;
    ~RenderChain() = default;

    RenderChain(const RenderChain&) = delete;
    RenderChain& operator=(const RenderChain&) = delete;

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }
    void setEnabled(bool enabled) noexcept { m_enabled = enabled; }

    void configure(double sampleRate) {
        m_sampleRate = sampleRate;
        m_peq.configure(sampleRate);
        m_geq.configure(sampleRate);
        m_filter.configure(sampleRate);
        m_comp.configure(sampleRate);
        m_limiter.configure(sampleRate);
        m_reverb.configure(sampleRate);
    }

    void reset() {
        m_peq.reset();
        m_geq.reset();
        m_filter.reset();
        m_comp.reset();
        m_limiter.reset();
        m_reverb.reset();
    }

    // Direct access to cloned DSP processors
    [[nodiscard]] ParametricEQ& peq() noexcept { return m_peq; }
    [[nodiscard]] GraphicEQ& geq() noexcept { return m_geq; }
    [[nodiscard]] HighLowPassFilter& filter() noexcept { return m_filter; }
    [[nodiscard]] Compressor& compressor() noexcept { return m_comp; }
    [[nodiscard]] PeakLimiter& limiter() noexcept { return m_limiter; }
    [[nodiscard]] AlgorithmicReverb& reverb() noexcept { return m_reverb; }

    /// Synchronize parameters from live console Master Chain
    void copyParametersFrom(const ParametricEQ& peq,
                            const GraphicEQ& geq,
                            const HighLowPassFilter& flt,
                            const Compressor& comp,
                            const AlgorithmicReverb& rev,
                            const PeakLimiter& lim)
    {
        // 1. Parametric EQ
        for (size_t b = 0; b < 12; ++b) {
            m_peq.setLeftBand(b, peq.leftBand(b));
            m_peq.setRightBand(b, peq.rightBand(b));
        }
        m_peq.setEnabled(peq.isEnabled());

        // 2. Graphic EQ
        for (size_t b = 0; b < 32; ++b) {
            m_geq.setBandGain(b, geq.bandGain(b));
        }
        m_geq.setEnabled(geq.isEnabled());

        // 3. Filter
        m_filter.setHpfFrequency(flt.hpfFrequency());
        m_filter.setHpfSlope(flt.hpfSlope());
        m_filter.setHpfEnabled(flt.isHpfEnabled());
        m_filter.setLpfFrequency(flt.lpfFrequency());
        m_filter.setLpfSlope(flt.lpfSlope());
        m_filter.setLpfEnabled(flt.isLpfEnabled());

        // 4. Compressor
        m_comp.configure(m_sampleRate, comp.thresholdDb(), comp.ratio(), comp.attackMs(), comp.releaseMs(), comp.makeupGainDb(), comp.kneeDb());
        m_comp.setEnabled(comp.isEnabled());

        // 5. Reverb
        m_reverb.configure(m_sampleRate, rev.roomSize(), rev.damping(), rev.wetLevel(), rev.predelayMs(), rev.isTailPreserving());
        m_reverb.setEnabled(rev.isEnabled());

        // 6. Peak Limiter
        m_limiter.configure(m_sampleRate, lim.ceilingDb(), lim.releaseMs(), lim.lookaheadMs());
        m_limiter.setEnabled(lim.isEnabled());
    }

    /// Process planar double stereo block through offline Render Chain
    void process(double* left, double* right, size_t frameCount) {
        if (!m_enabled || !left || !right || frameCount == 0) return;

        // Upstream DSP Chain in Mastering Order:
        // 1. PEQ
        if (m_peq.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_peq.process(left[i], right[i]);
            }
        }

        // 2. GEQ
        if (m_geq.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_geq.process(left[i], right[i]);
            }
        }

        // 3. HPF / LPF
        if (m_filter.isHpfEnabled() || m_filter.isLpfEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_filter.process(left[i], right[i]);
            }
        }

        // 4. Compressor
        if (m_comp.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_comp.process(left[i], right[i]);
            }
        }

        // 5. Algorithmic Reverb
        if (m_reverb.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_reverb.process(left[i], right[i]);
            }
        }

        // 6. Peak Limiter (brickwall safeguard for render)
        if (m_limiter.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_limiter.process(left[i], right[i]);
            }
        }

        // NOTE: Strictly NO Listening Correction here!
        // Listening Correction is isolated to Listening Path only.
    }

private:
    double m_sampleRate = 48000.0;
    bool m_enabled = true;

    ParametricEQ m_peq;
    GraphicEQ m_geq;
    HighLowPassFilter m_filter;
    Compressor m_comp;
    PeakLimiter m_limiter;
    AlgorithmicReverb m_reverb;
};

} // namespace desk32::engine::render
