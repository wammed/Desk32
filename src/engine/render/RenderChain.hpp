#pragma once

#include <memory>
#include <vector>
#include <array>
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/dsp/Compressor.hpp"
#include "engine/dsp/PeakLimiter.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include "engine/pipeline/AudioNode.hpp"

namespace desk32::engine::render {

/// Immutable snapshot of DSP parameters captured at Render Job enqueue time (P0-03).
/// Guarantees that running and enqueued render jobs are completely isolated from
/// subsequent GUI tweaks, live console edits, and subsequent render job registrations.
struct RenderDspSnapshot {
    bool enabled = true;

    // PEQ
    bool peqEnabled = true;
    std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> peqLeftBands{};
    std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> peqRightBands{};

    // GEQ
    bool geqEnabled = true;
    std::array<double, 32> geqGains{};
    std::array<bool, 32> geqBandEnabled = [] {
        std::array<bool, 32> a{};
        a.fill(true);
        return a;
    }();

    // Filter
    bool hpfEnabled = false;
    double hpfFrequency = 20.0;
    HighLowPassFilter::FilterSlope hpfSlope = HighLowPassFilter::FilterSlope::Slope12dB;
    double hpfQ = 0.7071067811865475;
    bool lpfEnabled = false;
    double lpfFrequency = 20000.0;
    HighLowPassFilter::FilterSlope lpfSlope = HighLowPassFilter::FilterSlope::Slope12dB;
    double lpfQ = 0.7071067811865475;

    // Compressor
    bool compEnabled = false;
    double compThresholdDb = -12.0;
    double compRatio = 4.0;
    double compAttackMs = 10.0;
    double compReleaseMs = 100.0;
    double compMakeupGainDb = 0.0;
    double compKneeDb = 4.0;

    // Reverb
    bool reverbEnabled = false;
    double reverbRoomSize = 0.7;
    double reverbDamp = 0.3;
    double reverbWetLevel = 0.2;
    double reverbPredelayMs = 0.0;
    bool reverbTailPreserving = true;

    // Limiter
    bool limiterEnabled = true;
    double limiterCeilingDb = -0.1;
    double limiterReleaseMs = 50.0;
    double limiterLookaheadMs = 1.0;

    // External Insert live-session exclusion notice (§6.6)
    bool externalInsertWasActiveInLive = false;
    std::string externalInsertNodeName;
};

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

    void setExternalInsertLiveStatus(bool active, std::string nodeName = {}) {
        m_externalInsertWasActiveInLive = active;
        m_externalInsertNodeName = std::move(nodeName);
    }

    // Direct access to cloned DSP processors
    [[nodiscard]] ParametricEQ& peq() noexcept { return m_peq; }
    [[nodiscard]] const ParametricEQ& peq() const noexcept { return m_peq; }
    [[nodiscard]] GraphicEQ& geq() noexcept { return m_geq; }
    [[nodiscard]] const GraphicEQ& geq() const noexcept { return m_geq; }
    [[nodiscard]] HighLowPassFilter& filter() noexcept { return m_filter; }
    [[nodiscard]] const HighLowPassFilter& filter() const noexcept { return m_filter; }
    [[nodiscard]] Compressor& compressor() noexcept { return m_comp; }
    [[nodiscard]] const Compressor& compressor() const noexcept { return m_comp; }
    [[nodiscard]] PeakLimiter& limiter() noexcept { return m_limiter; }
    [[nodiscard]] const PeakLimiter& limiter() const noexcept { return m_limiter; }
    [[nodiscard]] AlgorithmicReverb& reverb() noexcept { return m_reverb; }
    [[nodiscard]] const AlgorithmicReverb& reverb() const noexcept { return m_reverb; }

    /// Synchronize parameters from live console Master Chain
    void copyParametersFrom(const ParametricEQ& peq,
                            const GraphicEQ& geq,
                            const HighLowPassFilter& flt,
                            const Compressor& comp,
                            const AlgorithmicReverb& rev,
                            const PeakLimiter& lim)
    {
        // 1. Parametric EQ
        for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
            m_peq.setLeftBand(b, peq.leftBand(b));
            m_peq.setRightBand(b, peq.rightBand(b));
        }
        m_peq.setEnabled(peq.isEnabled());

        // 2. Graphic EQ
        for (size_t b = 0; b < 32; ++b) {
            m_geq.setBandEnabled(b, geq.isBandEnabled(b));
            m_geq.setBandGain(b, geq.bandGain(b));
        }
        m_geq.setEnabled(geq.isEnabled());

        // 3. Filter
        m_filter.setHpfFrequency(flt.hpfFrequency());
        m_filter.setHpfSlope(flt.hpfSlope());
        m_filter.setHpfQ(flt.hpfQ());
        m_filter.setHpfEnabled(flt.isHpfEnabled());
        m_filter.setLpfFrequency(flt.lpfFrequency());
        m_filter.setLpfSlope(flt.lpfSlope());
        m_filter.setLpfQ(flt.lpfQ());
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

    /// Capture an immutable parameter snapshot (P0-03)
    [[nodiscard]] RenderDspSnapshot createSnapshot() const {
        RenderDspSnapshot s;
        s.enabled = m_enabled;

        // 1. PEQ
        s.peqEnabled = m_peq.isEnabled();
        for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
            s.peqLeftBands[b] = m_peq.leftBand(b);
            s.peqRightBands[b] = m_peq.rightBand(b);
        }

        // 2. GEQ
        s.geqEnabled = m_geq.isEnabled();
        for (size_t b = 0; b < 32; ++b) {
            s.geqGains[b] = m_geq.bandGain(b);
            s.geqBandEnabled[b] = m_geq.isBandEnabled(b);
        }

        // 3. Filter
        s.hpfEnabled = m_filter.isHpfEnabled();
        s.hpfFrequency = m_filter.hpfFrequency();
        s.hpfSlope = m_filter.hpfSlope();
        s.hpfQ = m_filter.hpfQ();
        s.lpfEnabled = m_filter.isLpfEnabled();
        s.lpfFrequency = m_filter.lpfFrequency();
        s.lpfSlope = m_filter.lpfSlope();
        s.lpfQ = m_filter.lpfQ();

        // 4. Compressor
        s.compEnabled = m_comp.isEnabled();
        s.compThresholdDb = m_comp.thresholdDb();
        s.compRatio = m_comp.ratio();
        s.compAttackMs = m_comp.attackMs();
        s.compReleaseMs = m_comp.releaseMs();
        s.compMakeupGainDb = m_comp.makeupGainDb();
        s.compKneeDb = m_comp.kneeDb();

        // 5. Reverb
        s.reverbEnabled = m_reverb.isEnabled();
        s.reverbRoomSize = m_reverb.roomSize();
        s.reverbDamp = m_reverb.damping();
        s.reverbWetLevel = m_reverb.wetLevel();
        s.reverbPredelayMs = m_reverb.predelayMs();
        s.reverbTailPreserving = m_reverb.isTailPreserving();

        // 6. Peak Limiter
        s.limiterEnabled = m_limiter.isEnabled();
        s.limiterCeilingDb = m_limiter.ceilingDb();
        s.limiterReleaseMs = m_limiter.releaseMs();
        s.limiterLookaheadMs = m_limiter.lookaheadMs();

        s.externalInsertWasActiveInLive = m_externalInsertWasActiveInLive;
        s.externalInsertNodeName = m_externalInsertNodeName;

        return s;
    }

    /// Apply an immutable parameter snapshot configured for target sample rate (P0-03)
    void applySnapshot(const RenderDspSnapshot& s, double sampleRate) {
        configure(sampleRate);
        m_enabled = s.enabled;

        // 1. PEQ
        m_peq.setEnabled(s.peqEnabled);
        for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
            m_peq.setLeftBand(b, s.peqLeftBands[b]);
            m_peq.setRightBand(b, s.peqRightBands[b]);
        }

        // 2. GEQ
        m_geq.setEnabled(s.geqEnabled);
        for (size_t b = 0; b < 32; ++b) {
            m_geq.setBandEnabled(b, s.geqBandEnabled[b]);
            m_geq.setBandGain(b, s.geqGains[b]);
        }

        // 3. Filter
        m_filter.setHpfFrequency(s.hpfFrequency);
        m_filter.setHpfSlope(s.hpfSlope);
        m_filter.setHpfQ(s.hpfQ);
        m_filter.setHpfEnabled(s.hpfEnabled);
        m_filter.setLpfFrequency(s.lpfFrequency);
        m_filter.setLpfSlope(s.lpfSlope);
        m_filter.setLpfQ(s.lpfQ);
        m_filter.setLpfEnabled(s.lpfEnabled);

        // 4. Compressor
        m_comp.configure(sampleRate, s.compThresholdDb, s.compRatio, s.compAttackMs, s.compReleaseMs, s.compMakeupGainDb, s.compKneeDb);
        m_comp.setEnabled(s.compEnabled);

        // 5. Reverb
        m_reverb.configure(sampleRate, s.reverbRoomSize, s.reverbDamp, s.reverbWetLevel, s.reverbPredelayMs, s.reverbTailPreserving);
        m_reverb.setEnabled(s.reverbEnabled);

        // 6. Peak Limiter
        m_limiter.configure(sampleRate, s.limiterCeilingDb, s.limiterReleaseMs, s.limiterLookaheadMs);
        m_limiter.setEnabled(s.limiterEnabled);
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
    bool m_externalInsertWasActiveInLive = false;
    std::string m_externalInsertNodeName;

    ParametricEQ m_peq;
    GraphicEQ m_geq;
    HighLowPassFilter m_filter;
    Compressor m_comp;
    PeakLimiter m_limiter;
    AlgorithmicReverb m_reverb;
};

} // namespace desk32::engine::render
